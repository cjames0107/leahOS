"""Drive a leahOS machine headlessly, for tests.

Everything here talks to QEMU rather than to the guest: keys go in through the
monitor, and answers come back on the serial line. That asymmetry is the whole
design. A screenshot needs a person to look at it, so a test that takes one
proves nothing on its own; the serial console is text, and text can be asserted
against.

The guest writes to the serial line by writing to /dev/console, which is why
the device nodes had to become real before this was worth building.
"""

import os
import socket
import subprocess
import time

import pathlib
ROOT = str(pathlib.Path(__file__).resolve().parents[2])
SCRATCH = os.path.dirname(os.path.abspath(__file__))
SOCK = os.environ.get("LEAH_MONITOR", "/tmp/leah-monitor.sock")
LOG = os.environ.get("LEAH_SERIAL", "/tmp/leah-serial.log")

KEYMAP = {
    ' ': 'spc', '\n': 'ret', '.': 'dot', '/': 'slash', '-': 'minus',
    ',': 'comma', ';': 'semicolon', "'": 'apostrophe', '=': 'equal',
    '[': 'bracket_left', ']': 'bracket_right', '\\': 'backslash',
    '`': 'grave_accent', ':': 'shift-semicolon', '_': 'shift-minus',
    '>': 'shift-dot', '<': 'shift-comma', '|': 'shift-backslash',
    '?': 'shift-slash', '*': 'shift-8', '$': 'shift-4', '&': 'shift-7',
    '#': 'shift-3', '%': 'shift-5', '!': 'shift-1', '"': 'shift-apostrophe',
    '+': 'shift-equal', '(': 'shift-9', ')': 'shift-0', '~': 'shift-grave_accent',
    '{': 'shift-bracket_left', '}': 'shift-bracket_right', '@': 'shift-2',
    '^': 'shift-6',
}


class Machine:
    def __init__(self, cpus=1, mem="512M"):
        for p in (SOCK, LOG):
            if os.path.exists(p):
                os.remove(p)
        self.proc = subprocess.Popen([
            "qemu-system-x86_64", "-machine", "pc,hpet=on",
            "-drive", f"format=raw,file={ROOT}/build/dist/leahos.img,if=ide",
            "-drive", f"format=raw,file={ROOT}/build/dist/ext.img,if=ide",
            "-drive", f"format=raw,file={ROOT}/build/dist/mnt.img,if=ide",
            "-snapshot",
            "-netdev", "user,id=net0", "-device", "e1000,netdev=net0",
            "-audiodev", "wav,id=snd0,path=/tmp/leah-audio.wav,"
                         "out.frequency=48000,out.channels=2,out.format=s16",
            "-device", "AC97,audiodev=snd0",
            # The same controllers `make run` gives it. A harness that boots a
            # different machine from the one people use is checking something
            # nobody runs - which is how the SATA controller came to be absent
            # from every check while being present in every real boot.
            "-device", "ahci,id=sata0",
            "-drive", f"format=raw,file={ROOT}/build/dist/sata.img,if=none,id=satadisk",
            "-device", "ide-hd,drive=satadisk,bus=sata0.0",
            "-m", mem, "-smp", str(cpus), "-display", "none",
            "-serial", f"file:{LOG}",
            "-monitor", f"unix:{SOCK},server,nowait",
        ])
        for _ in range(200):
            if os.path.exists(SOCK):
                break
            time.sleep(0.05)
        self.mon = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        for _ in range(100):
            try:
                self.mon.connect(SOCK)
                break
            except OSError:
                time.sleep(0.05)
        self.mon.setblocking(False)
        time.sleep(0.3)
        self.drain()

    def drain(self):
        try:
            while self.mon.recv(65536):
                pass
        except OSError:
            pass

    def cmd(self, text, settle=0.06):
        self.mon.sendall((text + "\n").encode())
        time.sleep(settle)
        self.drain()

    def key(self, k, settle=0.05):
        self.cmd(f"sendkey {k}", settle)

    def type(self, text, settle=0.05):
        for ch in text:
            if ch in KEYMAP:
                self.key(KEYMAP[ch], settle)
            elif ch.isupper():
                self.key(f"shift-{ch.lower()}", settle)
            else:
                self.key(ch, settle)

    def move_to(self, x, y):
        """Walk into the corner to pin the pointer, then step over."""
        for _ in range(12):
            self.cmd("mouse_move -100 -100", 0.06)
        at_x = at_y = 0
        while at_x < x or at_y < y:
            dx = min(40, x - at_x)
            dy = min(40, y - at_y)
            # 0.12s, not 0.05: at the faster rate the monitor drops moves.
            self.cmd(f"mouse_move {dx} {dy}", 0.12)
            at_x += dx
            at_y += dy

    def click(self, x, y, double=False):
        self.move_to(x, y)
        time.sleep(0.15)
        for _ in (range(2) if double else range(1)):
            self.cmd("mouse_button 1", 0.06)
            self.cmd("mouse_button 0", 0.12)

    def shot(self, name):
        ppm = f"/tmp/{name}.ppm"
        self.cmd(f"screendump {ppm}", 0.7)
        png = f"{SCRATCH}/{name}.png"
        subprocess.run(["sips", "-s", "format", "png", ppm, "--out", png],
                       capture_output=True)
        print(f"  shot -> {png}")
        return png

    def serial(self):
        try:
            with open(LOG, "r", errors="replace") as f:
                return f.read()
        except OSError:
            return ""

    def wait_for_serial(self, needle, timeout=60):
        end = time.time() + timeout
        while time.time() < end:
            if needle in self.serial():
                return True
            time.sleep(0.5)
        raise TimeoutError(f"never saw {needle!r} on the serial console")

    def stop(self):
        try:
            self.cmd("quit", 0.2)
        except OSError:
            pass
        try:
            self.proc.wait(timeout=10)
        except Exception:
            self.proc.kill()


