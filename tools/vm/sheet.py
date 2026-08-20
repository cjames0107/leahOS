"""Paint's save sheet, end to end.

The dialogue it replaces was drawn into Paint's own window, and Paint's window
*is* the picture - there is no model behind the canvas - so the dialogue
destroyed the part of the picture it covered and then saved the hole. This
checks the two things that fixes: that the sheet appears at all, and that its
answer reaches the disk.
"""
import sys, os, time
sys.path.insert(0, os.path.join(os.getcwd(), "tools/vm"))
from machine import main, Failure

def body(t):
    t.m.type("/Applications/Paint.app/paint 40 40 &\n")
    time.sleep(8)
    t.m.click(300, 400)
    t.m.move_to(600, 480)
    time.sleep(1)
    t.m.type("p")               # save as PNG -> the sheet
    time.sleep(3)
    t.m.type("\n")              # Return in the field is Save
    time.sleep(6)
    # Back to the shell to look. Click the terminal first.
    t.m.click(200, 690)
    time.sleep(2)
    t.expect("ls -l /picture.png", "picture.png")
    t.checks += 1

main("sheet2", body)
