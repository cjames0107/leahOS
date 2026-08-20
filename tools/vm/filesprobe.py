"""Files on its own: the toolbar, the sidebar, the menus and the search."""
import sys, os, time
sys.path.insert(0, os.path.join(os.getcwd(), "tools/vm"))
from machine import main, Failure

def body(t):
    t.m.type("/Applications/Files.app/browse 40 40 &\n")
    time.sleep(8)
    t.m.shot("files")
    # The View menu, which is a component now.
    t.m.click(727, 85)          # "View"
    time.sleep(2)
    t.m.shot("files-menu")
    t.m.click(660, 133)         # "as List"
    time.sleep(3)
    t.m.shot("files-list")
    # And the search field, which is a component too.
    t.m.click(572, 85)
    time.sleep(1)
    t.m.type("passwd")
    time.sleep(3)
    t.m.shot("files-search")
    t.checks += 1

main("files-alone", body)
