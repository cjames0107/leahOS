"""What the keyboard actually delivers."""
import sys, os, time
sys.path.insert(0, os.path.join(os.getcwd(), "tools/vm"))
from machine import main

def body(t):
    was = len(t.m.serial())
    t.m.type("/usr/bin/wintest keys 9000 &\n")
    time.sleep(4)
    for k in ("left", "shift-left", "ctrl-left", "shift-ctrl-left", "a", "shift-a", "ctrl-z"):
        t.m.key(k)
        time.sleep(0.4)
    time.sleep(6)
    print(t.m.serial()[was:])
    t.checks += 1

main("keydump", body)
