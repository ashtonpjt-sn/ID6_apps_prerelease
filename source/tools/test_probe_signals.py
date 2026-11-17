import argparse
import json
from pathlib import Path
import socket
import struct
import tempfile
import threading
import unittest

from probe_signals import CATALOG_LIMIT, decode, exchange, load_catalog, read_request

CATALOG = Path(__file__).resolve().parent / "b58_catalog.json"


def frame(control, data):
    return struct.pack(">IH", len(data), control) + data


class ProbeTest(unittest.TestCase):
    def test_reply_filters_and_heartbeat(self):
        client, gateway = socket.socketpair()
        with client, gateway:
            gateway.settimeout(2)
            def respond():
                self.assertEqual(gateway.recv(11), frame(1, bytes.fromhex("f412224300")))
                gateway.sendall(frame(0x12, b""))
                self.assertEqual(gateway.recv(8), frame(0x13, bytes.fromhex("00f4")))
                for value in ("18f46243000000", "12f47f2278", "12f46243000bf8"):
                    gateway.sendall(frame(1, bytes.fromhex(value)))
            thread = threading.Thread(target=respond)
            thread.start()
            result = exchange(client, 0x12, read_request("224300"))
            thread.join(3)
            self.assertFalse(thread.is_alive())
            self.assertEqual(result, {"reply": "62 43 00 0b f8", "data": "0b f8"})

    def test_refuses_actuation_and_decodes_negative_reply(self):
        for text in ("2f0100", "2e430001", "14ffffff", "2243", "01050c"):
            with self.assertRaises(argparse.ArgumentTypeError):
                read_request(text)
        client, gateway = socket.socketpair()
        with client, gateway:
            gateway.sendall(frame(1, bytes.fromhex("12f47f2231")))
            self.assertEqual(exchange(client, 0x12, read_request("22586f"))["nrc"], "31")


class CatalogTest(unittest.TestCase):
    def write(self, requests):
        handle = tempfile.NamedTemporaryFile("w", suffix=".json", delete=False, encoding="utf-8")
        with handle:
            json.dump({"requests": requests}, handle)
        self.addCleanup(Path(handle.name).unlink)
        return handle.name

    def test_shipped_catalog_is_read_only_and_bounded(self):
        plan, _ = load_catalog(CATALOG)
        self.assertGreater(len(plan), 100)
        self.assertLessEqual(len(plan), CATALOG_LIMIT)
        for ecu, request, _ in plan:
            self.assertIn(ecu, ("12", "18"))
            self.assertIn(request[0], (0x01, 0x22))

    def test_only_keeps_the_named_reads(self):
        plan, _ = load_catalog(CATALOG, ["224A49", "22f470"])
        self.assertEqual(sorted(r.hex() for _, r, _ in plan), ["224a49", "22f470"])

    def test_writes_and_other_units_are_refused(self):
        for entry in ({"ecu": "12", "request": "2E430001"}, {"ecu": "12", "request": "2F60FD0303E8"},
                      {"ecu": "29", "request": "22DBE1"}, {"ecu": "12", "request": "1902"}):
            with self.subTest(entry=entry), self.assertRaises(argparse.ArgumentTypeError):
                load_catalog(self.write([entry]))

    def test_decode_scales_signed_values_and_flags_short_replies(self):
        timing = [{"name": "t", "type": "s16", "at": 0, "factor": 0.1, "unit": "deg", "label": "Timing"}]
        # The handoff's live reply for 22 4A49 was ff e9: ignition angle -2.3 degrees.
        self.assertEqual(decode(timing, bytes.fromhex("ffe9")), {"Timing": "-2.3 deg"})
        self.assertEqual(decode(timing, bytes.fromhex("ff")), {"Timing": "short reply"})
        boost = [{"name": "a", "type": "u16", "at": 3, "factor": 0.03125, "label": "Actual", "min": 20, "max": 600},
                 {"name": "t", "type": "u16", "at": 1, "factor": 0.03125, "label": "Target", "min": 20, "max": 600}]
        values = decode(boost, bytes.fromhex("1f0c800ca0"))
        self.assertEqual(values, {"Actual": 101.0, "Target": 100.0})
        oil = [{"name": "o", "type": "u8", "at": 0, "offset": -40, "label": "Oil", "min": -40, "max": 170}]
        self.assertEqual(decode(oil, bytes.fromhex("ff")), {"Oil": "215 (outside -40..170)"})


if __name__ == "__main__":
    unittest.main()
