"""Stage the apps this port changes or adds, ready to copy to the head unit's plugin folder.

    python3 tools/stage_apps.py --package ../../packages/dashboard.zip --build out \
        --gateway 169.254.10.3 STAGE_DIR

For BoostDeck and ShiftDeck it takes the manifest and image archive from the published dashboard
package, the layout from plugin/, and the QNX build from build.sh's out/. ShiftDeck's archive gains
the GEAR legend (55930) its gear readout sits under. Warm-Up, Performance Timer and Fault Scanner
come wholly from plugin/ (run make_text_apps.py first) and out/. Each app also gets gateway-ip.txt.

The result is laid out for 1280 x 480; rescale each folder with rescale_layout.py for another panel.
Nothing is sent to the car.
"""
import argparse
from io import BytesIO
from ipaddress import IPv4Address
from pathlib import Path
import shutil
import sys
import zipfile

sys.path.insert(0, str(Path(__file__).resolve().parent))
from make_text_apps import GEAR_LEGEND, gear_legend  # noqa: E402

PLUGIN = Path(__file__).resolve().parents[1] / "plugin"
# App: (plugin library, worker)
APPS = {
    "BoostDeck": ("libBoostDeck.so", "boostdeck-collector-qnx"),
    "ShiftDeck": ("libShiftDeck.so", "shift-deck-qnx"),
    "WarmUp": ("libWarmUp.so", "warm-up-qnx"),
    "PerfTimer": ("libPerfTimer.so", "perf-timer-qnx"),
    "FaultScan": ("libFaultScan.so", "fault-scan-qnx"),
}
FROM_PACKAGE = ("BoostDeck", "ShiftDeck")


def gateway(value):
    address = IPv4Address(value)
    if not address.is_link_local:
        raise argparse.ArgumentTypeError("use the car's 169.254.x.x diagnostic gateway")
    return str(address)


def with_legend(archive, legend):
    """The image archive with the GEAR legend added; every existing entry is kept byte for byte."""
    out = BytesIO()
    with zipfile.ZipFile(BytesIO(archive)) as source, zipfile.ZipFile(out, "w") as target:
        names = source.namelist()
        for info in source.infolist():
            target.writestr(info, source.read(info), compress_type=info.compress_type)
        if f"{GEAR_LEGEND}.png" not in names:
            image = BytesIO()
            legend.save(image, format="PNG", optimize=True)
            target.writestr(zipfile.ZipInfo(f"{GEAR_LEGEND}.png", (2026, 1, 1, 0, 0, 0)),
                            image.getvalue(), compress_type=zipfile.ZIP_DEFLATED)
    return out.getvalue()


def stage(package, build, gateway_ip, target, font=None):
    missing = [f"{build}/{name}" for app in APPS for name in APPS[app] if not (Path(build) / name).is_file()]
    missing += [str(PLUGIN / f"{app}_ALL_Images.zip") for app in APPS
                if app not in FROM_PACKAGE and not (PLUGIN / f"{app}_ALL_Images.zip").is_file()]
    if missing:
        raise FileNotFoundError("missing: " + ", ".join(missing))
    target = Path(target)
    if target.exists() and any(target.iterdir()):
        raise FileExistsError(f"{target} is not empty")
    with zipfile.ZipFile(package) as published:
        for app, binaries in APPS.items():
            folder = target / app
            folder.mkdir(parents=True)
            images = f"{app}_ALL_Images.zip"
            if app in FROM_PACKAGE:
                (folder / f"{app}.manifest").write_bytes(published.read(f"{app}/{app}.manifest"))
                archive = published.read(f"{app}/{images}")
                if app == "ShiftDeck":
                    archive = with_legend(archive, gear_legend(font))
                (folder / images).write_bytes(archive)
            else:
                shutil.copyfile(PLUGIN / f"{app}.manifest", folder / f"{app}.manifest")
                shutil.copyfile(PLUGIN / images, folder / images)
            shutil.copyfile(PLUGIN / f"{app}.xml", folder / f"{app}.xml")
            for name in binaries:
                shutil.copyfile(Path(build) / name, folder / name)
                (folder / name).chmod(0o755)
            (folder / "gateway-ip.txt").write_text(gateway_ip + "\n", encoding="ascii", newline="\n")
    return sorted(APPS)


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--package", required=True, help="published dashboard.zip")
    parser.add_argument("--build", required=True, help="build.sh output folder (out)")
    parser.add_argument("--gateway", required=True, type=gateway, help="the car's diagnostic gateway IP")
    parser.add_argument("--font", help="font for the GEAR legend, if none of the defaults is installed")
    parser.add_argument("target", help="empty folder to stage into")
    args = parser.parse_args()
    try:
        apps = stage(args.package, args.build, args.gateway, args.target, args.font)
    except (OSError, KeyError, zipfile.BadZipFile) as error:
        parser.error(str(error))
    print(f"Staged {', '.join(apps)} in {args.target}")


if __name__ == "__main__":
    main()
