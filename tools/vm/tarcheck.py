"""tar, after the format moved into the library shared with the Archiver."""
import sys, os, time
sys.path.insert(0, os.path.join(os.getcwd(), "tools/vm"))
from machine import main, Failure

def body(t):
    was = len(t.m.serial())
    t.m.type("tar -tv /root/Documents/manual.tar > /dev/console\n")
    time.sleep(4)
    listing = t.m.serial()[was:]
    if "man/ls.1" not in listing:
        raise Failure("tar did not list the archive:\n%s" % listing[:400])
    print("  listed %d members" % listing.count("\n"))
    t.checks += 1

    was = len(t.m.serial())
    t.m.type("mkdir /tmp/out\n")
    time.sleep(1)
    t.m.type("cd /tmp/out; tar -x /root/Documents/manual.tar; cat man/ls.1 > /dev/console; cd /root\n")
    time.sleep(6)
    got = t.m.serial()[was:]
    if "list what is in a directory" not in got:
        raise Failure("tar did not extract:\n%s" % got[:400])
    print("  extracted and read back a member")
    t.checks += 1

main("tar", body)
