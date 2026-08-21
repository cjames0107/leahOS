"""What an application remembers, and the keys that work wherever you are.

Geometry across two runs in one boot, a shortcut that is not in any
application's own handler, and Tab moving between fields.
"""
import sys, os, time
sys.path.insert(0, os.path.join(os.getcwd(), "tools/vm"))
from machine import main, Failure


def body(t):
    # Open Write somewhere specific, then close it. Its size is remembered.
    t.m.type("/Applications/Write.app/write 60 60 &\n")
    time.sleep(9)
    # Resize by dragging the bottom-right corner.
    t.m.cmd("mouse_move 780 590", 0.2)
    t.m.cmd("mouse_button 1", 0.2)
    t.m.cmd("mouse_move 640 420", 0.3)
    t.m.cmd("mouse_button 0", 0.3)
    time.sleep(2)
    t.m.shot("rem-resized")

    # Ctrl+S with nothing typed: the framework's own, no handler in write.
    t.m.key("ctrl-s")
    time.sleep(3)
    t.m.shot("rem-saved")

    # Escape the sheet, then close the window.
    t.m.key("esc")
    time.sleep(2)
    t.m.click(75, 74)
    time.sleep(3)

    # And again: it should come back the size it was left.
    t.m.click(200, 690)
    time.sleep(2)
    t.m.type("/Applications/Write.app/write &\n")
    time.sleep(9)
    t.m.shot("rem-reopened")

    # Tab between the toolbar's controls.
    t.m.key("tab")
    time.sleep(1)
    t.m.key("tab")
    time.sleep(2)
    t.m.shot("rem-tabbed")

    # Back to the terminal: the window that just opened has the keyboard, and
    # a command typed into a text editor is a command that does not run.
    t.m.click(200, 690)
    time.sleep(2)
    was = len(t.m.serial())
    t.m.type("cat /root/.config/Write.conf > /dev/console\n")
    time.sleep(3)
    conf = t.m.serial()[was:]
    print("Write.conf:\n%s" % conf.strip())
    if "window.w" not in conf:
        raise Failure("no geometry was remembered:\n%s" % conf)
    t.checks += 1

main("remembers", body)
