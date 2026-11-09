#!/usr/bin/env python3
"""Validate locally built QNX artifacts and stage them for the desktop installer."""

import argparse
from pathlib import Path
from shutil import copyfile
import struct
from xml.etree import ElementTree
from zipfile import ZipFile

ROOT = Path(__file__).resolve().parents[1]
APPS = (
    ("BoostDeck", "boostdeck-collector-qnx", "100"),
    ("ShiftDeck", "shift-deck-qnx", "104"),
    ("MhdMaps", "mhd-maps-qnx", "103"),
    ("WarmUp", "warm-up-qnx", "105"),
    ("PerfTimer", "perf-timer-qnx", "106"),
    ("FaultScan", "fault-scan-qnx", "107"),
)


def require_arm(path, elf_type):
    with path.open("rb") as binary:
        head = binary.read(20)
    if head[:6] != b"\x7fELF\x01\x01" or len(head) != 20:
        raise ValueError(f"{path}: expected a little-endian ELF32 file")
    if struct.unpack_from("<HH", head, 16) != (elf_type, 40):
        raise ValueError(f"{path}: unexpected ARM ELF type")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--check", action="store_true", help="verify the staged payload")
    args = parser.parse_args()
    native = ROOT / "native"
    payload = ROOT / "payload"
    copies = []
    for app, worker, connector in APPS:
        destination = payload / app
        plugin = destination if args.check else native / "plugin"
        binary = destination if args.check else native / "out"
        manifest = plugin / f"{app}.manifest"
        description = plugin / f"{app}.xml"
        images = plugin / f"{app}_ALL_Images.zip"
        library = binary / f"lib{app}.so"
        executable = binary / worker
        config = ElementTree.parse(manifest)
        if config.findtext("name") != app or config.findtext("config/dependencies/pluginAppConnector/id") != connector:
            raise ValueError(f"{manifest}: app name or connector mismatch")
        ids = {f'{node.attrib["imageId"]}.png' for node in ElementTree.parse(description).iter("imageIdModel")}
        with ZipFile(images) as archive:
            if archive.testzip() is not None or not ids <= set(archive.namelist()):
                raise ValueError(f"{images}: invalid archive or missing initial image")
        require_arm(library, 3)
        require_arm(executable, 2)
        copies.extend((path, destination / path.name) for path in (manifest, description, images, library, executable))
    helper = payload / "tools" / "gaugedeck-fileops"
    executable = helper if args.check else native / "out" / helper.name
    require_arm(executable, 2)
    copies.append((executable, helper))
    if not args.check:
        for source, destination in copies:
            destination.parent.mkdir(exist_ok=True)
            copyfile(source, destination)
    print(f"Verified all {len(APPS)} app payloads and the ARM helper.")


if __name__ == "__main__":
    try:
        main()
    except (OSError, ValueError, ElementTree.ParseError) as error:
        raise SystemExit(f"{error}\nSee docs/BUILD.md for the required build inputs.") from None
