"""The checks worth making on every build, in the order a failure matters.

Deliberately shallow and broad: this is meant to catch a boot that does not
boot and a filesystem that does not answer, not to replace the in-guest suite,
which is what `tests` is for and which this runs last.
"""

import sys, os
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from machine import main


def body(t):
    # The suite forks children that try port I/O from ring 3 to prove the
    # kernel stops them. Those two deaths are the test working; anything else
    # that dies is not.
    t.allow_fault("tests[", "the suite's own port-I/O children, killed on purpose")

    # The filesystem answers, and answers about itself.
    t.expect("echo alive", "alive")
    t.expect("ls /bin | wc -l", "")
    t.expect("mount", "on / type ext4")

    # Device nodes are nodes, not empty files.
    t.expect("stat /dev/null", "character device")
    t.expect("echo discard > /dev/null; echo wrote", "wrote")

    # The journal is being honoured.
    t.expect("fsck", "clean")

    # A second filesystem attaches, is readable, and detaches.
    t.expect("mount 0 /mnt", "")
    t.expect("cat /mnt/notes/hello.txt", "second filesystem")
    t.expect("mount -u /mnt; mount", "procfs on /proc")

    # The SATA controller is found and its port enumerated. Reading the serial
    # line directly, because this is printed at boot before there is a shell.
    from machine import Failure
    if "ahcid: AHCI" not in t.m.serial():
        raise Failure("the AHCI controller was not found at boot")
    t.checks += 1
    # And that its command path moves data both ways: the driver writes a
    # pattern to a spare sector, reads it back and compares.
    if "DMA read verified" not in t.m.serial():
        raise Failure("the AHCI disk did not read")
    t.checks += 1

    # The root itself is on the AHCI controller: every block of every read
    # above this line already came back by DMA.
    if "vfsd: root is disk 4" not in t.m.serial():
        raise Failure("the root filesystem is not on the AHCI disk")
    t.checks += 1

    # And a second disk on the same controller, which is what multi-port
    # support is for: disk 5 is the AHCI driver's second port.
    t.expect("mount 5 /sata", "")
    t.expect("cat /sata/sata/hello.txt", "came off the SATA disk")
    t.expect("mount -u /sata; echo detached", "detached")

    # Something is actually on the screen. Every other check here would pass
    # on a machine that draws nothing at all.
    colours = t.m.screen_colours("smoke")
    if colours < 16:
        raise Failure("the screen has %d colour(s) - nothing is being drawn"
                      % colours)
    t.checks += 1

    # How the filesystem behaves when more than one thing wants it. The ratio
    # between one reader and four is the whole question: a server that serves
    # strictly one at a time takes four times as long for four readers.
    for n in (1, 2, 4):
        t.expect("fsbench %d" % n, "fsbench: %d reader" % n, timeout=240)

    # And the in-guest suite, which is the deep one.
    t.expect("tests", "0 failure(s)", timeout=600)


main("smoke", body)
