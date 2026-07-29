#!/usr/bin/env python3
"""Generate the account files for the leahOS image.

The hash has to match kernel/lib/sha256.cpp exactly:

    digest = sha256(salt || password)
    repeat (iterations - 1) times: digest = sha256(digest)

Salted so two users with the same password do not share a digest, and stretched
so guessing costs something. It is not memory-hard - see the note in
sha256.hpp - and these are demonstration passwords in a public repository, which
is precisely why they are written down here rather than pretended to be secret.

    mkaccounts.py <staging-dir>
"""

import hashlib
import os
import sys

ITERATIONS = 4096

# name, uid, gid, home, salt, password
ACCOUNTS = [
    ("root", 0,    0,    "/root",      "rootsalt", "toor"),
    ("leah", 1000, 1000, "/home/leah", "leahsalt", "leah"),
    ("guest", 1001, 1001, "/home/guest", "gstsalt", "guest"),
]


def password_hash(salt: str, password: str, iterations: int) -> str:
    digest = hashlib.sha256((salt + password).encode("ascii")).digest()
    for _ in range(iterations - 1):
        digest = hashlib.sha256(digest).digest()
    return digest.hex()


def main() -> int:
    if len(sys.argv) != 2:
        print("usage: mkaccounts.py <staging-dir>", file=sys.stderr)
        return 2
    staging = sys.argv[1]

    os.makedirs(os.path.join(staging, "etc"), exist_ok=True)

    # /etc/passwd is the public half: names, ids and home directories, and
    # nothing an attacker could use. World readable, as it is everywhere.
    with open(os.path.join(staging, "etc", "passwd"), "w") as handle:
        for name, uid, gid, home, _salt, _password in ACCOUNTS:
            handle.write(f"{name}:x:{uid}:{gid}:{home}:/bin/sh.elf\n")

    # /etc/shadow holds the digests, and only the kernel ever reads it.
    with open(os.path.join(staging, "etc", "shadow"), "w") as handle:
        for name, uid, gid, home, salt, password in ACCOUNTS:
            digest = password_hash(salt, password, ITERATIONS)
            handle.write(f"{name}:{uid}:{gid}:{home}:{salt}:{ITERATIONS}:{digest}\n")

    # Home directories. Empty of files on purpose: mkext.sh gives each one a
    # Desktop, Documents, Apps and Public, which is something to see and
    # somewhere to put things. A readme explaining whose directory you are
    # already standing in was neither.
    for _name, _uid, _gid, home, _salt, _password in ACCOUNTS:
        os.makedirs(os.path.join(staging, home.lstrip("/")), exist_ok=True)

    print(f"accounts: {len(ACCOUNTS)} users, {ITERATIONS} hash iterations")
    return 0


if __name__ == "__main__":
    sys.exit(main())
