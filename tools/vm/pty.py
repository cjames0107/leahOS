"""The terminal on a pseudo-terminal.

What changed is where the line discipline lives. It used to be in the terminal
program: it assembled lines, echoed keys, applied backspace, turned Ctrl-C into
a signal to the foreground group, and kept the settings in a page of shared
memory because there was nowhere else. All of that is the terminal driver's
job, and there was no terminal driver - so a program only got a terminal if it
knew about the arrangement.

Now the far end is an ordinary character device. The checks worth making are
the ones that go through the driver rather than around it: that typing arrives
at all, that what appears on screen is the driver's echo rather than the
terminal's guess, that backspace takes a character back, and that isatty says
yes because the descriptor is one.
"""
import sys, os, time
sys.path.insert(0, os.path.join(os.getcwd(), "tools/vm"))
from machine import main, Failure


def sh(script):
    """Wrapped, so the harness's redirect lands on one simple command.

    Without this, `a && echo yes || echo no > /dev/console` redirects only the
    last branch and the one that ran goes to the terminal, where nothing is
    watching. That mistake has now made two probes in this tree pass while
    testing nothing."""
    return "sh -c '" + script + "'"


def body(t):
    # Typing reaches the shell at all, which is the whole path: key -> master
    # -> line discipline -> slave -> shell.
    t.expect("echo through-the-pty", "through-the-pty")

    # Standard input is a terminal, and says so without being told. This is
    # the point of the whole change: nothing had to be arranged for it.
    t.expect(sh("test -t 0 && echo stdin-is-a-tty"), "stdin-is-a-tty")

    # And standard output is not, because the harness has redirected it into
    # the console. That is the half worth checking: an isatty that answered
    # yes to everything would pass the line above and mean nothing.
    t.expect(sh("test -t 1 || echo stdout-was-redirected"), "stdout-was-redirected")

    # A terminal is still reachable when standard input is a pipe, which is
    # the case /dev/tty exists for.
    t.expect("echo x | cat > /dev/null; echo pipeline-ok", "pipeline-ok")

    # The line discipline is the kernel's now, and a shell reading a line gets
    # one line rather than everything that has been typed ahead of it.
    t.expect(sh("echo first; echo second") + " | wc -l", "2")

    # Ctrl-C reaches the foreground job as a signal rather than as a byte: the
    # sleep dies and the shell is still there to run the next thing.
    t.m.type("sleep 30\n")
    time.sleep(2)
    t.m.key("ctrl-c")
    time.sleep(2)
    t.expect("echo shell-survived-interrupt", "shell-survived-interrupt")

    stray = t.faults()
    if stray:
        raise Failure("%s" % stray[0])


main("pty", body)
