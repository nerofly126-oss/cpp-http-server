#!/usr/bin/env python3
"""Black-box tests: start the real server binary and talk to it over raw sockets.

Usage: integration_test.py <path-to-httpserver> [--slow]

--slow adds the tests that wait on real timeouts (idle, stalled, slowloris).
Uses only the Python standard library.
"""
import os
import signal
import socket
import subprocess
import sys
import threading
import time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
HOST = "127.0.0.1"
PORT = 0
failures = 0


def check(name, cond, detail=""):
    global failures
    print(("PASS " if cond else "FAIL ") + name + (" " + detail if detail else ""), flush=True)
    if not cond:
        failures += 1


def connect():
    s = socket.create_connection((HOST, PORT))
    s.settimeout(15)
    return s


def read_response(s, buf=b""):
    """Reads one full response. Returns (response, leftover) or (None, leftover)."""
    while b"\r\n\r\n" not in buf:
        data = s.recv(65536)
        if not data:
            return None, buf
        buf += data
    head, rest = buf.split(b"\r\n\r\n", 1)
    lines = head.split(b"\r\n")
    headers = {}
    for line in lines[1:]:
        name, value = line.split(b":", 1)
        headers[name.strip().lower()] = value.strip()
    length = int(headers.get(b"content-length", b"0"))
    while len(rest) < length:
        data = s.recv(65536)
        if not data:
            break
        rest += data
    response = {"status": int(lines[0].split()[1]), "headers": headers, "body": rest[:length]}
    return response, rest[length:]


def request(raw):
    """Sends raw bytes on a fresh connection, returns the response (or None)."""
    s = connect()
    try:
        s.sendall(raw)
        response, _ = read_response(s)
        return response
    finally:
        s.close()


def status_of(raw):
    r = request(raw)
    return r["status"] if r else None


# --------------------------------------------------------------------- tests


def test_basics():
    r = request(b"GET /health HTTP/1.1\r\n\r\n")
    check("GET /health", r and r["status"] == 200 and r["body"] == b"ok\n")
    r = request(b"GET / HTTP/1.1\r\n\r\n")
    check("static index.html", r and r["status"] == 200 and b"<h1>" in r["body"])
    check("404", status_of(b"GET /nope HTTP/1.1\r\n\r\n") == 404)
    check("405", status_of(b"DELETE /health HTTP/1.1\r\n\r\n") == 405)
    check("path traversal blocked", status_of(b"GET /../src/main.cpp HTTP/1.1\r\n\r\n") == 403)
    check("encoded traversal blocked", status_of(b"GET /%2e%2e/src/main.cpp HTTP/1.1\r\n\r\n") == 403)
    r = request(b"POST /echo HTTP/1.1\r\nContent-Length: 5\r\n\r\nhello")
    check("POST /echo", r and r["body"] == b"hello")


def test_connection_handling():
    s = connect()
    s.sendall(b"GET /health HTTP/1.1\r\n\r\n")
    r1, rest = read_response(s)
    s.sendall(b"POST /echo HTTP/1.1\r\nContent-Length: 5\r\n\r\nhello")
    r2, rest = read_response(s, rest)
    check("keep-alive: two requests on one connection", r1 and r2 and r2["body"] == b"hello")
    s.close()

    s = connect()
    s.sendall(b"GET /health HTTP/1.1\r\n\r\nPOST /echo HTTP/1.1\r\nContent-Length: 3\r\n\r\nabc")
    r1, rest = read_response(s)
    r2, rest = read_response(s, rest)
    check("pipelined requests answered in order", r1 and r2 and r1["body"] == b"ok\n" and r2["body"] == b"abc")
    s.close()

    s = connect()
    for piece in [b"POST /ec", b"ho HTTP/1.1\r\nContent-Le", b"ngth: 11\r\n\r\nhello ", b"world"]:
        s.sendall(piece)
        time.sleep(0.1)
    r, _ = read_response(s)
    check("request split across many packets", r and r["body"] == b"hello world")
    s.close()

    s = connect()
    s.sendall(b"GET /health HTTP/1.1\r\nConnection: close\r\n\r\n")
    r, _ = read_response(s)
    check("Connection: close is honoured", r and s.recv(10) == b"")
    s.close()

    s = connect()
    s.sendall(b"GET /health HTTP/1.0\r\n\r\n")
    r, _ = read_response(s)
    check("HTTP/1.0 closes by default", r and s.recv(10) == b"")
    s.close()

    body = b"x" * 600000
    s = connect()
    s.sendall(b"POST /echo HTTP/1.1\r\nContent-Length: %d\r\n\r\n" % len(body) + body)
    r, _ = read_response(s)
    check("600 KB body echoed back", r and r["body"] == body)
    s.close()

    s = connect()
    s.sendall(b"GET /slow HTTP/1.1\r\n\r\n")
    s.shutdown(socket.SHUT_WR)
    r, _ = read_response(s)
    check("half-closed client still gets its response", r and r["body"] == b"finally\n")
    s.close()

    s = connect()
    s.sendall(b"POST /echo HTTP/1.1\r\nContent-Length: 100\r\n\r\nab")
    s.close()
    s = connect()
    s.sendall(b"GET /slow HTTP/1.1\r\n\r\n")
    time.sleep(0.2)
    s.close()
    time.sleep(2.5)
    check("server survives clients vanishing mid-request", status_of(b"GET /health HTTP/1.1\r\n\r\n") == 200)


