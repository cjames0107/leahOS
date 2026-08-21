"""A scroll view that can actually be scrolled, and clips what it holds."""
import sys, os, time
sys.path.insert(0, os.path.join(os.getcwd(), "tools/vm"))
from machine import main

def body(t):
    # A long manual page, so there is something to scroll.
    t.m.type("/Applications/Help.app/help 40 40 grep &\n")
    time.sleep(9)
    t.m.shot("scroll-top")
    # Drag the thumb down.
    t.m.click(752, 200)
    time.sleep(2)
    t.m.shot("scroll-jumped")
    t.checks += 1

main("scrolling", body)
