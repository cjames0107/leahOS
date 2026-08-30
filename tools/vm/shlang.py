"""The shell as a language rather than a launcher.

Until now sh could run things and join them with pipes and could not decide
anything: no if, no loops, no functions, no test, and a $( ) that silently
expanded to nothing - which is worse than not having it, because a script using
one looked like it worked.

Everything here is checked by what it prints, because the point of a
conditional is which branch ran.

Most of it goes through `sh -c '...'` rather than being typed at the prompt,
and that is not incidental: the harness redirects the command it is given to
the console, and on a compound command that redirect lands on the `fi` - so the
echo inside goes to the terminal and the check sees nothing. Wrapping makes it
one simple command again. Single quotes outside and double inside, so that a $
is expanded by the shell being tested rather than by the one typing at it.
"""
import sys, os
sys.path.insert(0, os.path.join(os.getcwd(), "tools/vm"))
from machine import main


def sh(script):
    return "sh -c '" + script + "'"


def body(t):
    # --- test, on its own ---------------------------------------------------
    t.expect("test -f /etc/passwd && echo yes", "yes")
    t.expect("test -f /nope || echo no", "no")
    t.expect("[ 3 -lt 5 ] && echo less", "less")
    t.expect("[ abc = abc ] && echo same", "same")
    t.expect("[ abc != abc ] || echo differs", "differs")
    t.expect('[ -z "" ] && echo empty', "empty")
    t.expect("[ -n x ] && echo nonempty", "nonempty")
    t.expect("[ ! -f /nope ] && echo negated", "negated")
    t.expect("[ -f /etc/passwd -a -d /bin ] && echo both", "both")
    t.expect("[ 2 -gt 5 ] || echo not-greater", "not-greater")
    # /bin/test is the same expression for anything that is not the shell.
    t.expect("/bin/test -d /bin && echo external", "external")
    t.expect("/bin/[ -d /bin ] && echo bracket", "bracket")

    # --- command substitution ----------------------------------------------
    t.expect("echo $(echo inner)", "inner")
    t.expect("echo a$(echo b)c", "abc")
    t.expect("echo `echo backtick`", "backtick")
    t.expect("echo $(echo $(echo nested))", "nested")
    t.expect("X=$(echo captured); echo $X", "captured")
    # Trailing newlines go, or `cd $(pwd)` could not work.
    t.expect("cd $(echo /bin); pwd", "/bin")
    # It splits into words unless quoted, which is what a for loop needs.
    t.expect(sh('for f in $(echo a b c); do echo w-$f; done') + " | wc -l", "3")

    # --- if -----------------------------------------------------------------
    t.expect(sh("if true; then echo taken; fi"), "taken")
    t.expect(sh("if false; then echo no; else echo else-taken; fi"), "else-taken")
    t.expect(sh("if false; then echo a; elif true; then echo elif-taken; fi"),
             "elif-taken")
    t.expect(sh("if test -f /etc/passwd; then echo found; fi"), "found")
    t.expect(sh('if [ "$HOME" = /root ]; then echo home-ok; fi'), "home-ok")

    # --- loops --------------------------------------------------------------
    t.expect(sh("for i in a b c; do echo item-$i; done") + " | wc -l", "3")
    t.expect(sh("for i in one; do echo $i; done"), "one")
    t.expect(sh("n=0; while [ $n -lt 3 ]; do n=3; echo looped; done"), "looped")
    t.expect(sh("i=9; until [ $i -lt 5 ]; do echo turn; i=1; done"), "turn")
    # break and continue leave the loop they are in, even from inside an if.
    t.expect(sh("for i in 1 2 3 4; do if [ $i = 3 ]; then break; fi; echo n$i; done")
             + " | wc -l", "2")
    t.expect(sh("for i in 1 2 3; do if [ $i = 2 ]; then continue; fi; echo k$i; done")
             + " | wc -l", "2")

    # --- case ---------------------------------------------------------------
    t.expect(sh("case hello in hello) echo matched;; *) echo other;; esac"),
             "matched")
    t.expect(sh("case zzz in hello) echo matched;; *) echo fell-through;; esac"),
             "fell-through")
    t.expect(sh("case abc in x|abc|y) echo alternate;; esac"), "alternate")
    t.expect(sh("case file.txt in *.txt) echo is-text;; esac"), "is-text")

    # --- functions ----------------------------------------------------------
    t.expect(sh("greet() { echo hello $1; }; greet world"), "hello world")
    t.expect(sh("f() { return 3; }; f; echo $?"), "3")
    t.expect(sh("g() { echo $1-$2; }; g a b"), "a-b")

    # --- nesting, which is where a naive parser falls over -------------------
    t.expect(sh("for i in 1 2; do if [ $i = 2 ]; then echo deep-$i; fi; done"),
             "deep-2")
    t.expect(sh("if true; then for i in x; do echo $i-inside; done; fi"),
             "x-inside")
    t.expect(sh("if true; then if true; then echo twice; fi; fi"), "twice")

    # --- and a real script, over several lines, run by its own #! -----------
    t.expect("printf '#!/bin/sh\\nfor f in a b\\ndo\\n  if [ $f = b ]\\n"
             "  then\\n    echo script-$f\\n  fi\\ndone\\n' > /tmp/s.sh; "
             "chmod 755 /tmp/s.sh; /tmp/s.sh", "script-b")


main("shell-language", body)