def test_concurrency():
    result = {}

    def slow():
        s = connect()
        t = time.time()
        s.sendall(b"GET /slow HTTP/1.1\r\n\r\n")
        r, _ = read_response(s)
        result["elapsed"] = time.time() - t
        result["body"] = r["body"] if r else None
        s.close()

    thread = threading.Thread(target=slow)
    thread.start()
    time.sleep(0.3)
    t = time.time()
    r = request(b"GET /health HTTP/1.1\r\n\r\n")
    fast = time.time() - t
    thread.join()
    check("fast request while /slow is running (%.3fs)" % fast, r and r["status"] == 200 and fast < 0.5)
    check("slow request completes (%.1fs)" % result["elapsed"], result["body"] == b"finally\n" and result["elapsed"] >= 1.9)

    bodies = []

    def one():
        s = connect()
        s.sendall(b"GET /slow HTTP/1.1\r\n\r\n")
        r, _ = read_response(s)
        bodies.append(r["body"])
        s.close()

    t = time.time()
    threads = [threading.Thread(target=one) for _ in range(4)]
    [x.start() for x in threads]
    [x.join() for x in threads]
    elapsed = time.time() - t
    check("4 parallel /slow finish in %.1fs (limit 5s)" % elapsed, len(bodies) == 4 and elapsed < 5)

    errors = []

    def burst():
        try:
            s = connect()
            for _ in range(5):
                s.sendall(b"GET /index.html HTTP/1.1\r\n\r\n")
                r, _ = read_response(s)
                if not r or r["status"] != 200:
                    errors.append(1)
            s.close()
        except Exception as e:  # noqa: BLE001
            errors.append(e)

    t = time.time()
    threads = [threading.Thread(target=burst) for _ in range(200)]
    [x.start() for x in threads]
    [x.join() for x in threads]
    check("200 connections x 5 requests (%d errors, %.2fs)" % (len(errors), time.time() - t), not errors)


def test_hardening():
    check("conflicting Content-Length -> 400",
          status_of(b"POST /echo HTTP/1.1\r\nContent-Length: 3\r\nContent-Length: 4\r\n\r\nabcd") == 400)
    check("identical duplicate Content-Length accepted",
          status_of(b"POST /echo HTTP/1.1\r\nContent-Length: 3\r\nContent-Length: 3\r\n\r\nabc") == 200)
    check("bare LF in header -> 400", status_of(b"GET /health HTTP/1.1\r\nX: a\nInjected: b\r\n\r\n") == 400)
    check("NUL in header -> 400", status_of(b"GET /health HTTP/1.1\r\nX: a\x00b\r\n\r\n") == 400)
    check("NUL in target -> 400", status_of(b"GET /he\x00alth HTTP/1.1\r\n\r\n") == 400)
    check("garbage -> 400", status_of(b"garbage\r\n\r\n") == 400)
    check("HTTP/2.0 -> 505", status_of(b"GET / HTTP/2.0\r\n\r\n") == 505)
    check("chunked bodies -> 501", status_of(b"POST /echo HTTP/1.1\r\nTransfer-Encoding: chunked\r\n\r\n") == 501)
    check("oversized body -> 413", status_of(b"POST /echo HTTP/1.1\r\nContent-Length: 99999999\r\n\r\n") == 413)

    def with_headers(n):
        return b"GET /health HTTP/1.1\r\n" + b"".join(b"H%d: v\r\n" % i for i in range(n)) + b"\r\n"

    check("100 headers accepted", status_of(with_headers(100)) == 200)
    check("101 headers -> 431", status_of(with_headers(101)) == 431)
    check("oversized header block -> 431",
          status_of(b"GET /health HTTP/1.1\r\nX: " + b"a" * 9000 + b"\r\n\r\n") == 431)

    sockets = []
    for _ in range(300):
        try:
            sockets.append(connect())
        except OSError:
            pass
    time.sleep(0.5)
    served = rejected = 0
    for s in sockets:
        try:
            s.settimeout(3)
            s.sendall(b"GET /health HTTP/1.1\r\n\r\n")
            r, _ = read_response(s)
            if r and r["body"] == b"ok\n":
                served += 1
            else:
                rejected += 1
        except OSError:
            rejected += 1
    for s in sockets:
        s.close()
    check("per-IP cap: 300 opened, %d served, %d rejected" % (served, rejected), served == 256 and rejected == 44)
    time.sleep(1)
    check("slots are freed when clients leave", status_of(b"GET /health HTTP/1.1\r\n\r\n") == 200)


