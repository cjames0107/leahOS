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
    t.expect("mount", "/dev/sda2 on / type ext4")

    # Device nodes are nodes, not empty files.
    t.expect("stat /dev/null", "character device")
    t.expect("echo discard > /dev/null; echo wrote", "wrote")

    # The journal is being honoured.
    t.expect("fsck", "clean")

    # A second filesystem attaches, is readable, and detaches.
    t.expect("mount 2 /mnt", "")
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
    if "DMA read and write verified" not in t.m.serial():
        raise Failure("AHCI read/write did not verify")
    t.checks += 1

    # And the in-guest suite, which is the deep one.
    t.expect("tests", "0 failure(s)", timeout=600)


main("smoke", body)