# --- what a test is made of --------------------------------------------------

class Failure(Exception):
    """A check that did not hold. Carries its own message; the runner prints
    it and exits non-zero, which is the whole point of the harness."""


class Test:
    """A booted machine, logged in, with a shell that reports to the serial
    line rather than to a screen nobody is watching."""

    def __init__(self, cpus=2, boot_timeout=420):
        start = time.time()
        self.m = Machine(cpus=cpus)
        self.checks = 0
        # Markers are counted separately from checks. They were the same
        # number until a check that runs no command was added, and then the
        # slice below started looking for a marker nobody had printed - which
        # reads as the last command having failed when it had passed.
        self.marks = 0
        self.allowed = []
        self.m.wait_for_serial("login", boot_timeout)
        # Worth recording rather than asserting: it moves with the host's load
        # as much as with the guest's code, so it is a number to watch and not
        # a threshold to fail on.
        self.boot_seconds = time.time() - start
        time.sleep(3)
        self.m.type("root\n"); time.sleep(0.5); self.m.type("toor\n")
        time.sleep(55)
        # The terminal, below every other window, so the click cannot land on
        # something that has opened over it.
        self.m.click(200, 690); time.sleep(3)

    def run(self, command, timeout=90):
        """Run a command and wait for it to finish.

        Output goes to /dev/console, which is the serial line, so the harness
        can read it. The marker is what makes "finished" observable - without
        it there is no way to tell a slow command from a hung one."""
        mark = "==done-%d==" % self.marks
        self.marks += 1
        # No 2>&1: this shell does not join the streams, and writing it puts
        # a stray "1" in the output rather than an error message in it.
        self.m.type("%s > /dev/console; echo %s > /dev/console\n"
                    % (command, mark))
        end = time.time() + timeout
        while time.time() < end:
            if mark in self.m.serial():
                return self.m.serial()
            time.sleep(0.5)
        raise Failure("timed out waiting for: %s" % command)

    def expect(self, command, needle, timeout=90):
        out = self.run(command, timeout)
        self.checks += 1
        # Everything since the previous command finished.
        tail = out[out.rfind("==done-%d==" % (self.marks - 2)):] \
            if self.marks > 1 else out
        if needle not in tail:
            raise Failure("%r did not appear after: %s" % (needle, command))

    def allow_fault(self, pattern, why):
        """Say that a fault matching `pattern` is meant to happen.

        Every other one fails the run. A test that deliberately makes a
        process die has to say so here, which means the list doubles as the
        record of which crashes this system expects to see."""
        self.allowed.append((pattern, why))

    def faults(self):
        """Faults and panics nobody asked for."""
        out = []
        for line in self.m.serial().splitlines():
            low = line.lower()
            if "faulted" not in low and "panic" not in low:
                continue
            if any(p in line for p, _ in self.allowed):
                continue
            out.append(line.strip())
        return out

    def stop(self):
        self.m.stop()


def main(name, body):
    """Run one test, print a line, and set the exit status."""
    import sys
    t = None
    try:
        t = Test()
        body(t)
    except Failure as e:
        print("FAIL  %s: %s" % (name, e))
        if t: t.stop()
        sys.exit(1)
    except Exception as e:                       # a broken harness, not a
        print("ERROR %s: %s" % (name, e))        # broken guest
        if t: t.stop()
        sys.exit(2)
    # A run can pass every assertion and still have killed something on the
    # way through. Demand paging landed with two faults in the log and a green
    # result, which is how this check came to exist.
    stray = t.faults()
    if stray:
        print("FAIL  %s: %d unexpected fault(s)" % (name, len(stray)))
        for line in stray[:8]:
            print("        %s" % line)
        t.stop()
        sys.exit(1)

    print("ok    %s (%d checks, %d expected fault(s), boot %.1fs)"
          % (name, t.checks, len(t.allowed), t.boot_seconds))
    t.stop()
    sys.exit(0)
