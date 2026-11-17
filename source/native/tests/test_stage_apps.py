"""Stage the changed and new apps from the published package and a stand-in QNX build."""
from io import BytesIO
from pathlib import Path
import sys
import tempfile
import unittest
import zipfile

TOOLS = Path(__file__).resolve().parents[1] / "tools"
sys.path.insert(0, str(TOOLS))
from make_text_apps import FONTS, GEAR_LEGEND  # noqa: E402
from stage_apps import APPS, PLUGIN, stage  # noqa: E402

PACKAGE = Path(__file__).resolve().parents[3] / "packages" / "dashboard.zip"
HAVE_FONT = any(Path(font).is_file() for font in FONTS)


@unittest.skipUnless(HAVE_FONT and all((PLUGIN / f"{app}_ALL_Images.zip").is_file()
                                       for app in ("WarmUp", "PerfTimer", "FaultScan")),
                     "needs a legend font and make_text_apps.py output")
class StageTest(unittest.TestCase):
    def setUp(self):
        self.directory = tempfile.TemporaryDirectory()
        self.root = Path(self.directory.name)
        self.build = self.root / "out"
        self.build.mkdir()
        for binaries in APPS.values():
            for name in binaries:
                (self.build / name).write_bytes(b"stand-in " + name.encode())

    def tearDown(self):
        self.directory.cleanup()

    def test_stages_every_app_with_the_legend_and_gateway(self):
        target = self.root / "stage"
        self.assertEqual(stage(PACKAGE, self.build, "169.254.10.3", target), sorted(APPS))
        with zipfile.ZipFile(PACKAGE) as published:
            original = zipfile.ZipFile(BytesIO(published.read("ShiftDeck/ShiftDeck_ALL_Images.zip")))
            self.assertEqual((target / "BoostDeck/BoostDeck_ALL_Images.zip").read_bytes(),
                             published.read("BoostDeck/BoostDeck_ALL_Images.zip"))
        staged = zipfile.ZipFile(target / "ShiftDeck/ShiftDeck_ALL_Images.zip")
        self.assertEqual(set(staged.namelist()), set(original.namelist()) | {f"{GEAR_LEGEND}.png"})
        for name in original.namelist():
            self.assertEqual(staged.read(name), original.read(name))
        for app, binaries in APPS.items():
            folder = target / app
            self.assertEqual((folder / f"{app}.xml").read_bytes(), (PLUGIN / f"{app}.xml").read_bytes())
            self.assertEqual((folder / "gateway-ip.txt").read_text(), "169.254.10.3\n")
            for name in binaries:
                self.assertEqual((folder / name).read_bytes(), b"stand-in " + name.encode())
        self.assertIn(b"<id>108</id>", (target / "FaultScan/FaultScan.manifest").read_bytes())

    def test_refuses_a_partial_build_or_a_used_folder(self):
        (self.build / "fault-scan-qnx").unlink()
        with self.assertRaises(FileNotFoundError):
            stage(PACKAGE, self.build, "169.254.10.3", self.root / "stage")
        self.assertFalse((self.root / "stage").exists())
        (self.build / "fault-scan-qnx").write_bytes(b"stand-in")
        used = self.root / "used"
        used.mkdir()
        (used / "keep.txt").write_text("x")
        with self.assertRaises(FileExistsError):
            stage(PACKAGE, self.build, "169.254.10.3", used)


if __name__ == "__main__":
    unittest.main()
