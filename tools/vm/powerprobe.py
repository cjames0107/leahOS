"""The three power buttons, which stop or restart the machine.

Driven rather than reasoned about: a reset written to the wrong port does
nothing at all and looks exactly like a button that was never wired up.

One machine each, because both actions end the session they were pressed in -
a reboot comes back to the login screen, where there is no panel left to press
the other button on.

Both need a machine that is allowed to obey: everything else here boots with
-no-reboot -no-shutdown, which turn a triple fault into a stopped machine
somebody can inspect and turn a working shutdown into an indistinguishable
nothing. See may_reset in machine.py.
"""
import sys, os, time
sys.path.insert(0, os.path.join(os.getcwd(), "tools/vm"))
from machine import Test, Failure

# Offsets into the panel, which opens at (8, 38) under the bar's left button.
# Three buttons 34 across with 16 between them, centred in 196.
POWER_Y  = 38 + 12 + 18 + 3 * 24 + 12 + 18 + 17
SHUT_X   = 8 + (196 - (34 * 3 + 16 * 2)) // 2 + 17
REBOOT_X = SHUT_X + 34 + 16


def open_panel(t):
    t.m.click(21, 17)
    time.sleep(2)


def check(name, press, verify):
    t = Test(may_reset=True)
    try:
        time.sleep(4)
        open_panel(t)
        t.m.shot("pw-%s" % name)
        press(t)
        verify(t)
        print("  ok    %s" % name)
    finally:
        try:
            t.stop()
        except Exception:
            pass


def shut_down(t):
    # The machine can go away between the press and the release, which is the
    # success case arriving faster than the click finishes - so a monitor that
    # stops answering here is not an error, it is the answer.
    try:
        t.m.click(SHUT_X, POWER_Y)
    except OSError:
        pass


def stopped(t):
    deadline = time.time() + 40
    while time.time() < deadline and t.m.proc.poll() is None:
        time.sleep(1)
    if t.m.proc.poll() is None:
        raise Failure("shut down did not stop the machine")


def restart(t):
    t.m.click(REBOOT_X, POWER_Y)


def came_back(t):
    """Two boot banners: the one it started with and the one it came back on."""
    deadline = time.time() + 90
    while time.time() < deadline:
        if t.m.serial().count("starting init") > 1:
            return
        time.sleep(2)
    raise Failure("reboot did not restart the machine")


check("shutdown", shut_down, stopped)
check("reboot", restart, came_back)
print("ok    power (2 checks)")
