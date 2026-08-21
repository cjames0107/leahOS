"""Work is not thrown away without being mentioned.

Paint could be closed with an unsaved picture and the picture was gone. Write
kept its own edited flag that nothing acted on. Both are documents now.
"""
import sys, os, time
sys.path.insert(0, os.path.join(os.getcwd(), "tools/vm"))
from machine import Test, Failure


def check(name, launch, work, close_at):
    t = Test()
    try:
        t.m.type(launch + "\n")
        time.sleep(9)
        work(t)
        time.sleep(2)
        t.m.shot("doc-%s-edited" % name)
        # The close box.
        t.m.click(close_at[0], close_at[1])
        time.sleep(3)
        t.m.shot("doc-%s-closing" % name)
        stray = t.faults()
        if stray:
            raise Failure("%s: %s" % (name, stray[0]))
    finally:
        t.stop()


def draw(t):
    t.m.click(300, 300)
    time.sleep(1)
    t.m.click(340, 340)


def typing(t):
    t.m.type("some words")


check("paint", "/Applications/Paint.app/paint 40 40", draw, (55, 55))
check("write", "/Applications/Write.app/write 40 40", typing, (55, 55))
print("ok    documents")
