#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""drive.py: the game's flat screens driven from the PC, a step at a time (a debug build).

  drive.py start [TITLE]     the console checked (no game open), the payload sent unless one of
                             ours is resident, the run's cvars.txt uploaded, the game launched,
                             the kernel log streamed to <dir>/klog
  drive.py shot [NAME]       agent.shot: the game's flat picture -> <dir>/NAME.raw and .png
  drive.py press HEX [MS]    agent.press: the buttons held for MS ms (default 150)
  drive.py tap NAME [MS]     the same by name: cross circle square triangle up down left right
                             options l1 r1 l2 r2 l3 r3 touch,
                             lsleft lsright lsup lsdown (the left stick), rsleft rsright (the right)
  drive.py log [N]           the agent's log fetched to <dir>/agent.log, its last N lines shown
  drive.py stop              the game closed (the control port's kill), the log fetched, the
                             console's cvars.txt removed; the payload stays resident (every send
                             of a payload is a moment the console has gone down in: one send a turn)
  drive.py quit              the payload asked to quit (handing the console to another session;
                             wait a minute after any payload quits before another is sent)

The agent reads cvars.txt every 2 s, so a press or a shot takes effect within about 2.5 s.
PS5_HOST from the environment or tools/local.env; DRIVE_DIR (default logs/drive) holds the state
(drive.json: the counters), the pictures and the logs. DRIVE_CVARS: lines every upload carries
(default "agent.novr2=1\\nagent.hands=0": an observing run, the palm tracker watch only).
"""
import ftplib
import io
import json
import os
import socket
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
DIR = os.environ.get("DRIVE_DIR", os.path.join(ROOT, "logs", "drive"))
CONS = "/data/doax3vr2"
PORT = 9089
TITLES = ("CUSA04555", "CUSA14173", "CUSA14831")
BUTTONS = {"l3": 0x2, "r3": 0x4, "options": 0x8, "up": 0x10, "right": 0x20, "down": 0x40,
           "left": 0x80, "l2": 0x100, "r2": 0x200, "l1": 0x400, "r1": 0x800, "triangle": 0x1000,
           "circle": 0x2000, "cross": 0x4000, "square": 0x8000, "touch": 0x100000,
           # the left stick pushed fully (the agent sets the stick, not a button)
           "lsleft": 0x1000000, "lsright": 0x2000000, "lsup": 0x4000000, "lsdown": 0x8000000,
           "rsleft": 0x20000000, "rsright": 0x40000000,
           # the controller shaken up and down for the press's length (the agent's acceleration)
           "shake": 0x10000000}


def host():
    h = os.environ.get("PS5_HOST")
    env = os.path.join(HERE, "local.env")
    if not h and os.path.exists(env):
        for line in open(env):
            k, _, v = line.strip().partition("=")
            if k.replace("export ", "") == "PS5_HOST":
                h = v.strip("\"'")
    if not h:
        sys.exit("PS5_HOST is not set (tools/local.env)")
    return h


def ctl(cmd, port=PORT, wait=5):
    try:
        s = socket.create_connection((host(), port), timeout=wait)
    except OSError:
        return None
    s.settimeout(wait)
    s.sendall(cmd.encode() + b"\n")
    out = b""
    try:
        while True:
            b = s.recv(4096)
            if not b:
                break
            out += b
    except OSError:
        pass
    s.close()
    return out.decode(errors="replace").strip()


def ftp():
    f = ftplib.FTP()
    f.connect(host(), 2121, timeout=30)
    f.login()
    return f


def listing(f, path):
    lines = []
    try:
        f.retrlines("LIST " + path, lines.append)
    except ftplib.all_errors:
        return {}
    out = {}
    for l in lines:
        p = l.split(None, 8)
        if len(p) == 9:
            out[p[8]] = (int(p[4]), " ".join(p[5:8]))
    return out


def state():
    p = os.path.join(DIR, "drive.json")
    return json.load(open(p)) if os.path.exists(p) else {"shot": 0, "press": 0, "press_line": ""}


def save(st):
    os.makedirs(DIR, exist_ok=True)
    json.dump(st, open(os.path.join(DIR, "drive.json"), "w"))


def upload_cvars(st):
    base = os.environ.get("DRIVE_CVARS", "agent.novr2=1\nagent.hands=0\nagent.errdialog=0")
    text = base.rstrip("\n") + "\n"
    if st["shot"]:
        text += "agent.shot=%d\n" % st["shot"]
    if st["press_line"]:
        text += st["press_line"] + "\n"
    f = ftp()
    f.storbinary("STOR %s/cvars.txt" % CONS, io.BytesIO(text.encode()))
    f.quit()


def fetch_log(n=0):
    os.makedirs(DIR, exist_ok=True)
    f = ftp()
    out = os.path.join(DIR, "agent.log")
    with open(out, "wb") as o:
        f.retrbinary("RETR %s/agent.log" % CONS, o.write)
    f.quit()
    if n:
        lines = [l for l in open(out, errors="replace").read().splitlines()
                 if not l.startswith(("pad:", "head:"))]
        print("\n".join(l[:200] for l in lines[-n:]))


def start(title):
    if title not in TITLES:
        sys.exit("not a known title: " + title)
    f = ftp()
    mounted = [n for n in listing(f, "/mnt/sandbox/pfsmnt/") if n[:4] in ("CUSA", "PPSA")]
    f.quit()
    if mounted:
        sys.exit("a game is open (%s): close it first" % ", ".join(mounted))
    for port in (9087, 9088):
        if ctl("probe", port, 3) == "yes":
            sys.exit("another VR payload is resident on port %d: not starting" % port)
    if ctl("probe") != "yes":
        elf = os.path.join(ROOT, "payload", "doax3vr2.elf")
        print("sending", elf)
        s = socket.create_connection((host(), 9021), timeout=10)
        s.sendall(open(elf, "rb").read())
        s.close()
        for _ in range(30):
            time.sleep(1)
            if ctl("probe", wait=3) == "yes":
                break
        else:
            sys.exit("the payload did not come up")
        time.sleep(3)
    else:
        print("keeping the resident payload")
    ps = ctl("ps") or ""
    n = sum("payload.elf" in l for l in ps.splitlines())
    want = 1 + int(os.environ.get("PS5_OTHER_PAYLOADS", "0"))
    if n != want:
        sys.exit("%d payload.elf processes, %d expected: not launching\n%s" % (n, want, ps))
    st = {"shot": 0, "press": 0, "press_line": ""}
    save(st)
    upload_cvars(st)
    f = ftp()
    # (agent.elf: one a runner left would go in instead of the payload's built-in agent)
    for name in ("test.txt", "shot.raw", "agent.elf"):
        try:
            f.delete("%s/%s" % (CONS, name))
        except ftplib.all_errors:
            pass
    # the test words (test.txt, read once at the game's start): "worn" by default, the headset on
    # the desk counted as on a head (DRIVE_TEST="" for none)
    words = os.environ.get("DRIVE_TEST", "worn")
    if words:
        f.storbinary("STOR %s/test.txt" % CONS, io.BytesIO((words + "\n").encode()))
    # DRIVE_AGENT=<agent.elf>: that agent goes in instead of the resident payload's built-in one
    # (an agent change tried without sending a payload again)
    agent = os.environ.get("DRIVE_AGENT")
    if agent:
        with open(agent, "rb") as a:
            f.storbinary("STOR %s/agent.elf" % CONS, a)
        print("agent.elf uploaded:", agent, os.path.getsize(agent), "bytes")
    f.quit()
    subprocess.run(["pkill", "-f", "nc %s 3232" % host()])
    klog = open(os.path.join(DIR, "klog"), "wb")
    subprocess.Popen("sleep 7200 | nc %s 3232" % host(), shell=True, stdout=klog,
                     stderr=subprocess.DEVNULL, start_new_session=True)
    time.sleep(2)
    print("launch:", ctl("launch " + title))


def shot(name):
    st = state()
    f = ftp()
    try: # (the last one gone: a new file is this request's)
        f.delete(CONS + "/shot.raw")
    except ftplib.all_errors:
        pass
    st["shot"] += 1
    save(st)
    upload_cvars(st)
    raw = os.path.join(DIR, (name or "shot-%03d" % st["shot"]) + ".raw")
    for _ in range(40):
        time.sleep(0.5)
        now = listing(f, CONS + "/").get("shot.raw")
        if now:
            break
    else:
        f.quit()
        sys.exit("no new shot.raw in 20 s (see the log)")
    time.sleep(0.3)
    with open(raw, "wb") as o:
        f.retrbinary("RETR %s/shot.raw" % CONS, o.write)
    f.quit()
    print(raw, os.path.getsize(raw), "bytes")
    # a texture of another size (a composed eye): its size and pitch from the agent's log line
    size = os.path.getsize(raw)
    import re
    fetch_log() # (the log's line says what it is: a size alone can be two things)
    array = False
    for l in open(os.path.join(DIR, "agent.log"), errors="replace"):
        k = re.search(r"^shot %d: .* (\d+) slice" % st["shot"], l)
        if k:
            array = k.group(1) == "4"
    if size != 8847360 and not array:
        m = None
        for l in open(os.path.join(DIR, "agent.log"), errors="replace"):
            k = re.search(r"^shot %d: .* (\d+)x(\d+) pitch (\d+), 1 slice" % st["shot"], l)
            m = k or m
        if m:
            ppm = raw[:-4] + ".ppm"
            subprocess.run([os.path.join(HERE, "detile"), raw, ppm, m.group(1), m.group(2), "16", "2",
                            "thin", m.group(3)], stderr=subprocess.DEVNULL)
            subprocess.run(["sips", "-s", "format", "png", ppm, "--out", raw[:-4] + ".png"],
                           stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
            os.remove(ppm)
            print(raw[:-4] + ".png", m.group(1) + "x" + m.group(2))
        return
    subprocess.run(["sh", os.path.join(HERE, "shot2png.sh"), raw])


def press(hexv, ms):
    st = state()
    st["press"] += 1
    st["press_line"] = "agent.press=%d:%x:%d" % (st["press"], hexv, ms)
    save(st)
    upload_cvars(st)
    print(st["press_line"])


def stop():
    print("kill:", ctl("kill"))
    time.sleep(4)
    try:
        fetch_log(12)
    except ftplib.all_errors as e:
        print("log not fetched:", e)
    f = ftp()
    try:
        f.delete(CONS + "/cvars.txt")
    except ftplib.all_errors:
        pass
    mounted = [n for n in listing(f, "/mnt/sandbox/pfsmnt/") if n[:4] in ("CUSA", "PPSA")]
    f.quit()
    subprocess.run(["pkill", "-f", "nc %s 3232" % host()])
    if mounted:
        print("still mounted:", mounted, ": the payload left resident")
        return
    if os.environ.get("DRIVE_QUIT") == "1":
        print("quit:", ctl("quit"))
    else:
        print("the payload stays resident (drive.py quit, when handing the console over)")


def main():
    a = sys.argv[1:]
    os.makedirs(DIR, exist_ok=True)
    if not a:
        sys.exit(__doc__)
    if a[0] == "start":
        start(a[1] if len(a) > 1 else TITLES[0])
    elif a[0] == "shot":
        shot(a[1] if len(a) > 1 else "")
    elif a[0] == "press":
        press(int(a[1], 16), int(a[2]) if len(a) > 2 else 150)
    elif a[0] == "tap":
        v = 0
        for n in a[1].split("+"):
            v |= BUTTONS[n]
        press(v, int(a[2]) if len(a) > 2 else 150)
    elif a[0] == "log":
        fetch_log(int(a[1]) if len(a) > 1 else 15)
    elif a[0] == "stop":
        stop()
    elif a[0] == "quit":
        print("quit:", ctl("quit"))
    else:
        sys.exit(__doc__)


if __name__ == "__main__":
    main()
