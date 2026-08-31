"""vi, driven the way a person drives it.

There was no way to edit a file without the window server: Edit is a window,
and if the desktop does not come up there was nothing to fix the file that was
stopping it. This is that gap closed, and the checks are the sequence somebody
actually types - open, change something, write, quit - followed by reading the
file back to see whether the change is in it.

Every key goes through the pty's raw mode, which is what an editor needs and
what the terminal driver is for. So this exercises that too: an editor is the
program termios exists for.
"""
import sys, os, time
sys.path.insert(0, os.path.join(os.getcwd(), "tools/vm"))
from machine import main, Failure


def keys(t, text, pause=0.25):
    """Type into the editor. Slowly: each key is read one at a time and drawn
    before the next, and the emulator is not fast."""
    for ch in text:
        t.m.type(ch)
        time.sleep(pause)


def body(t):
    # A file with something in it, made in one command so the harness's own
    # redirect lands on the cat rather than on the file being written.
    t.expect("printf 'alpha\\nbeta\\ngamma\\n' > /tmp/e.txt; cat /tmp/e.txt",
             "gamma")

    # Open it. The status line names the file and counts the lines, which is
    # the first thing that says the editor read it rather than started empty.
    t.m.type("vi /tmp/e.txt\n")
    time.sleep(4)
    t.m.shot("vi-0-opened")

    # Down a line, to the end of it, and append. This is the sequence that
    # proves motion and insert together: landing on the wrong line would put
    # the text somewhere the check below would not find it.
    keys(t, "j")
    keys(t, "A")
    keys(t, "-edited")
    t.m.key("esc")
    time.sleep(0.5)
    t.m.shot("vi-1-edited")

    # Open a line below and type on it.
    keys(t, "o")
    keys(t, "delta")
    t.m.key("esc")
    time.sleep(0.5)

    # Write and leave.
    keys(t, ":")
    keys(t, "wq")
    t.m.key("ret")
    time.sleep(3)
    t.m.shot("vi-2-after")

    # And the file on disk is what was typed.
    t.expect("cat /tmp/e.txt", "beta-edited")
    t.expect("cat /tmp/e.txt", "delta")
    t.expect("wc -l < /tmp/e.txt", "4")
    # The lines it did not touch are untouched.
    t.expect("head -1 /tmp/e.txt", "alpha")

    # dd removes a line, and :q! leaves without writing - so the file still
    # has what it had.
    t.m.type("vi /tmp/e.txt\n")
    time.sleep(4)
    keys(t, "dd")
    keys(t, ":")
    keys(t, "q!")
    t.m.key("ret")
    time.sleep(3)
    t.expect("head -1 /tmp/e.txt", "alpha")

    stray = t.faults()
    if stray:
        raise Failure("%s" % stray[0])
    t.checks += 1


main("editor", body)
