"""Run the Warm-Up, Performance Timer, Fault Scanner and Vitals workers against a
simulated HSFZ gateway and check the lines they send to the iDrive plugin."""
from pathlib import Path
import socket
import struct
import subprocess
import tempfile
import threading
import unittest

from test_transport import exact, frame

BUILD = Path(__file__).resolve().parents[1] / "build"


class Gateway:
    """Answers each request from a table keyed by target and request hex.

    A value is a list of (source, response hex) frames. Physical requests that
    are not in the table are refused with NRC 31; functional requests (target DF)
    that are not in the table get no reply."""

    def __init__(self, table):
        self.table = {key.lower(): value for key, value in table.items()}
        self.requests = []
        self.errors = []
        self.listener = socket.socket()
        self.listener.bind(("127.0.0.1", 0))
        self.listener.listen()
        self.listener.settimeout(0.1)
        self.stopping = False
        self.port = self.listener.getsockname()[1]
        self.thread = threading.Thread(target=self.serve)
        self.thread.start()

    def serve(self):
        try:
            while not self.stopping:
                try:
                    conn, _ = self.listener.accept()
                except TimeoutError:
                    continue
                except OSError:
                    return
                with conn:
                    conn.settimeout(5)
                    while True:
                        first = conn.recv(1)
                        if not first:
                            break
                        size, control = struct.unpack(">IH", first + exact(conn, 5))
                        body = exact(conn, size)
                        if control != 1 or body[0] != 0xF4:
                            raise AssertionError(f"unexpected frame {control} {body.hex()}")
                        target, request = body[1], body[2:].hex()
                        self.requests.append((target, request))
                        key = f"{target:02x}:{request}"
                        if key in self.table:
                            replies = self.table[key]
                            if replies and isinstance(replies[0], list):
                                # A list of lists answers repeated requests in turn.
                                replies = replies.pop(0) if len(replies) > 1 else replies[0]
                        elif target == 0xDF:
                            replies = []
                        else:
                            replies = [(target, "7f" + request[:2] + "31")]
                        for source, response in replies:
                            conn.sendall(frame(1, bytes([source, 0xF4]) + bytes.fromhex(response)))
        except Exception as error:  # reported by close()
            self.errors.append(error)

    def close(self):
        self.stopping = True
        self.thread.join(6)
        self.listener.close()
        self.thread.join(6)
        if self.errors:
            raise self.errors[0]


def run(worker, args, table, timeout=20):
    gateway = Gateway(table)
    try:
        result = subprocess.run(
            [BUILD / worker, "--host", "127.0.0.1", "--port", str(gateway.port), *args],
            capture_output=True, check=True, timeout=timeout)
    finally:
        gateway.close()
    return lines(result.stdout), gateway.requests


def lines(stdout):
    """The latest text per model, decoded as the HMI's Latin-1."""
    shown, values = {}, []
    shown["all"] = stdout.decode("latin-1").splitlines()
    for line in shown["all"]:
        kind, _, rest = line.partition(" ")
        if kind.startswith("T") or kind.startswith("I"):
            shown[kind] = rest
        elif kind.startswith("V"):
            values.append((kind, rest))
        else:
            raise AssertionError(f"line outside the plugin protocol: {line!r}")
    shown["values"] = values
    return shown


