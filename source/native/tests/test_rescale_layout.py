"""Check that a layout port scales geometry and art and keeps every ID."""
from io import BytesIO
from pathlib import Path
import sys
import tempfile
import unittest
from xml.etree import ElementTree as ET
from zipfile import ZIP_DEFLATED, ZipFile, ZipInfo

from PIL import Image

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools"))
import rescale_layout  # noqa: E402

PLUGIN = Path(__file__).resolve().parents[1] / "plugin"

MANIFEST = """<?xml version='1.0' encoding='utf-8'?>
<plugin>
  <name>Demo</name>
  <config>
    <parameters>
      <DescFile>{pluginDir}/Demo.xml</DescFile>
      <ImageDBFile>{pluginDir}/Demo_ALL_Images.zip</ImageDBFile>
    </parameters>
  </config>
</plugin>
"""

LAYOUT = """<?xml version='1.0' encoding='utf-8'?>
<pluginApps>
  <pluginApp applicationType="VehicleInformation" applicationWeight="210" text="Demo">
    <entryButton id="1" action="10">
      <properties>
        <property id="23" value="2" />
      </properties>
    </entryButton>
    <models>
      <imageIdModel id="60" imageId="55000" />
    </models>
    <hmiStates>
      <hmiState id="100" text="Demo">
        <components>
          <image id="200" model="60">
            <properties>
              <property id="20" value="52" />
              <property id="21" value="287" />
              <property id="9" value="1104" />
              <property id="10" value="90" />
              <property id="2" value="false" />
              <property id="3" value="true" />
            </properties>
          </image>
          <label id="150" model="50" />
        </components>
      </hmiState>
    </hmiStates>
  </pluginApp>
</pluginApps>
"""


def png(size):
    data = BytesIO()
    Image.new("RGBA", size, (255, 69, 72, 255)).save(data, format="PNG")
    return data.getvalue()


def geometry(path):
    found = {}
    for component in ET.parse(path).getroot().iter():
        values = {p.get("id"): p.get("value") for p in component.findall("properties/property")}
        if "20" in values:
            found[component.get("id")] = tuple(int(values[k]) for k in ("20", "21", "9", "10"))
    return found


class RescaleLayoutTest(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.app = Path(self.tmp.name) / "Demo"
        self.app.mkdir()
        (self.app / "Demo.manifest").write_text(MANIFEST)
        (self.app / "Demo.xml").write_text(LAYOUT)
        (self.app / "gateway-ip.txt").write_text("169.254.10.3\n")
        (self.app / "libDemo.so").write_bytes(b"\x7fELF fake")
        with ZipFile(self.app / "Demo_ALL_Images.zip", "w", ZIP_DEFLATED) as archive:
            for name, size in (("55001.png", (240, 12)), ("55000.png", (1104, 80)), ("56002.png", (1, 1))):
                archive.writestr(ZipInfo(name), png(size), ZIP_DEFLATED)

    def tearDown(self):
        self.tmp.cleanup()

    def test_1440x540_scales_boxes_and_art_by_nine_eighths(self):
        out = Path(self.tmp.name) / "out"
        count, extent = rescale_layout.rescale_app(self.app, out, (1280, 480), (1440, 540))
        self.assertEqual(count, 1)
        # 52*9/8 = 58.5 rounds up; 287*9/8 = 322.875; 1104*9/8 = 1242; 90*9/8 = 101.25
        self.assertEqual(geometry(out / "Demo.xml"), {"200": (59, 323, 1242, 101)})
        self.assertEqual(extent, (59 + 1242, 323 + 101))
        with ZipFile(out / "Demo_ALL_Images.zip") as archive:
            self.assertEqual(archive.namelist(), ["55001.png", "55000.png", "56002.png"])
            sizes = [Image.open(BytesIO(archive.read(n))).size for n in archive.namelist()]
            self.assertEqual(sizes, [(270, 14), (1242, 90), (1, 1)])
            self.assertTrue(all(i.compress_type == ZIP_DEFLATED for i in archive.infolist()))

    def test_ids_and_non_geometry_properties_are_unchanged(self):
        out = Path(self.tmp.name) / "out"
        rescale_layout.rescale_app(self.app, out, (1280, 480), (1440, 540))
        source, port = ET.parse(self.app / "Demo.xml").getroot(), ET.parse(out / "Demo.xml").getroot()
        ids = lambda root: [(e.tag, e.get("id"), e.get("model"), e.get("imageId"), e.get("action"))
                            for e in root.iter() if e.tag != "property"]
        self.assertEqual(ids(source), ids(port))
        entry = port.find(".//entryButton/properties/property")
        self.assertEqual((entry.get("id"), entry.get("value")), ("23", "2"))

    def test_other_files_are_copied_byte_for_byte(self):
        out = Path(self.tmp.name) / "out"
        rescale_layout.rescale_app(self.app, out, (1280, 480), (1440, 540))
        for name in ("Demo.manifest", "gateway-ip.txt", "libDemo.so"):
            self.assertEqual((out / name).read_bytes(), (self.app / name).read_bytes(), name)

    def test_identity_scale_round_trips_the_committed_layouts(self):
        for layout in sorted(PLUGIN.glob("*.xml")):
            with self.subTest(layout=layout.name):
                out = Path(self.tmp.name) / layout.name
                rescale_layout.rescale_xml(layout, out, 1)
                self.assertEqual(out.read_bytes(), layout.read_bytes())

    def test_aspect_change_is_refused(self):
        with self.assertRaises(ValueError):
            rescale_layout.rescale_app(self.app, Path(self.tmp.name) / "out", (1280, 480), (1920, 1080))

    def test_layout_beyond_the_panel_is_refused(self):
        (self.app / "Demo.xml").write_text(LAYOUT.replace('value="1104"', 'value="1300"'))
        with self.assertRaises(ValueError):
            rescale_layout.rescale_app(self.app, Path(self.tmp.name) / "out", (1280, 480), (1440, 540))


if __name__ == "__main__":
    unittest.main()
