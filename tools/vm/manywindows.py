"""More windows than one bank holds.

The table used to be a fixed array - sixteen, then thirty-two - so this is the
check that it is not one any more. Asked of a program rather than of a shell
loop, because the first window to open takes the keyboard and every command
typed after that goes into it instead of into the terminal.
"""
import sys, os, time
sys.path.insert(0, os.path.join(os.getcwd(), "tools/vm"))
from machine import main, Failure

WANT = 70          # a bank is 32, so this needs a third one

def body(t):
    was = len(t.m.serial())
    t.m.type("/usr/bin/wintest %d 6000 > /dev/console\n" % WANT)

    said = ""
    for _ in range(60):
        time.sleep(0.5)
        said = t.m.serial()[was:]
        if "wintest:" in said:
            break
        if "windows" in said:
            break
    t.m.shot("manywindows")

    line = [l for l in said.splitlines() if l.startswith("wintest:")]
    if not line:
        raise Failure("wintest said nothing:\n%s" % said)
    print("  %s" % line[0])
    got = int(line[0].split()[1])
    if got < WANT:
        raise Failure("only %d of %d windows opened" % (got, WANT))
    t.checks += 1

main("many-windows", body)