class WarmUpTest(unittest.TestCase):
    def test_oil_coolant_and_limit(self):
        shown, requests = run("warm-up", ["--cycles", "1"], {
            "12:015c": [(0x12, "415c7a")],          # 82 C oil
            "12:224300": [(0x12, "6243000e4b")],    # 92.75 C coolant
            "12:010c": [(0x12, "410c1f40")],        # 2000 rpm
        })
        self.assertEqual(shown["T50"], "Engine oil   180°F   (82°C)")
        self.assertEqual(shown["T51"], "Coolant   199°F   (93°C)")
        self.assertEqual(shown["T52"], "Nearly warm")
        self.assertEqual(shown["T53"], "Suggested limit   5500 rpm   (now 2000 rpm)")
        self.assertEqual(shown["I60"], str(56100 + 88 * 32 // 100))
        self.assertEqual([r for _, r in requests], ["015c", "224300", "010c"])

    def test_falls_back_to_the_did_then_to_coolant(self):
        shown, requests = run("warm-up", ["--cycles", "1"], {
            "12:224300": [(0x12, "6243000c3a")],    # 40 C coolant
        })
        self.assertIn((0x12, "22f45c"), requests)
        self.assertEqual(shown["T50"], "Engine oil   --")
        self.assertIn("estimated from coolant", shown["T55"])
        self.assertEqual(shown["T52"], "Cold: take it easy")

    def test_cold_engine_over_the_limit(self):
        shown, _ = run("warm-up", ["--cycles", "1"], {
            "12:015c": [(0x12, "415c3c")],          # 20 C oil
            "12:010c": [(0x12, "410c3a98")],        # 3750 rpm
        })
        self.assertEqual(shown["T52"], "Above the warm-up limit: ease off")
        self.assertEqual(shown["T53"], "Suggested limit   3000 rpm   (now 3750 rpm)")


class PerfTimerTest(unittest.TestCase):
    def test_ready_when_stopped_and_best_results_persist(self):
        with tempfile.TemporaryDirectory() as directory:
            state = Path(directory) / "perf-state.txt"
            state.write_text("best 0 4.7200 0 0 0\nbest 4 2.9000 0 36.000 0.9400\nbogus\n")
            shown, requests = run("perf-timer", ["--cycles", "12", "--state", str(state)], {
                "12:010d": [(0x12, "410d00")],
            })
            self.assertTrue(all(r == (0x12, "010d") for r in requests))
            self.assertGreaterEqual(len(requests), 10)
            self.assertEqual(shown["T51"], "0-60 mph      last --      best 4.7 s")
            self.assertEqual(shown["T55"], "60-0 mph braking      last --      best 118 ft (0.94 g)")
            self.assertTrue(shown["T50"].startswith("Ready"))
            # Steady data never flickers to "waiting".
            self.assertNotIn("T50 Waiting for speed data", shown["all"])
            shown, _ = run("perf-timer", ["--cycles", "1", "--state", str(state), "--reset"],
                           {"12:010d": [(0x12, "410d00")]})
            self.assertEqual(shown["T51"], "0-60 mph      last --      best --")
            self.assertFalse(state.exists())


SCAN = "df:19020c"
FAULTS = [(0x12, "5902ff2a82002f12345608"), (0x18, "5902ff"), (0x29, "7f1931")]


class FaultScanTest(unittest.TestCase):
    def setUp(self):
        self.directory = tempfile.TemporaryDirectory()
        self.file = str(Path(self.directory.name) / "last-scan.txt")

    def tearDown(self):
        self.directory.cleanup()

    def scan(self):
        return run("fault-scan", ["--file", self.file, "--scan"], {SCAN: FAULTS})

    def test_scan_lists_faults_by_unit_and_saves_them(self):
        shown, requests = self.scan()
        self.assertEqual(requests, [(0xDF, "19020c")])
        self.assertEqual(shown["T50"], "2 faults in 1 of 3 control units   Page 1/1")
        self.assertEqual(shown["T51"], "Engine (DME) 12:  2 faults")
        self.assertEqual(shown["T52"], "      123456   stored")
        self.assertEqual(shown["T53"], "      2A8200   active now")
        self.assertEqual(shown["T54"], "")
        # Pages come from the saved file without any vehicle traffic.
        result = subprocess.run([BUILD / "fault-scan", "--file", self.file, "--show", "3"],
                                capture_output=True, check=True, timeout=5)
        again = lines(result.stdout)
        self.assertEqual(again["T51"], shown["T51"])
        self.assertEqual(again["values"], [("V1", "0")])

    def test_clear_all_checks_speed_clears_and_rescans(self):
        self.scan()
        shown, requests = run("fault-scan", ["--file", self.file, "--clear", "all"], {
            "12:010d": [(0x12, "410d00")],
            "df:14ffffff": [(0x12, "54")],
            SCAN: [(0x12, "5902ff"), (0x18, "5902ff")],
        })
        self.assertEqual(requests, [(0x12, "010d"), (0xDF, "14ffffff"), (0xDF, "19020c")])
        self.assertEqual(shown["T61"], "Cleared 1 of 1. Faults that return at once are still present.")
        self.assertEqual(shown["T50"], "No stored faults in 2 control units")

    def test_never_clears_while_moving(self):
        self.scan()
        shown, requests = run("fault-scan", ["--file", self.file, "--clear", "all"], {
            "12:010d": [(0x12, "410d20")],
        })
        self.assertEqual(requests, [(0x12, "010d")])
        self.assertEqual(shown["T61"], "Could not confirm the car is stopped. Nothing was cleared.")

    def test_never_clears_without_a_speed_reading(self):
        shown, requests = run("fault-scan", ["--file", self.file, "--clear", "12"], {})
        self.assertEqual(requests, [(0x12, "010d")])
        self.assertIn("Nothing was cleared", shown["T61"])

    def test_engine_refusal_retries_in_extended_session(self):
        shown, requests = run("fault-scan", ["--file", self.file, "--clear", "12"], {
            "12:010d": [(0x12, "410d00")],
            "12:14ffffff": [(0x12, "7f1422")],
            "12:1003": [(0x12, "5003001900c8")],
            "12:1001": [(0x12, "5001001900c8")],
        })
        self.assertEqual([r for _, r in requests],
                         ["010d", "14ffffff", "1003", "14ffffff", "1001", "19020c"])
        self.assertEqual(shown["T61"], "Cleared 0 of 1. Not cleared: Engine (DME) 12: "
                                       "refused now: try with the engine off, ignition on")


class VitalsTest(unittest.TestCase):
    def test_vitals_page(self):
        shown, requests = run("boostdeck-collector", ["--vitals", "--cycles", "1"], {
            "12:015c": [(0x12, "415c82")],          # 90 C
            "12:22f40a": [(0x12, "62f40aa0")],  # 160 * 0.03 = 4.8 bar
            "12:0142": [(0x12, "414236b0")],        # 14.0 V
            "12:0106": [(0x12, "410682")],          # +1.6 %
            "12:0107": [(0x12, "41077e")],          # -1.6 %
            "12:0152": [(0x12, "41521a")],          # 10 %
            "12:0104": [(0x12, "410440")],          # 25 %
            "12:06a2": [(0x12, "46a20c240003000000ff")],
            "12:06a3": [(0x12, "46a30b240001000000ff")],  # wrong TID: unavailable
        })
        self.assertEqual(shown["T120"], "Engine oil   194°F   (90°C)")
        self.assertEqual(shown["T121"], "Low-pressure fuel   70 psi")
        self.assertEqual(shown["T122"], "Battery   14.0 V")
        self.assertEqual(shown["T123"], "Fuel trims   short +1.6%   long -1.6%")
        self.assertEqual(shown["T124"], "Ethanol content   10%")
        self.assertEqual(shown["T125"], "Engine load   25%")
        self.assertEqual(shown["T126"], "Misfires this drive   cyl 1: 3   cyl 2: --   cyl 3: --")
        self.assertIn((0x12, "06a7"), requests)


if __name__ == "__main__":
    unittest.main()
