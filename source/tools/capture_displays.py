#!/usr/bin/env python3
"""Read-only survey of the displays an NBT Evo drives, to find a path to the HUD and cluster.

On F/G-series cars the HUD, and on 6WB cars the cluster's map area, show pictures
the head unit draws. This collects the layer manager's screens and scene, the
running processes, the network interfaces and the names of HUD, cluster and video
link files, so the paths can be studied off the car. Every command only reads.
Output stays in a private local folder: it can hold MAC and IP addresses.
"""

import argparse
import json
from pathlib import Path
import re
import subprocess

from capture_headunit import enet_ip, ssh_read

LMC = "/opt/sys/bin/LayerManagerControl"
MAX_SCREENS = 8
RESOLUTION = re.compile(rb"resolution:\s*x\s*=\s*([0-9]{2,4}),\s*y\s*=\s*([0-9]{2,4})\b", re.I)
SCREEN_ID = re.compile(rb"\bscreen\s+(\d+)\b", re.I)
NAMES = ("*hud*", "*HUD*", "*kombi*", "*KOMBI*", "*cluster*", "*apix*", "*APIX*", "*ramses*", "*dcsm*")
SEARCH_DIRS = "/opt/sys /opt/hmi /etc /dev"
# One name per step, so the saved raw output says what each one is.
COMMANDS = {
    "screens": LMC + " get screens",
    "scene": LMC + " get scene",
    "processes": "pidin -f an",
    "process_args": "pidin arg",
    "network": "ifconfig -a",
    "devices": "ls /dev",
    "named_files": "find " + SEARCH_DIRS + " -maxdepth 4 \\( "
                   + " -o ".join("-name '" + n + "'" for n in NAMES) + " \\) 2>/dev/null",
}
KEYWORDS = re.compile(rb"hud|kombi|cluster|apix|ramses|dcsm|someip", re.I)


def screen_ids(screens_text):
    """Screen ids the layer manager lists, or 0..MAX_SCREENS-1 when it lists none."""
    ids = sorted({int(n) for n in SCREEN_ID.findall(screens_text)})
    return ids or list(range(MAX_SCREENS))


def survey(read):
    raw = {name: read(command, optional=True) for name, command in COMMANDS.items()}
    screens = []
    for sid in screen_ids(raw["screens"]):
        text = read(f"{LMC} get screen {sid}", optional=True)
        if not text.strip():
            continue
        raw[f"screen_{sid}"] = text
        match = RESOLUTION.search(text)
        screens.append({"id": sid, "width": int(match.group(1)) if match else None,
                        "height": int(match.group(2)) if match else None})
    hits = sorted({line.strip().decode(errors="replace")
                   for name in ("processes", "process_args", "devices", "named_files")
                   for line in raw[name].splitlines() if KEYWORDS.search(line)})
    summary = {
        "screens": screens,
        "keyword_hits": hits[:400],
        "missing": sorted(name for name, data in raw.items() if not data.strip()),
    }
    return summary, raw


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--head", required=True, type=enet_ip, help="head unit SSH IP")
    parser.add_argument("--out", required=True, type=Path, help="new private output directory")
    parser.add_argument("--legacy", action="store_true", help="allow legacy SSH algorithms for this ENET connection only")
    args = parser.parse_args()
    try:
        if args.out.exists():
            raise ValueError("output directory already exists")
        summary, raw = survey(lambda command, optional=False: ssh_read(args.head, args.legacy, command, optional))
        args.out.mkdir(mode=0o700)
        (args.out / "displays.json").write_text(json.dumps(summary, indent=2) + "\n", encoding="utf-8")
        for name, data in raw.items():
            (args.out / f"{name}.txt").write_bytes(data)
        print(f"Found {len(summary['screens'])} screen(s); saved the survey to {args.out}")
    except (OSError, ValueError, subprocess.TimeoutExpired) as error:
        raise SystemExit(str(error)) from None


if __name__ == "__main__":
    main()
