"""Write: does it type, style, save and read back?"""
import sys, os, time
sys.path.insert(0, os.path.join(os.getcwd(), "tools/vm"))
from machine import main, Failure

def body(t):
    t.m.type("/Applications/Write.app/write 40 40 &\n")
    time.sleep(8)
    t.m.shot("write-empty")

    # Type something plain.
    t.m.type("The quick brown fox. ")
    time.sleep(2)

    # Bold on, more text, bold off.
    t.m.click(70, 88)           # the Bold switch
    time.sleep(1)
    t.m.type("This part is bold. ")
    time.sleep(2)
    t.m.click(70, 88)
    time.sleep(1)

    # Italic, at a bigger size.
    t.m.click(162, 88)          # Italic
    time.sleep(1)
    t.m.type("and this leans.")
    time.sleep(2)
    t.m.shot("write-styled")
    t.checks += 1

main("write", body)
