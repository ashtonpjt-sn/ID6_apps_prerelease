"""Check the layouts the text apps, BoostDeck's More vitals page and ShiftDeck's gear readout rely on.

The workers and plugins name model, action and state IDs in code; these must exist in the RHMI XML.
Connectors and menu weights must stay unique across every app, including the published packages.
"""
from io import BytesIO
from pathlib import Path
import unittest
import zipfile
from xml.etree import ElementTree as ET

PLUGIN = Path(__file__).resolve().parents[1] / "plugin"
PACKAGES = Path(__file__).resolve().parents[3] / "packages"


def layout(name):
    app = ET.parse(PLUGIN / f"{name}.xml").getroot().find("pluginApp")
    models = {int(m.get("id")) for m in app.find("models")}
    actions = {int(a.get("id")): a.get("target") for a in app.find("actions")}
    states = {int(s.get("id")): s for s in app.iter("hmiState")}
    shown = {int(c.get("model")) for c in app.iter() if c.get("model")}
    return app, models, actions, states, shown


def manifests():
    """(name, connector) for every manifest in plugin/ and in the published packages."""
    found = {}
    for path in PLUGIN.glob("*.manifest"):
        found[path.stem] = int(ET.parse(path).getroot().find("config/dependencies/pluginAppConnector/id").text)
    for package in sorted(PACKAGES.glob("*.zip")):
        with zipfile.ZipFile(package) as archive:
            for entry in archive.namelist():
                if entry.endswith(".manifest"):
                    root = ET.parse(BytesIO(archive.read(entry))).getroot()
                    name = root.find("name").text
                    found.setdefault(name, int(root.find("config/dependencies/pluginAppConnector/id").text))
    return found


class TextLayoutTest(unittest.TestCase):
    def test_connectors_and_weights_are_unique(self):
        connectors = manifests()
        self.assertEqual(connectors["WarmUp"], 105)
        self.assertEqual(connectors["PerfTimer"], 106)
        self.assertEqual(connectors["FaultScan"], 108)
        self.assertEqual(len(set(connectors.values())), len(connectors), connectors)
        weights = [layout(path.stem)[0].get("applicationWeight") for path in PLUGIN.glob("*.xml")]
        self.assertEqual(len(set(weights)), len(weights), weights)

    def test_worker_models_are_shown(self):
        # warm_up.c, perf_timer.c and fault_scan.c write these models.
        for name, models in (("WarmUp", [50, 51, 52, 53, 54, 55, 60]),
                             ("PerfTimer", list(range(50, 57))),
                             ("FaultScan", list(range(50, 59)) + [60, 61])):
            _, declared, _, _, shown = layout(name)
            for model in models:
                self.assertIn(model, declared, (name, model))
                self.assertIn(model, shown, (name, model))

    def test_plugin_actions_and_states(self):
        _, _, actions, states, _ = layout("PerfTimer")
        self.assertIn(41, actions)                       # Reset best results
        _, _, actions, states, _ = layout("FaultScan")
        self.assertTrue({41, 42, 43, 44, 45} <= set(actions))
        self.assertEqual(actions[40], "101")             # Clear fault codes page
        self.assertIn(101, states)

    def test_boostdeck_more_vitals_page(self):
        _, models, actions, states, shown = layout("BoostDeck")
        self.assertEqual(actions[32], "105")
        self.assertEqual(states[105].get("parentId"), "100")
        options = [b.get("action") for b in states[100].find("optionComponents")]
        self.assertIn("32", options)
        for model in range(120, 128):                    # vitals.c M_OIL..M_MISFIRE_B
            self.assertIn(model, models)
            self.assertIn(model, shown)

    def test_shiftdeck_gear_readout(self):
        app, models, _, _, shown = layout("ShiftDeck")
        images = {int(m.get("id")): int(m.get("imageId")) for m in app.find("models") if m.get("imageId")}
        self.assertEqual(images[75], 55930)               # GEAR legend, shift_assets.h kGearLegend
        self.assertIn(74, shown)
        self.assertIn(75, shown)
        boxes = {}
        for image in app.iter("image"):
            values = {p.get("id"): int(p.get("value")) for p in image.iter("property")
                      if p.get("id") in ("20", "21", "9", "10")}
            boxes[int(image.get("model"))] = (values["20"], values["21"], values["9"], values["10"])
        x, y, w, h = boxes[74]
        for model, (ox, oy, ow, oh) in boxes.items():
            if model in (74, 75) or oy + oh <= y or oy >= y + h:
                continue
            self.assertTrue(ox + ow <= x or ox >= x + w, f"gear digit overlaps model {model}")


if __name__ == "__main__":
    unittest.main()
