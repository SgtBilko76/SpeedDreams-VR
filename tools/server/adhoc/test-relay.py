#!/usr/bin/env python3
"""End-to-end test of sd-adhoc-relay on localhost.

Plays every part: a "race server" (UDP echo), the host's tunnel (what the game
does on the hosting headset), and two joining players. Run after building:
    ./test-relay.py ./sd-adhoc-relay
"""
import json, os, shutil, socket, struct, subprocess, sys, tempfile, threading, time
import urllib.error, urllib.request

RELAY = sys.argv[1] if len(sys.argv) > 1 else "./sd-adhoc-relay"
CTL, BASE = 39500, 39510
CTL_B, BASE_B = 39700, 39710      # the peer relay
WEB_A, WEB_B = 39600, 39601
DATA = tempfile.mkdtemp()
DATA_A, DATA_B = os.path.join(DATA, "a"), os.path.join(DATA, "b")
os.makedirs(DATA_A); os.makedirs(DATA_B)
ok = True


def check(cond, what):
    global ok
    print(("PASS " if cond else "FAIL ") + what)
    ok &= bool(cond)


def start_a():
    return subprocess.Popen([RELAY, "-p", str(CTL), "-b", str(BASE), "-n", "4", "-w", str(WEB_A),
                             "-d", DATA_A, "-P", f"127.0.0.1:{CTL_B}"])


def start_b():
    return subprocess.Popen([RELAY, "-p", str(CTL_B), "-b", str(BASE_B), "-n", "4", "-w", str(WEB_B),
                             "-d", DATA_B, "-P", f"127.0.0.1:{CTL}"])


def records(port):
    with urllib.request.urlopen(f"http://127.0.0.1:{port}/records", timeout=3) as r:
        return {t["track"]: t for t in json.load(r)["tracks"]}


