"""The commands after they were moved onto <cli.h>.

Every one of them now gets its options read, its name, its usage line and its
errors from one place, so this checks the things that are now the library's
job rather than each program's - and checks them on a spread of programs wide
enough that a library change that broke one would be caught.

The interesting cases are the ones that were wrong or absent before:

  - "head -n 10 file" used to leave the 10 among the filenames, so head opened
    a file called 10. Nothing declared which options took a value.
  - a stray option was a filename to some programs and an error to others.
  - nothing answered --help.
  - the name in an error message was a literal, so a renamed program lied.

Errors go through 2> /dev/console because this shell does not join the two
streams and the harness reads the console.
"""

import sys, os
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from machine import main


def body(t):
    t.expect("echo alive", "alive")

    # --- a value is a value, not a filename ---------------------------------
    # The bug the options string exists to prevent. Three lines out, and no
    # complaint about a file called 3.
    t.expect("head -n 3 /etc/passwd | wc -l", "3")
    t.expect("head -n 3 /etc/passwd 2> /dev/console | wc -l", "3")
    t.expect("head -n3 /etc/passwd | wc -l", "3")
    t.expect("tail -n 2 /etc/passwd | wc -l", "2")
    # The obsolescent count. It is what fingers type and what old scripts are
    # written in, and it used to be answered with "unknown option -1".
    t.expect("head -1 /etc/passwd | wc -l", "1")
    t.expect("tail -2 /etc/passwd | wc -l", "2")

    # --- unknown options are refused, once and in the program's name --------
    t.expect("wc -Z /etc/passwd 2> /dev/console", "wc: unknown option -Z")
    t.expect("ls -Z 2> /dev/console", "ls: unknown option -Z")
    t.expect("sort -Z 2> /dev/console", "sort: unknown option -Z")
    t.expect("grep -Z x /etc/passwd 2> /dev/console", "grep: unknown option -Z")

    # A refusal prints the usage line with it, so the answer follows the
    # complaint.
    t.expect("wc -Z 2> /dev/console", "usage: wc [-lwc] [file...]")

    # --- --help, which nothing had before -----------------------------------
    t.expect("grep --help", "usage: grep [-ivncrlF] PATTERN [FILE...]")
    t.expect("tar --help", "usage: tar -t|-x [-v]")
    t.expect("df --help", "usage: df [-h]")

    # --- runs of letters ----------------------------------------------------
    t.expect("wc -lw /etc/passwd", "/etc/passwd")
    t.expect("ls -l /bin | wc -l", "")
    t.expect("sort -ru /etc/passwd | wc -l", "")

    # --- "--" still ends the options ----------------------------------------
    t.expect("cd /tmp; touch -- -weird; ls /tmp | grep weird", "-weird")
    t.expect("cd /tmp; rm -- -weird; echo gone", "gone")

    # --- a lone - is standard input, by name --------------------------------
    t.expect("echo piped | cat -", "piped")

    # --- errors carry the program's name ------------------------------------
    t.expect("cat /nope 2> /dev/console", "cat: /nope:")
    t.expect("stat /nope 2> /dev/console", "stat: /nope: no such file")
    t.expect("mkdir /nope/deeper 2> /dev/console", "mkdir: /nope/deeper:")

    # And the name is the one it was run under, not one typed into the source:
    # the same binary under another name says the other name.
    t.expect("cp /bin/wc /tmp/countit; chmod 755 /tmp/countit; "
             "/tmp/countit -Z 2> /dev/console", "countit: unknown option -Z")

    # --- the programs still do their jobs -----------------------------------
    t.expect("printf '%s-%d\\n' ab 7", "ab-7")
    t.expect("echo -n stays a word", "-n stays a word")
    t.expect("basename /usr/share/doc/readme.md .md", "readme")
    t.expect("dirname /usr/share/doc/readme.md", "/usr/share/doc")
    t.expect("grep -c root /etc/passwd", "1")
    t.expect("df -h | grep sda", "GiB")
    t.expect("ps -a | wc -l", "")
    t.expect("date +%Y", "20")
    t.expect("uptime", "load average")
    t.expect("id", "uid=0(root)")

    # --- one walker, used by both programs that walk -----------------------
    # find and grep -r each had their own copy of "list a directory, skip . and
    # .., build the child path, recurse". Both are on cli_walk now, so these
    # check the parts where the copies had differed.
    t.expect("cd /tmp; mkdir w; mkdir w/deep; echo needle > w/deep/buried.txt; "
             "echo made", "made")

    # It descends.
    t.expect("find /tmp/w -name buried.txt", "/tmp/w/deep/buried.txt")
    t.expect("grep -rl needle /tmp/w", "/tmp/w/deep/buried.txt")

    # A name beginning with a dot is an ordinary file. The library's walker
    # used to skip every one of them, which would have made find answer
    # nothing for a file that is there - and grep -r used to skip them too.
    t.expect("echo needle > /tmp/w/.hidden; find /tmp/w -name .hidden",
             "/tmp/w/.hidden")
    t.expect("grep -rl needle /tmp/w | grep hidden", "/tmp/w/.hidden")

    # The starting point is a result in its own right: the walk reports what is
    # inside a root, so find has to test the root itself.
    t.expect("find /tmp/w -type d", "/tmp/w")

    # -type f is regular files, not merely "not a directory": the walker passes
    # the entry's type through, so a fifo is neither.
    t.expect("mkfifo /tmp/w/pipe; find /tmp/w -type f | grep pipe; echo end",
             "end")
    t.expect("find /tmp/w -type f | wc -l", "2")

    # A usage failure is a failure: `basename` with nothing to work on exits
    # non-zero rather than printing an empty line and claiming success.
    t.expect("basename 2> /dev/console; echo status=$?", "usage: basename")
    t.expect("stat 2> /dev/console; echo status=$?", "status=1")


main("converted", body)
