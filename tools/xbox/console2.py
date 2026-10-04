#!/usr/bin/env python3
"""console.py for two consoles (docs/testing.md "Two consoles"): one build on
console A and console B, their logs side by side.

    tools/xbox/console2.py stage v60            # build-xbox -> hw/stage-v60 (as console.py stage)
    tools/xbox/console2.py deploy v60           # upload to A, then B (deletes their old logs first)
    tools/xbox/console2.py pull v60             # A's logs -> hw/logs60-a, B's -> hw/logs60-b
    tools/xbox/console2.py ls                   # both log folders
    tools/xbox/console2.py deploy v60 --only b  # one console (a or b)

Console A is MX_FTP_HOST, console B MX_FTP_HOST_B (their IPs; the dashboard
shows them), both xbox/xbox unless MX_FTP_USER/MX_FTP_PASS
(MX_FTP_USER_B/MX_FTP_PASS_B for B) say otherwise. B's folders default to
A's (/F/Applications/Melee-X, /E/UDATA/4d580001); MX_FTP_APP_B moves its
XBE folder (another dashboard, another partition). Both consoles must run
the same XBE (docs/lan-plan.md D6), so there is one stage folder. Pull
before every boot: each boot deletes the previous boot's logs.
"""
import os
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import console  # noqa: E402  (same folder: its stage, its FTP helpers)

CONSOLES = {
    "a": dict(host=os.environ.get("MX_FTP_HOST", ""), user=os.environ.get("MX_FTP_USER", "xbox"),
              pw=os.environ.get("MX_FTP_PASS", "xbox"), app=console.APP),
    "b": dict(host=os.environ.get("MX_FTP_HOST_B", ""), user=os.environ.get("MX_FTP_USER_B", "xbox"),
              pw=os.environ.get("MX_FTP_PASS_B", "xbox"), app=os.environ.get("MX_FTP_APP_B", console.APP)),
}


def use(side):
    c = CONSOLES[side]
    if not c["host"]:
        sys.exit(f"console {side.upper()}: set {'MX_FTP_HOST' if side == 'a' else 'MX_FTP_HOST_B'} to its IP address")
    console.HOST, console.USER, console.PASS = c["host"], c["user"], c["pw"]
    console.APP = c["app"]
    console.FILES = {"default.xbe": c["app"], "default.tbn": c["app"],
                     "TitleImage.xbx": console.UDATA, "TitleMeta.xbx": console.UDATA}
    print(f"== console {side.upper()} ({c['host']})")


def pull(side, v):
    out = console.HW / f"logs{console.ver(v)[1:]}-{side}"
    out.mkdir(parents=True, exist_ok=True)
    f = console.connect()
    names = console.logs(f)
    for n in names:
        with open(out / n, "wb") as fh:
            f.retrbinary(f"RETR {console.UDATA}/{n}", fh.write)
        print(f"got {n} ({(out / n).stat().st_size} bytes)")
    f.quit()
    print(f"{len(names)} files in {out}")


def main():
    args = sys.argv[1:]
    only = None
    if "--only" in args:
        i = args.index("--only")
        only = args[i + 1].lower() if i + 1 < len(args) else ""
        del args[i:i + 2]
        if only not in CONSOLES:
            sys.exit("--only a or --only b")
    if not args or args[0] not in ("stage", "deploy", "pull", "ls"):
        sys.exit(__doc__)
    cmd = args[0]
    if cmd == "stage":
        if len(args) < 2:
            sys.exit(__doc__)
        console.stage(args[1])
        return
    if cmd != "ls" and len(args) < 2:
        sys.exit(__doc__)
    for side in ([only] if only else ["a", "b"]):
        use(side)
        if cmd == "ls":
            f = console.connect()
            print("\n".join(console.logs(f)) or "(no logs)")
            f.quit()
        elif cmd == "deploy":
            console.deploy(args[1])
        else:
            pull(side, args[1])


if __name__ == "__main__":
    main()
