#!/usr/bin/env python3
"""Deploy a build to the console and pull its logs over FTP (any OS).

    tools/xbox/console.py stage v33            # build-xbox -> hw/stage-v33 + hw/melee_x.v33.map
    tools/xbox/console.py deploy v33           # upload hw/stage-v33, verify (deletes old logs/shots first)
    tools/xbox/console.py pull v33             # logs + shots -> hw/logs33
    tools/xbox/console.py ls                   # what's in the log folder

hw/ is ~/xemu/hw unless MX_HW is set. The console's FTP server is at
MX_FTP_HOST (its IP, shown by the dashboard), login xbox/xbox unless
MX_FTP_USER / MX_FTP_PASS say otherwise.
Deploy puts default.xbe and default.tbn in /F/Applications/Melee-X/ next to
the disc image, and TitleImage.xbx and TitleMeta.xbx in /E/UDATA/4d580001/
(UnleashX's icon cache). Logs (boot*.log, trace.log, crash.log, hang.log,
shotNN.bmp) are in /E/UDATA/4d580001/. Keep each deployed build's map: sym.py
and prof_report.py need the map of the build that wrote the log.
"""
import ftplib
import io
import os
import re
import shutil
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
HW = Path(os.environ.get("MX_HW", Path.home() / "xemu" / "hw"))
HOST = os.environ.get("MX_FTP_HOST", "")
USER = os.environ.get("MX_FTP_USER", "xbox")
PASS = os.environ.get("MX_FTP_PASS", "xbox")
APP = "/F/Applications/Melee-X"
UDATA = "/E/UDATA/4d580001"
LOGS = re.compile(r"^(shot\d+\.bmp|boot(\d|_prev)?\.log|trace\.log|crash\.log|hang\.log)$")
FILES = {"default.xbe": APP, "default.tbn": APP, "TitleImage.xbx": UDATA, "TitleMeta.xbx": UDATA}


def ver(v):
    return v if v.startswith("v") else "v" + v


def connect():
    if not HOST:
        sys.exit("set MX_FTP_HOST to the Xbox's IP address (the dashboard shows it)")
    try:
        f = ftplib.FTP(HOST, timeout=15)
    except OSError as e:
        sys.exit(f"console not reachable at {HOST} ({e}): is the Xbox on, with its FTP server up?")
    f.login(USER, PASS)
    return f


def logs(f):
    # the console's FTP server lists the current directory whatever path NLST is given
    f.cwd(UDATA)
    return sorted(n for n in (p.rsplit("/", 1)[-1] for p in f.nlst()) if LOGS.match(n))


def stage(v):
    v = ver(v)
    out = HW / f"stage-{v}"
    out.mkdir(parents=True, exist_ok=True)
    for name in FILES:
        shutil.copy2(ROOT / "build-xbox" / "xbe" / name, out / name)
    shutil.copy2(ROOT / "build-xbox" / "melee_x.map", HW / f"melee_x.{v}.map")
    # static functions too, from this build's objects (static_syms.py)
    subprocess.run([sys.executable, str(ROOT / "tools/xbox/static_syms.py"), "--map", str(HW / f"melee_x.{v}.map")])
    print(f"staged {out} and melee_x.{v}.map")


def deploy(v):
    src = HW / f"stage-{ver(v)}"
    f = connect()
    old = logs(f)
    for n in old:
        f.delete(f"{UDATA}/{n}")
    print(f"deleted {len(old)} old logs/shots (pull them first if you need them)")
    for name, d in FILES.items():
        data = (src / name).read_bytes()
        f.storbinary(f"STOR {d}/{name}", io.BytesIO(data))
        back = io.BytesIO()
        f.retrbinary(f"RETR {d}/{name}", back.write)
        if back.getvalue() != data:
            sys.exit(f"{name} MISMATCH after upload")
        print(f"{name} ok ({len(data)} bytes)")
    f.quit()


def pull(v):
    out = HW / f"logs{ver(v)[1:]}"
    out.mkdir(parents=True, exist_ok=True)
    f = connect()
    names = logs(f)
    for n in names:
        with open(out / n, "wb") as fh:
            f.retrbinary(f"RETR {UDATA}/{n}", fh.write)
        print(f"got {n} ({(out / n).stat().st_size} bytes)")
    f.quit()
    print(f"{len(names)} files in {out}")


def main():
    if len(sys.argv) < 2 or sys.argv[1] not in ("stage", "deploy", "pull", "ls"):
        sys.exit(__doc__)
    cmd = sys.argv[1]
    if cmd == "ls":
        f = connect()
        print("\n".join(logs(f)) or "(no logs)")
        f.quit()
        return
    if len(sys.argv) < 3:
        sys.exit(__doc__)
    {"stage": stage, "deploy": deploy, "pull": pull}[cmd](sys.argv[2])


if __name__ == "__main__":
    main()
