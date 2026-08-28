"""Whether the new settings do anything.

The rule in settings.c is that a setting must act - nothing there is a switch
that is merely remembered. Rendering a page proves nothing about that, so this
drives the controls and then goes and looks: at /etc/timezone, at ~/.profile
and at the desktop's preferences file, and asks `date` what it now thinks.

Control positions are offsets into the window, measured off a screenshot
rather than derived: the layout decides them and there is no arithmetic here
that would stay right if it changed.
"""
import sys, os, time
sys.path.insert(0, os.path.join(os.getcwd(), "tools/vm"))
from machine import main, Failure

WX, WY = 280, 40
TOP = WY + 37
ROW_H = 24
ROWS = ["Desktop", "General", "Appearance", "Screen",
        "Hardware", "Sound", "Mouse", "Storage",
        "System", "Network", "Date & Time", "Users", "About",
        "UNIX", "Shell", "Terminal"]

def at(dx, dy):
    return (WX + dx, WY + dy)

def page(t, name):
    t.m.click(WX + 60, TOP + ROWS.index(name) * ROW_H + ROW_H // 2)
    time.sleep(2)


def body(t):
    t.m.type("/Applications/Settings.app/settings %d %d &\n" % (WX, WY))
    time.sleep(8)

    # --- the time zone, which writes the file libc has always read ----------
    page(t, "Date & Time")
    for _ in range(4):                          # four quarter hours: UTC+01:00
        t.m.click(*at(669, 224))
        time.sleep(1)
    t.m.shot("eff-timezone")

    # --- natural scrolling, which the compositor reads on every notch -------
    page(t, "Mouse")
    t.m.click(*at(652, 224))
    time.sleep(2)

    # --- the shell's environment, into a file sh now sources ----------------
    page(t, "Shell")
    t.m.click(*at(650, 177))                    # Save
    time.sleep(2)
    t.m.shot("eff-shell")

    # --- the theme, which is the setting that never used to survive --------
    page(t, "Appearance")
    t.m.click(*at(655, 121))                    # Dark
    time.sleep(3)
    t.m.click(*at(661, 177))                    # Window Shadows, off
    time.sleep(3)
    t.m.shot("eff-dark-no-shadows")

    # --- and now go and look ------------------------------------------------
    t.m.click(WX + 15, WY + 15)                 # close Settings
    time.sleep(2)
    t.m.click(150, 620)                         # the terminal, for focus
    time.sleep(1)

    t.expect("cat /etc/timezone", "+60")
    t.expect("date", "UTC+01:00")
    t.expect("grep PATH /root/.profile", "export PATH=")
    t.expect("grep SHELL /root/.profile", "export SHELL=/bin/sh")
    # Values are written as hex, and the desktop's file is desktop.conf - the
    # scope's name with a suffix. Both were guessed at first and both were
    # wrong, which is exactly what an assertion is for.
    t.expect("grep natural /root/.config/desktop.conf",
             "input.natural_scroll 0x000001")
    # And that the theme went into the desktop's file rather than this
    # application's. That is the bug the scope fix was for: it was written to
    # ~/.config/Settings and read back at the next launch from the desktop's
    # file, so no appearance chosen here had ever survived a restart.
    t.expect("grep theme.mode /root/.config/desktop.conf", "theme.mode 0x000001")
    t.expect("grep theme.shadows /root/.config/desktop.conf",
             "theme.shadows 0x000000")

    stray = t.faults()
    if stray:
        raise Failure("settings: %s" % stray[0])


main("settings-effect", body)
