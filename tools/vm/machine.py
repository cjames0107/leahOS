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

# One serial log and one monitor socket per machine, not per harness.
#
# They used to be two fixed paths. That is fine while exactly one machine is
# ever running, and a check that boots a machine per application is not that: a
# machine that has been told to quit is still alive for a moment, and for that
# moment two of them are writing the same serial file and answering on the same
# socket name. The next check then reads a log two machines are appending to
# and drives whichever one happened to bind - so it waits for a login prompt
# that already went past, or types into a machine it is not looking at.
#
# The symptom was an application "not starting" and a boot that "stalled",
# neither of which had anything to do with the guest. The environment variables
# still name the first machine's paths, so `LEAH_SERIAL=... make headless` and
# anything else pointing at them keeps working.
_next_id = [0]


def _paths():
    n = _next_id[0]
    _next_id[0] += 1
    if n == 0:
        return SOCK, LOG, SOCK + ".qmp"
    return ("%s.%d" % (SOCK, n), "%s.%d" % (LOG, n), "%s.qmp.%d" % (SOCK, n))

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
        self.sock, self.log, self.qmp_path = _paths()
        for p in (self.sock, self.log, self.qmp_path):
            if os.path.exists(p):
                os.remove(p)
        # The machine comes from the Makefile, not from a copy kept here.
        # Two bugs came out of keeping a copy: the harness spent weeks booting
        # a machine with no SATA controller, and then passed every check while
        # `make run` had no root disk at all because a line continuation had
        # been broken. A test that boots a different machine from the one
        # people use is testing something nobody runs.
        # Memory, processors and the sound backend are the machine's, so they
        # are asked for through make rather than added afterwards - passing
        # them twice is how you get a QEMU that refuses to start. A test wants
        # no audio device at all, which is what "none" is for.
        flags = subprocess.run(
            ["make", "-s", "print-qemuflags",
             "AUDIODEV=none", "MEM=%s" % mem, "CPUS=%d" % cpus],
            cwd=ROOT, capture_output=True, text=True).stdout.split()
        if not flags:
            raise RuntimeError("make print-qemuflags said nothing")

        self.proc = subprocess.Popen([
            "qemu-system-x86_64", *flags,
            # What a test needs on top: no window, the console somewhere
            # readable, a monitor to drive it through, and a disk that forgets
            # everything when the machine stops.
            "-snapshot",
            "-display", "none",
            "-serial", f"file:{self.log}",
            "-monitor", f"unix:{self.sock},server,nowait",
            # A second monitor, speaking QMP.
            #
            # The human monitor cannot send a scroll wheel: its mouse_button
            # takes 1, 2 and 4 for the three buttons and has no bits for a
            # wheel, and input_send_event is not one of its commands. QMP has
            # input-send-event, which does.
            "-qmp", f"unix:{self.qmp_path},server,nowait",
        ], cwd=ROOT)     # the Makefile names its images relatively
        for _ in range(200):
            if os.path.exists(self.sock):
                break
            time.sleep(0.05)
        self.mon = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        for _ in range(100):
            try:
                self.mon.connect(self.sock)
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

    def drag(self, from_x, from_y, to_x, to_y, steps=8, hold=None):
        """Press at one point, walk to another, and let go.

        The moves are relative - that is what the monitor's mouse_move is, and
        passing it a destination sends the pointer somewhere else entirely -
        so the walk is done in steps from where the press happened. `hold` is
        called part way through, with the button still down, for anything that
        wants to see what is happening rather than what it ended as."""
        self.move_to(from_x, from_y)
        time.sleep(0.15)
        self.cmd("mouse_button 1", 0.12)
        at_x, at_y = from_x, from_y
        for i in range(1, steps + 1):
            want_x = from_x + (to_x - from_x) * i // steps
            want_y = from_y + (to_y - from_y) * i // steps
            self.cmd("mouse_move %d %d" % (want_x - at_x, want_y - at_y), 0.12)
            at_x, at_y = want_x, want_y
            if hold is not None and i == steps // 2:
                hold()
        time.sleep(0.15)
        self.cmd("mouse_button 0", 0.12)

    def click(self, x, y, double=False):
        self.move_to(x, y)
        time.sleep(0.15)
        for _ in (range(2) if double else range(1)):
            self.cmd("mouse_button 1", 0.06)
            self.cmd("mouse_button 0", 0.12)

    def _qmp(self, command, arguments=None):
        """One QMP command, with the capabilities handshake done on demand."""
        import json
        if getattr(self, "qmp", None) is None:
            self.qmp = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
            for _ in range(100):
                try:
                    self.qmp.connect(self.qmp_path)
                    break
                except OSError:
                    time.sleep(0.05)
            self.qmp.recv(65536)                    # the greeting
            self.qmp.sendall(b'{"execute":"qmp_capabilities"}\n')
            self.qmp.recv(65536)                    # its answer
        message = {"execute": command}
        if arguments is not None:
            message["arguments"] = arguments
        self.qmp.sendall((json.dumps(message) + "\n").encode())
        time.sleep(0.05)
        try:
            return self.qmp.recv(65536)
        except OSError:
            return b""

    def wheel(self, x, y, notches):
        """Turn the wheel over a point. Positive is downwards.

        QEMU sends a wheel as buttons 8 (up) and 16 (down) in the same
        mouse_button mask the physical buttons use, which is how a PS/2 mouse
        with the IntelliMouse extension reports it."""
        self.move_to(x, y)
        time.sleep(0.15)
        button = "wheel-down" if notches > 0 else "wheel-up"
        for _ in range(abs(notches)):
            self._qmp("input-send-event", {"events": [
                {"type": "btn", "data": {"down": True,  "button": button}},
                {"type": "btn", "data": {"down": False, "button": button}},
            ]})
            time.sleep(0.08)

    def shot(self, name):
        ppm = f"/tmp/{name}.ppm"
        self.cmd(f"screendump {ppm}", 0.7)
        png = f"{SCRATCH}/{name}.png"
        subprocess.run(["sips", "-s", "format", "png", ppm, "--out", png],
                       capture_output=True)
        print(f"  shot -> {png}")
        return png

    def screen_colours(self, name="probe"):
        """How many distinct colours are on the screen.

        The checks drive the terminal by keystroke and read the answers off the
        serial line, which means a machine whose display never draws anything
        passes every one of them. A blank screen is the one failure this suite
        was structurally unable to see, so it samples the framebuffer and
        counts: a desktop has hundreds of colours in it, and a dead one has
        one."""
        ppm = "/tmp/%s.ppm" % name
        self.cmd("screendump %s" % ppm, 1.0)
        try:
            with open(ppm, "rb") as f:
                data = f.read()
        except OSError:
            return 0
        # P6 header: magic, width, height, maxval - each possibly on its own
        # line, with comments allowed between.
        at, fields = 2, []
        while len(fields) < 3 and at < len(data):
            while at < len(data) and data[at:at + 1].isspace():
                at += 1
            if data[at:at + 1] == b"#":
                while at < len(data) and data[at] != 0x0A:
                    at += 1
                continue
            start = at
            while at < len(data) and not data[at:at + 1].isspace():
                at += 1
            fields.append(int(data[start:at]))
        at += 1
        seen = set()
        for i in range(at, len(data) - 3, 997 * 3):      # a scattered sample
            seen.add(data[i:i + 3])
        return len(seen)

    def screen_signature(self, name="sig"):
        """A number that changes when the screen does.

        Counting colours answers "is anything drawn at all", and that is all it
        answers. It cannot see a window that opened over a colourful wallpaper,
        because covering colour with a flat panel makes the count go *down* -
        which reads as nothing having been drawn.

        What a GUI check actually wants to ask is "did the screen change when I
        did that", and for a click on a component that is the whole assertion:
        the tree routed the event, something changed, and it was repainted.
        """
        ppm = "/tmp/%s.ppm" % name
        self.cmd("screendump %s" % ppm, 1.0)
        try:
            with open(ppm, "rb") as f:
                data = f.read()
        except OSError:
            return 0
        total = 0
        for i in range(0, len(data) - 3, 613):
            total = (total * 131 + data[i]) & 0xFFFFFFFF
        return total

    def serial(self):
        try:
            with open(self.log, "r", errors="replace") as f:
                return f.read()
        except OSError:
            return ""

    def wait_for_serial(self, needle, timeout=60):
        end = time.time() + timeout
        while time.time() < end:
            if needle in self.serial():
                return True
            time.sleep(0.5)
        # With the tail of the log, because without it this says only that the
        # machine did not finish and leaves the one question that matters -
        # where it stopped - to be answered by running it again and hoping it
        # fails the same way. An intermittent boot stall was pinned for weeks
        # on the strength of this message alone; the line it stopped after was
        # in the log the whole time and thrown away here.
        tail = self.serial().strip().splitlines()[-25:]
        raise TimeoutError(
            "never saw %r on the serial console; it stopped after:\n%s"
            % (needle, "\n".join("    " + line for line in tail)))

    def stop(self):
        try:
            self.cmd("quit", 0.2)
        except OSError:
            pass
        try:
            self.proc.wait(timeout=10)
        except Exception:
            self.proc.kill()
        # And wait for the kill to land. A run that boots a machine per check
        # otherwise hands the next one a host still running the last, and two
        # emulators without acceleration on this host do not take twice as long
        # each - they take much longer, and the run reads as a hang.
        try:
            self.proc.wait(timeout=15)
        except Exception:
            pass
        for p in (self.sock, self.qmp_path):
            try:
                os.remove(p)
            except OSError:
                pass


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
        # Find the terminal, rather than assuming a click landed on it.
        #
        # Clicking a fixed point and typing was the single biggest source of
        # lost runs in this harness: a window opens over the spot, or the
        # desktop has not finished arranging itself, and every keystroke goes
        # somewhere that is not listening. The failure looks exactly like a
        # broken system - the machine boots, draws, faults nowhere, and answers
        # nothing - which sent me diagnosing the keyboard driver for a fault
        # that was in this file.
        #
        # So it asks. The shell echoes to the serial line, which is the one
        # place this can read, and a shell that answers is a shell that has the
        # keys.
        for attempt in range(6):
            self.m.click(200, 690)
            time.sleep(3)
            mark = "==awake-%d==" % attempt
            self.m.type("echo %s > /dev/console\n" % mark)
            for _ in range(20):
                if mark in self.m.serial():
                    return
                time.sleep(0.5)
        raise Failure("no window took the keyboard after six tries")

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