proc = start_a()
proc_b = start_b()
time.sleep(0.3)
try:
    # Local "race server": echoes with a prefix.
    game = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    game.bind(("127.0.0.1", 0))
    game_port = game.getsockname()[1]

    def echo():
        while True:
            try:
                d, a = game.recvfrom(4096)
            except OSError:
                return
            game.sendto(b"echo:" + d, a)
    threading.Thread(target=echo, daemon=True).start()

    # Host tunnel.
    tun = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    tun.settimeout(2)
    tun.sendto(b"HOST 1 8 jarama Bilko's race", ("127.0.0.1", CTL))
    reply = tun.recv(2048).decode()
    check(reply.startswith("OK "), "host registers: " + reply)
    sid, sport = map(int, reply.split()[1:3])

    tun.sendto(b"HOST 9 8 jarama x", ("127.0.0.1", CTL))
    check(tun.recv(2048).startswith(b"ERR version"), "wrong protocol version is refused")
    tun.sendto(b"HOST 1 8 jarama Bilko's race", ("127.0.0.1", CTL))
    check(tun.recv(2048).decode() == reply, "re-registering keeps the same session")

    lst = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    lst.settimeout(2)
    lst.sendto(b"LIST", ("127.0.0.1", CTL))
    listing = lst.recv(2048).decode()
    check(f"{sid} {sport} 1 8 jarama Bilko's race" in listing, "LIST shows the session")
    lst.sendto(b"PING", ("127.0.0.1", CTL))
    check(lst.recv(64) == b"PONG", "PING")

    # Host-side tunnel pump: one local socket per relay client id.
    locals_ = {}
    stop = False

    def pump():
        tun.settimeout(0.05)
        while not stop:
            try:
                d = tun.recv(4096)
                if d[:1] == b"\x01":
                    cid = struct.unpack("!I", d[1:5])[0]
                    if cid not in locals_:
                        s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
                        s.connect(("127.0.0.1", game_port))
                        s.setblocking(False)
                        locals_[cid] = s
                    locals_[cid].send(d[5:])
            except (socket.timeout, BlockingIOError):
                pass
            for cid, s in list(locals_.items()):
                try:
                    while True:
                        tun.sendto(b"\x01" + struct.pack("!I", cid) + s.recv(4096), ("127.0.0.1", CTL))
                except BlockingIOError:
                    pass
    t = threading.Thread(target=pump, daemon=True)
    t.start()

    # Two players join through the public session port.
    p1 = socket.socket(socket.AF_INET, socket.SOCK_DGRAM); p1.settimeout(2)
    p2 = socket.socket(socket.AF_INET, socket.SOCK_DGRAM); p2.settimeout(2)
    for i in range(20):
        p1.sendto(b"p1-%d" % i, ("127.0.0.1", sport))
        p2.sendto(b"p2-%d" % i, ("127.0.0.1", sport))
    r1 = sorted(p1.recv(4096) for _ in range(20))
    r2 = sorted(p2.recv(4096) for _ in range(20))
    check(r1 == sorted(b"echo:p1-%d" % i for i in range(20)), "player 1 gets exactly its own 20 replies")
    check(r2 == sorted(b"echo:p2-%d" % i for i in range(20)), "player 2 gets exactly its own 20 replies")
    check(len(locals_) == 2, "host sees two separate clients")

    big = bytes(range(256)) * 5 + b"x" * 100  # 1380 bytes, ENet's usual MTU size
    p1.sendto(big, ("127.0.0.1", sport))
    check(p1.recv(4096) == b"echo:" + big, "full-size packet survives the round trip")

    # Somebody else cannot inject traffic as the host.
    evil = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    evil.sendto(b"\x01" + struct.pack("!I", 1) + b"spoof", ("127.0.0.1", CTL))
    p1.settimeout(0.5)
    try:
        got = p1.recv(4096)
    except socket.timeout:
        got = None
    check(got is None, "non-host cannot send to players")

    # ---- lap records ------------------------------------------------------
    evil.sendto(b"LAP 1 jarama road sc-boxer 61.5 Cheater", ("127.0.0.1", CTL))
    time.sleep(0.3)
    check("jarama" not in records(WEB_A), "a lap from someone who is not hosting is ignored")

    tun.sendto(b"LAP 1 jarama road sc-boxer 72.125 Bilko", ("127.0.0.1", CTL))
    time.sleep(0.3)
    ra, rb = records(WEB_A), records(WEB_B)
    check(ra.get("jarama", {}).get("times", [{}])[0].get("time") == 72.125
          and ra["jarama"]["times"][0]["name"] == "Bilko" and ra["jarama"]["category"] == "road",
          "host's lap is on the leaderboard")
    check(rb.get("jarama", {}).get("times", [{}])[0].get("time") == 72.125, "and on the backup relay too")

    tun.sendto(b"LAP 1 jarama road sc-boxer 75.0 Bilko", ("127.0.0.1", CTL))
    tun.sendto(b"LAP 1 jarama road ls1 70.5 Bilko", ("127.0.0.1", CTL))
    for i in range(11):
        tun.sendto(f"LAP 1 jarama road car{i} {80 + i}.0 Driver {i}".encode(), ("127.0.0.1", CTL))
    tun.sendto(b"LAP 1 jarama road car 2.0 Glitch", ("127.0.0.1", CTL))
    time.sleep(0.4)
    times = records(WEB_A)["jarama"]["times"]
    check(times[0]["name"] == "Bilko" and times[0]["time"] == 70.5 and times[0]["car"] == "ls1",
          "a better lap replaces the driver's old one; a worse one does not")
    check(len(times) == 10 and [t["time"] for t in times] == sorted(t["time"] for t in times),
          "ten best per track, fastest first")
    check(all(t["name"] != "Glitch" for t in times), "impossible lap times are refused")
    check(records(WEB_B)["jarama"]["times"] == times, "backup has exactly the same table")

    # Restart A: records come back from disk.
    proc.terminate(); proc.wait(5)
    proc = start_a(); time.sleep(0.4)
    check(records(WEB_A)["jarama"]["times"] == times, "records survive a restart")

    # B loses everything; on start it asks A, and A sends it all back.
    proc_b.terminate(); proc_b.wait(5)
    shutil.rmtree(DATA_B); os.makedirs(DATA_B)
    proc_b = start_b(); time.sleep(1.0)
    got_b = records(WEB_B).get("jarama", {}).get("times")
    if got_b != times: print("B:", got_b, "\nA:", times)
    check(got_b == times, "a wiped relay is refilled by its peer")

    try:
        urllib.request.urlopen(f"http://127.0.0.1:{WEB_A}/nothing", timeout=3)
        code = 200
    except urllib.error.HTTPError as e:
        code = e.code
    check(code == 404, "unknown paths are 404")
    with urllib.request.urlopen(f"http://127.0.0.1:{WEB_A}/sessions", timeout=3) as r:
        check("sessions" in json.load(r), "/sessions answers")

    stop = True
    t.join()
    tun.settimeout(2)
    tun.sendto(f"BYE {sid}".encode(), ("127.0.0.1", CTL))
    time.sleep(0.2)
    lst.sendto(b"LIST", ("127.0.0.1", CTL))
    check(lst.recv(2048) == b"LIST\n", "BYE removes the session")

    # Silent host times out.
    t2 = socket.socket(socket.AF_INET, socket.SOCK_DGRAM); t2.settimeout(2)
    t2.sendto(b"HOST 1 4 forza quiet", ("127.0.0.1", CTL))
    check(t2.recv(2048).startswith(b"OK"), "second host registers")
    print("waiting 17s for the session timeout...")
    time.sleep(17)
    lst.sendto(b"LIST", ("127.0.0.1", CTL))
    check(lst.recv(2048) == b"LIST\n", "silent host is timed out")
finally:
    for p_ in (proc, proc_b):
        p_.terminate()
        p_.wait(5)
    shutil.rmtree(DATA, ignore_errors=True)

print("ALL PASSED" if ok else "SOME TESTS FAILED")
sys.exit(0 if ok else 1)