def test_slow_timeouts():
    s = connect()
    t = time.time()
    data = s.recv(10)
    el = time.time() - t
    check("idle connection closed silently after ~10s (%.1fs)" % el, data == b"" and 9 < el < 13)
    s.close()

    s = connect()
    s.sendall(b"GET / HTTP/1.1\r\nHost: x")
    t = time.time()
    r, _ = read_response(s)
    el = time.time() - t
    check("stalled request gets 408 after ~10s (%.1fs)" % el, r and r["status"] == 408 and 9 < el < 13)
    s.close()

    # One byte every 2s keeps the connection "active", so only the per-request
    # deadline can catch it.
    s = connect()
    s.settimeout(1)
    prefix = b"GET / HTTP/1.1\r\nX-Slow: "
    t = time.time()
    got = None
    i = 0
    while time.time() - t < 25 and got is None:
        s.sendall(prefix[i:i + 1] if i < len(prefix) else b"a")
        i += 1
        try:
            data = s.recv(4096)
            got = data if data else b"(closed)"
        except socket.timeout:
            time.sleep(1)
        except (ConnectionResetError, BrokenPipeError):
            got = b"(reset)"
    el = time.time() - t
    check("slowloris dripper dropped at ~15s (%.1fs)" % el,
          got is not None and 14 < el < 19 and (b"408" in got or got in (b"(closed)", b"(reset)")))
    s.close()


# ---------------------------------------------------------------------- main


def wait_for_port(proc):
    for _ in range(100):
        if proc.poll() is not None:
            return False
        try:
            socket.create_connection((HOST, PORT), timeout=0.2).close()
            return True
        except OSError:
            time.sleep(0.05)
    return False


def main():
    global PORT
    if len(sys.argv) < 2:
        print(__doc__)
        return 2
    binary = os.path.abspath(sys.argv[1])
    slow = "--slow" in sys.argv[2:]

    probe = socket.socket()
    probe.bind((HOST, 0))
    PORT = probe.getsockname()[1]
    probe.close()

    proc = subprocess.Popen([binary, str(PORT), os.path.join(ROOT, "static"), "-q"],
                            stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
    if not wait_for_port(proc):
        proc.kill()
        out, _ = proc.communicate()
        print("server did not start:\n" + out)
        return 1

    try:
        test_basics()
        test_connection_handling()
        test_concurrency()
        test_hardening()
        if slow:
            test_slow_timeouts()
    finally:
        proc.send_signal(signal.SIGINT)
        try:
            out, _ = proc.communicate(timeout=15)
        except subprocess.TimeoutExpired:
            proc.kill()
            out, _ = proc.communicate()
            check("server exits on SIGINT", False)

    check("server exits cleanly on SIGINT", proc.returncode == 0, "(exit code %s)" % proc.returncode)
    reports = [m for m in ("AddressSanitizer", "ThreadSanitizer", "LeakSanitizer", "runtime error") if m in out]
    check("no sanitizer reports", not reports, str(reports))
    if reports:
        print(out)

    print("\n%s" % ("ALL PASSED" if failures == 0 else "%d FAILED" % failures))
    return 0 if failures == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