def keep_binaries(name, log=LOG):
    """Copy the ELFs aside when a run has faulted.

    A user fault names an address, and turning an address back into a line
    needs *the binary that faulted*. Twice now that binary had been rebuilt
    before anyone looked, and the second time the wrong disassembly sent the
    investigation a fortnight in the wrong direction - "the stack pointer is
    gone" from an instruction that was not the one which ran.

    The build directory is rewritten constantly and the fault is rare, so the
    binaries are kept at the moment of the failure rather than hoped for
    afterwards.
    """
    try:
        with open(log, "r", errors="replace") as f:
            text = f.read()
        if "faulted" not in text and "stack smashed" not in text:
            return
    except OSError:
        return
    keep = os.path.join(ROOT, "build", "faulted-%s" % name)
    try:
        os.makedirs(keep, exist_ok=True)
        import glob, shutil
        for elf in glob.glob(os.path.join(ROOT, "build", "*.elf")):
            shutil.copy2(elf, keep)
        shutil.copy2(log, os.path.join(keep, "serial.log"))
        print("  faulted: binaries kept in %s" % keep)
    except OSError:
        pass


def main(name, body):
    """Run one test, print a line, and set the exit status."""
    import sys
    t = None
    try:
        t = Test()
        body(t)
    except Failure as e:
        print("FAIL  %s: %s" % (name, e))
        keep_binaries(name, t.m.log if t else LOG)
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
        keep_binaries(name, t.m.log)
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
