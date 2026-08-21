"""A text field that can actually be edited.

Selection, the clipboard and undo, in a field - none of which existed before.
"""
import sys, os, time
sys.path.insert(0, os.path.join(os.getcwd(), "tools/vm"))
from machine import main

def body(t):
    t.m.type("/Applications/Files.app/browse 40 40 &\n")
    time.sleep(9)

    # Type into the search field.
    t.m.click(566, 86)
    time.sleep(1)
    t.m.type("hello world")
    time.sleep(2)
    t.m.shot("te-typed")

    # Shift+ctrl+left selects the last word.
    t.m.key("shift-ctrl-left")
    time.sleep(2)
    t.m.shot("te-selected")

    # Undo the whole run of typing in one step.
    t.m.key("ctrl-z")
    time.sleep(2)
    t.m.shot("te-undone")
    t.checks += 1

main("textedit", body)
