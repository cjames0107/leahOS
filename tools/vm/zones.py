"""Real time zones: rules, not a number of minutes.

/etc/localtime is a compiled zone file in the format every UNIX uses, and libc
reads it. The thing worth checking is the part a fixed offset cannot do: the
same zone is a different number of hours behind in January and in July, and the
offset shown for a given instant has to be the one that was in force at that
instant rather than the one in force now.

Chicago in August is CDT, UTC-05:00. A zone read as a fixed number would give
UTC-06:00 - its winter offset - so the difference between the two is the whole
feature.
"""
import sys, os, time
sys.path.insert(0, os.path.join(os.getcwd(), "tools/vm"))
from machine import main, Failure


def body(t):
    # The zones are installed, and the one the image ships with is in force.
    t.expect("ls /usr/share/zoneinfo", "America/")
    t.expect("cat /etc/timezone", "UTC")
    t.expect("date", "UTC")

    # A zone with daylight saving, in a month that has it.
    t.expect("cp /usr/share/zoneinfo/America/Chicago /etc/localtime; date",
             "CDT")
    # And one without, on the other side of the world.
    t.expect("cp /usr/share/zoneinfo/Asia/Tokyo /etc/localtime; date", "JST")
    # One whose abbreviation changes with the season, to show the name is read
    # from the file rather than derived from the offset.
    t.expect("cp /usr/share/zoneinfo/Europe/London /etc/localtime; date", "BST")

    # Half-hour zones exist and are not rounded away.
    t.expect("cp /usr/share/zoneinfo/Asia/Kolkata /etc/localtime; date", "IST")

    # Back to where it started, so the image is left as it was found.
    t.expect("cp /usr/share/zoneinfo/UTC /etc/localtime; date", "UTC")

    stray = t.faults()
    if stray:
        raise Failure("%s" % stray[0])


main("zones", body)
