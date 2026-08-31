"""Sockets: being the end that is connected to.

The stack could open a connection, carry it and close it, and had no listen and
no accept - so nothing here could be a server. This starts one inside the guest
and connects to it from outside, over a port QEMU forwards in, which is the
only test that exercises the whole path: a SYN this machine did not send, a
handshake it answers, an accept that hands back a descriptor, and a reply
written to it with an ordinary write.

Connecting from the host rather than from the guest is deliberate. There is no
loopback interface here, so a guest-to-guest connection would have nothing to
travel over - and a test that ran both ends in one process would prove less
anyway.
"""
import sys, os, time, threading, socket as pysocket
sys.path.insert(0, os.path.join(os.getcwd(), "tools/vm"))
from machine import Test, Failure

PORT = 8080


def fetch(path, timeout=10):
    """One HTTP/1.0 request from this machine into the guest."""
    s = pysocket.create_connection(("127.0.0.1", PORT), timeout=timeout)
    try:
        s.sendall(("GET %s HTTP/1.0\r\nHost: leah\r\n\r\n" % path).encode())
        chunks = []
        while True:
            b = s.recv(4096)
            if not b:
                break
            chunks.append(b)
        return b"".join(chunks).decode("utf-8", "replace")
    finally:
        s.close()


# A server on this machine, for the guest to be a client of.
def host_server(sock):
    while True:
        try:
            client, _ = sock.accept()
        except OSError:
            return
        try:
            client.recv(2048)
            client.sendall(b"HTTP/1.0 200 OK\r\nContent-Length: 14\r\n"
                           b"Connection: close\r\n\r\nfrom-the-host\n")
        except OSError:
            pass
        finally:
            client.close()


listener = pysocket.socket(pysocket.AF_INET, pysocket.SOCK_STREAM)
listener.setsockopt(pysocket.SOL_SOCKET, pysocket.SO_REUSEADDR, 1)
listener.bind(("0.0.0.0", 0))
HOST_PORT = listener.getsockname()[1]
listener.listen(4)
threading.Thread(target=host_server, args=(listener,), daemon=True).start()

t = Test(hostfwd=(PORT, 80))
try:
    time.sleep(4)
    # Something to serve that is certainly there, and a server to serve it.
    #
    # Written and then read back in one command, because the harness appends
    # its own redirect: `echo x > file` becomes `echo x > file > /dev/console`
    # and the last redirect wins, so the file is never written. Ending with a
    # cat puts the harness's redirect on the cat, where it belongs, and proves
    # the file exists in the same breath.
    t.expect("echo hello-from-leahos > /tmp/probe.txt; cat /tmp/probe.txt",
             "hello-from-leahos")
    t.m.type("httpd -p 80 /tmp &\n")
    time.sleep(4)

    body = fetch("/probe.txt")
    if "200 OK" not in body:
        raise Failure("no 200 in the reply: %r" % body[:200])
    if "hello-from-leahos" not in body:
        raise Failure("the file did not come back: %r" % body[:200])
    if "text/plain" not in body:
        raise Failure("wrong content type: %r" % body[:200])
    print("  ok    a file served over TCP from inside the guest")

    # A second request on a second connection, which is what says accept comes
    # back round rather than working once.
    again = fetch("/probe.txt")
    if "hello-from-leahos" not in again:
        raise Failure("the second connection failed: %r" % again[:200])
    print("  ok    a second connection, accepted after the first closed")

    # And something that is not there, so the server is reading the request
    # rather than sending the same bytes to everyone.
    missing = fetch("/nothing-here.txt")
    if "404" not in missing:
        raise Failure("a missing file did not 404: %r" % missing[:200])
    print("  ok    a missing file answered 404")

    # And the client side still works, which is worth checking because the
    # handle a connection is known by changed: it used to be the local port,
    # which is unique for a connection this machine started and is not for one
    # it accepted - every connection to a listening port shares that port.
    #
    # Against a server on this machine rather than something on the internet:
    # the guest reaches the host at the gateway SLIRP gives it, so this needs
    # no network beyond the one QEMU is already pretending to be, and a test
    # that fails when the wifi is off is a test nobody trusts.
    # No 2>&1: this shell has 2> and 1> and does not join the two streams,
    # so it would be read as a redirect to a file called &1.
    t.expect("fetch -p %d 10.0.2.2 /hello" % HOST_PORT, "from-the-host")
    print("  ok    outbound connections still work")

    stray = t.faults()
    if stray:
        raise Failure("%s" % stray[0])
    print("ok    sockets (4 checks)")
except Failure as e:
    print("FAIL  sockets: %s" % e)
    sys.exit(1)
finally:
    try:
        t.stop()
    except Exception:
        pass
