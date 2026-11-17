"""Rescale a packaged RHMI app folder to another iDrive panel size.

The raw dashboard ZIP targets 1280 x 480. Other NBTevo panels keep the same
aspect ratio (1440 x 540 is exactly 9/8), so a port scales the component
geometry in the layout XML and resamples the PNGs in the image archive by the
same factor. Everything else is copied byte for byte.

The plugin libraries and workers refer to model, image, action, and component
IDs only (see src/shift_assets.h and src/dial_scales.h), so a layout port needs
no native rebuild and keeps every ID unchanged. A box and its image are scaled
together, which preserves however the head unit fits the image to the box.

    python tools/rescale_layout.py APP_DIR OUT_DIR --to 1440x540
"""
import argparse
import math
import shutil
from fractions import Fraction
from io import BytesIO
from pathlib import Path
from xml.etree import ElementTree as ET
from zipfile import ZipFile, ZipInfo

from PIL import Image

REFERENCE = (1280, 480)
# Component properties: 20 = x, 21 = y, 9 = width, 10 = height.
GEOMETRY = ("20", "21", "9", "10")


def parse_size(text):
    width, _, height = text.lower().partition("x")
    return int(width), int(height)


def factor(source, target):
    sx, sy = Fraction(target[0], source[0]), Fraction(target[1], source[1])
    if sx != sy:
        raise ValueError(f"{source[0]}x{source[1]} -> {target[0]}x{target[1]} changes the aspect ratio")
    return sx


def scaled(value, scale):
    # Round half up so 0.5 steps do not alternate the way round() does.
    return math.floor(value * scale + Fraction(1, 2))


def plugin_files(app):
    """Return the layout XML and image archive named by the app's manifest."""
    manifests = list(app.glob("*.manifest"))
    if len(manifests) != 1:
        raise ValueError(f"{app}: expected one .manifest, found {len(manifests)}")
    params = ET.parse(manifests[0]).getroot().find("config/parameters")

    def local(tag):
        node = params.find(tag)
        return None if node is None else app / node.text.replace("{pluginDir}/", "")

    return local("DescFile"), local("ImageDBFile")


def rescale_xml(source, target, scale):
    tree = ET.parse(source)
    boxes = []
    for properties in tree.getroot().iter("properties"):
        values = {node.get("id"): node for node in properties.findall("property")}
        if not all(key in values for key in GEOMETRY):
            continue
        x, y, w, h = (int(values[key].get("value")) for key in GEOMETRY)
        for key, value in zip(GEOMETRY, (x, y, w, h)):
            values[key].set("value", str(scaled(value, scale)))
        boxes.append(tuple(scaled(v, scale) for v in (x, y, w, h)))
    ET.indent(tree, space="  ")
    # A binary handle keeps LF line endings on Windows as well.
    with open(target, "wb") as handle:
        tree.write(handle, encoding="utf-8", xml_declaration=True)
    return boxes


def rescale_archive(source, target, scale):
    with ZipFile(source) as base, ZipFile(target, "w") as archive:
        for entry in base.infolist():
            data = base.read(entry)
            if entry.filename.lower().endswith(".png"):
                image = Image.open(BytesIO(data))
                size = tuple(max(1, scaled(v, scale)) for v in image.size)
                if size != image.size:
                    if image.mode not in ("RGBA", "RGB", "L", "LA"):
                        image = image.convert("RGBA")
                    # Pillow premultiplies alpha for RGBA/LA before resampling.
                    image = image.resize(size, Image.Resampling.LANCZOS)
                    out = BytesIO()
                    image.save(out, format="PNG", optimize=True)
                    data = out.getvalue()
            info = ZipInfo(entry.filename, date_time=entry.date_time)
            info.compress_type = entry.compress_type
            info.external_attr = entry.external_attr
            archive.writestr(info, data)


def rescale_app(app, out, source_size=REFERENCE, target_size=(1440, 540)):
    app, out = Path(app), Path(out)
    scale = factor(source_size, target_size)
    layout, images = plugin_files(app)
    if layout is None or not layout.is_file():
        raise ValueError(f"{app}: manifest DescFile is missing")
    out.mkdir(parents=True, exist_ok=True)
    for item in app.iterdir():
        if item.is_file() and item not in (layout, images) and not item.name.startswith("._"):
            shutil.copy2(item, out / item.name)
    boxes = rescale_xml(layout, out / layout.name, scale)
    if images is not None and images.is_file():
        rescale_archive(images, out / images.name, scale)
    right = max((x + w for x, _, w, _ in boxes), default=0)
    bottom = max((y + h for _, y, _, h in boxes), default=0)
    if right > target_size[0] or bottom > target_size[1]:
        raise ValueError(f"{app.name}: layout reaches {right}x{bottom}, beyond the panel")
    return len(boxes), (right, bottom)


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("app", type=Path, help="packaged app folder with a .manifest")
    parser.add_argument("out", type=Path, help="output folder for the rescaled app")
    parser.add_argument("--from", dest="source", type=parse_size, default=REFERENCE,
                        help="panel size the app was laid out for (default 1280x480)")
    parser.add_argument("--to", dest="target", type=parse_size, required=True,
                        help="panel size to lay out for, e.g. 1440x540")
    args = parser.parse_args()
    count, (right, bottom) = rescale_app(args.app, args.out, args.source, args.target)
    print(f"{args.app.name}: {count} components scaled, extent {right}x{bottom} -> {args.out}")


if __name__ == "__main__":
    main()
