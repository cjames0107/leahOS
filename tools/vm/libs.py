"""The component layer and the CLI layer, exercised rather than inspected."""
import sys, os, time
sys.path.insert(0, os.path.join(os.getcwd(), "tools/vm"))
from machine import main, Failure

def body(t):
    # --- cli.h, through the program ported onto it ---
    # -n and -n<value> are the same option, which is the thing every program
    # used to decide for itself.
    t.expect("head -n 2 /usr/share/doc/notes.txt", "Notes live")
    t.expect("head -n2 /usr/share/doc/hello.txt", "Hello from ext4")
    # A missing file is named by the program, from argv[0], not a literal.
    # Through a file, because cli_fail writes to stderr - which is correct, and
    # which this shell does not send to the serial line.
    t.expect("head /nonexistent 2> /tmp/err; cat /tmp/err", "head:")
    # The option's value is not mistaken for a filename.
    t.expect("head -n 1 /usr/share/doc/hello.txt", "Hello from ext4")

    # --- ui.h, through Elements ---
    # The signature rather than the colour count: a window opening over the
    # wallpaper covers colour, so counting it goes down and reads as nothing
    # having been drawn.
    before = t.m.screen_signature("a")
    t.m.type("/Applications/Elements.app/uitest 30 30 &\n")
    time.sleep(8)
    opened = t.m.screen_signature("b")
    if opened == before:
        raise Failure("the components window did not appear")
    t.checks += 1

    # A click on a sidebar row. If the tree routes the event, the selection
    # moves and the report label changes - so the screen changes. If it does
    # not, nothing happens at all, which is exactly what this catches.
    t.m.click(80, 150)
    time.sleep(2)
    clicked = t.m.screen_signature("c")
    if clicked == opened:
        raise Failure("clicking a component changed nothing on screen")
    t.checks += 1

    # Typing is not asserted from out here. Landing a click on one component
    # rather than its neighbour needs a pixel worked out by hand, and getting
    # that wrong reads as a broken library - it did twice. The keyboard path is
    # checked properly in the in-guest suite, where the events are synthesised
    # straight into ui_event and nothing depends on aim.
    print("SIGNATURES %d %d %d" % (before, opened, clicked))

main("libs", body)
