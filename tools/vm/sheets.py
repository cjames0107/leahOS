"""The panels that used to be drawn into the window behind them.

Files asks through sheets now - rename and open-with - and the desktop asks
its own. All of them were the old file dialogue, which painted over whatever
was underneath and is gone.
"""
import sys, os, time
sys.path.insert(0, os.path.join(os.getcwd(), "tools/vm"))
from machine import main, Failure

def body(t):
    t.m.type("/Applications/Files.app/browse 40 40 &\n")
    time.sleep(9)

    # Choose something, then File > Rename.
    t.m.click(230, 130)         # the "lost+found" icon
    time.sleep(2)
    t.m.click(671, 85)          # the File menu
    time.sleep(2)
    t.m.click(700, 135)         # "Rename"
    time.sleep(3)
    t.m.shot("sheet-rename")
    t.checks += 1

    # It is a window of its own, so what is under it must survive being
    # covered: dismiss it and look.
    t.m.key("esc")
    time.sleep(3)
    t.m.shot("sheet-dismissed")
    t.checks += 1

main("sheets", body)
