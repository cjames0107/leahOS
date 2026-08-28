"""The applications that used to scroll themselves, on the shared scroll view.

Each one is opened with something taller than its window, wheeled down, and
looked at: a bar where there should be one, content clipped to its pane, and
nothing drawn over the chrome above it.

Files and Terminal are here for a further reason. Both drew a bar of their own,
hit-tested it and dragged its thumb by hand, and neither listened for the wheel
at all - so a wheel notch over either of them did nothing whatever. They are
driven a little harder than the others below: the wheel, and then the thumb,
which is where the difference between the bar moving and the content moving
would show.
"""
import sys, os, time
sys.path.insert(0, os.path.join(os.getcwd(), "tools/vm"))
from machine import Test, Failure


def look(name, launch, at, then=None, before=None):
    t = Test()
    try:
        t.m.type(launch + "\n")
        time.sleep(9)
        if before is not None:
            before(t)
        t.m.shot("sc-%s-top" % name)
        t.m.wheel(at[0], at[1], 6)
        time.sleep(2)
        t.m.shot("sc-%s-down" % name)
        if then is not None:
            then(t)
        stray = t.faults()
        if stray:
            raise Failure("%s: %s" % (name, stray[0]))
    finally:
        t.stop()


def files_harder(t):
    """The three views, the thumb, and the keyboard.

    The thumb is dragged from a point on the track rather than on the thumb, so
    this covers the press as well as the drag - which is the pair that came
    apart: the bar jumped to the new place and the rows stayed where they were,
    because a press on the track moved the offset without laying the content
    out again."""
    t.m.click(713, 86)                  # View
    time.sleep(2)
    t.m.click(660, 133)                 # as List: headings that must stay put
    time.sleep(3)
    t.m.shot("sc-files-list")
    t.m.wheel(500, 300, 5)
    time.sleep(1)
    t.m.shot("sc-files-list-wheeled")
    t.m.drag(791, 200, 791, 400, steps=10)
    time.sleep(1)
    t.m.shot("sc-files-list-dragged")
    t.m.click(713, 86)                  # View
    time.sleep(2)
    t.m.click(660, 151)                 # as Tree
    time.sleep(3)
    t.m.wheel(500, 300, 4)
    time.sleep(1)
    t.m.shot("sc-files-tree")


def term_harder(t):
    """Back to the bottom on a keypress, and the thumb.

    Typing while scrolled back has to bring the view down to where the echo
    will appear: the position is still the terminal's own line number, and this
    is what says the scroll view and that line have stayed in step."""
    t.m.drag(707, 450, 707, 380, steps=8)
    time.sleep(1)
    t.m.shot("sc-term-dragged")
    t.m.type("echo back-at-the-bottom\n")
    time.sleep(3)
    t.m.shot("sc-term-typed")


look("write", "/Applications/Write.app/write 40 40 /root/Documents/welcome.rtf", (400, 300))
look("edit", "/Applications/Edit.app/edit 40 40 /usr/share/doc/readme.md", (400, 300))
look("web", "/Applications/Web.app/web 40 40", (400, 400))

# A directory with more in it than fits, in a window opened onto it.
# Files opens where it last was, so it is walked into a directory with more in
# it than fits. Only the position is read from the command line.
look("files", "/Applications/Files.app/browse 40 40", (500, 300),
     files_harder, lambda t: (t.m.click(382, 130, double=True), time.sleep(3)))
# The terminal is already there and already has a shell; give it some output.
look("term", "ls -l /bin", (300, 600), term_harder)
print("ok    scrolled")
