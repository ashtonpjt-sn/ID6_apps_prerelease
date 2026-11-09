import unittest

from capture_displays import COMMANDS, LMC, MAX_SCREENS, screen_ids, survey


def mock_head(screens=b"screen 0 (0x0)\nscreen 2 (0x2)\n"):
    seen = []
    def read(command, optional=False):
        seen.append(command)
        if command == COMMANDS["screens"]:
            return screens
        if command == LMC + " get screen 0":
            return b"- resolution: x=1280, y=480\n"
        if command == LMC + " get screen 2":
            return b"- resolution: x=480, y=240\n"
        if command == COMMANDS["processes"]:
            return b"  1 procnto\n 42 /opt/hmi/bin/hud_manager\n 43 kombi_link\n 44 io-audio\n"
        return b""
    return read, seen


class SurveyTest(unittest.TestCase):
    def test_reads_listed_screens_and_flags_hud_and_cluster(self):
        read, seen = mock_head()
        summary, raw = survey(read)
        self.assertEqual(summary["screens"], [{"id": 0, "width": 1280, "height": 480},
                                              {"id": 2, "width": 480, "height": 240}])
        self.assertEqual(summary["keyword_hits"], ["42 /opt/hmi/bin/hud_manager", "43 kombi_link"])
        self.assertIn("network", summary["missing"])
        self.assertIn("screen_2", raw)

    def test_probes_a_range_when_no_screens_are_listed(self):
        self.assertEqual(screen_ids(b""), list(range(MAX_SCREENS)))
        read, seen = mock_head(screens=b"")
        summary, _ = survey(read)
        self.assertEqual([s["id"] for s in summary["screens"]], [0, 2])

    def test_only_reads(self):
        read, seen = mock_head()
        survey(read)
        for command in seen:
            first = command.split()[0]
            self.assertIn(first, (LMC, "pidin", "ifconfig", "ls", "find"))
            if first == LMC:
                self.assertEqual(command.split()[1], "get")
            self.assertNotRegex(command, r"-delete|-exec|(?<!2)>|\brm\b")


if __name__ == "__main__":
    unittest.main()
