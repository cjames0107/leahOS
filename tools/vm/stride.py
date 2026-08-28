"""Windows that draw into their own buffer, and the stride they must use.

A window's buffer is allocated with room to spare so a resize can change what
is shown without allocating anything, which puts its rows round_up(width)
apart - 512 for a window 300 wide. Two things went wrong with that.

A client drawing straight into the buffer stepped by its width, so every row
landed a little past the one before and the server, which reads at the stride,
showed the window sheared into bands. Calculator is 300 wide and did exactly
that. The desktop did too and got away with it, because it is the width of the
screen and 1024 happens to be a multiple of 256.

And the compositor read the stride live out of shared memory on every row,
while the width, the height and the size of the mapping it had checked them
against were all frozen. A client publishes its new, larger stride just before
it publishes the new segment - deliberately, so the server cannot see a new
generation described by an old size - which left a window where the server
composited the old, smaller buffer at the new, larger stride: wrong rows, and
then off the end of the mapping.

So: a window whose width is not a multiple of 256, drawn and looked at, and a
window resized hard enough to replace its buffer repeatedly.
"""
import sys, os, time
sys.path.insert(0, os.path.join(os.getcwd(), "tools/vm"))
from machine import main, Failure


def body(t):
    # 300 wide: round_up(300) is 512, so the stride is not the width.
    t.m.type("/Applications/Calculator.app/calc 60 60 &\n")
    time.sleep(7)
    t.m.shot("st-calc")

    # A window drawn at the wrong pitch has its lower half made of stripes.
    # The shot is the check - these are screenshots for a person to look at -
    # and pressing a key gives it something to have changed.
    t.m.click(174, 382)                 # the 2 key
    time.sleep(1)
    t.m.shot("st-calc-clicked")

    # Back to the terminal before typing, or the launch below is typed into
    # whatever was clicked last - which is the calculator, and it reads the
    # command as a very long sum.
    t.m.click(400, 600)
    time.sleep(1)

    # Now the resize path, which is where the compositor died. Write is on the
    # framework and resizes by reallocating once its slack runs out, so growing
    # it well past its original width replaces the segment several times.
    t.m.type("/Applications/Write.app/write 40 40 &\n")
    time.sleep(9)
    for i in range(3):
        t.m.drag(756, 586, 430, 400, steps=6)
        time.sleep(1)
        t.m.drag(430, 400, 900, 700, steps=6)
        time.sleep(1)
    t.m.shot("st-resized")

    stray = t.faults()
    if stray:
        raise Failure("the compositor did not survive: %s" % stray[0])
    t.checks += 1


main("stride", body)
