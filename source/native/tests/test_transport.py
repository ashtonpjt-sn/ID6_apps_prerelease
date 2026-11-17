"""Exercise the real C collector against a loopback HSFZ gateway; no car needed."""
import os
import re
import signal
import socket
import struct
import subprocess
import threading
import time
import unittest
from pathlib import Path

BIN = str(Path(__file__).resolve().parents[1] / "build/boostdeck-collector")
VALUES = {0x4201: bytes.fromhex("63ca"), 0x4205: bytes.fromhex("31e5"),
          0x5889: bytes.fromhex("104e"), 0x4A49: bytes.fromhex("004e"),
          0x580F: bytes.fromhex("82"), 0xDA12: bytes.fromhex("008e")}
VALUES.update({0x56D7: bytes.fromhex("1169"), 0x586F: bytes.fromhex("09c1"), 0x4300: bytes.fromhex("0bf8")})
CAR_VALUES = {0x4201: bytes.fromhex("6589"), 0x4205: bytes.fromhex("32dd"),
              0x5889: bytes.fromhex("1015"), 0x4A49: bytes.fromhex("ffff"),
              0x580F: bytes.fromhex("8c"), 0xDA12: bytes.fromhex("005a")}
CAR_VALUES.update({did: VALUES[did] for did in (0x56D7, 0x586F, 0x4300)})


def frame(ctrl, body):
    return struct.pack(">IH", len(body), ctrl) + body


def exact(conn, n):
    out = b""
    while len(out) < n:
        data = conn.recv(n - len(out))
        if not data:
            raise EOFError
        out += data
    return out


class TransportTest(unittest.TestCase):
    def run_collector(self, mode="normal", extra=(), cycles=1, env=None):
        udp = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        udp.bind(("127.0.0.1", 0))
        udp.settimeout(4)
        server = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        server.bind(("127.0.0.1", 0))
        server.listen(1)
        server.settimeout(4)
        requests, errors = [], []

        def gateway():
            try:
                conn, _ = server.accept()
                with conn:
                    conn.settimeout(3)
                    # The fragments below go out at once; with Nagle on, Linux delayed ACKs add ~20 ms each.
                    conn.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
                    while True:
                        first = conn.recv(1)
                        if not first:
                            break
                        length, ctrl = struct.unpack(">IH", first + exact(conn, 5))
                        self.assertEqual(ctrl, 1)
                        req = exact(conn, length)
                        requests.append(req)
                        self.assertEqual(req[0], 0xF4)
                        self.assertEqual(length, 5)
                        self.assertEqual(req[2], 0x22)  # read only
                        ecu, did = req[1], int.from_bytes(req[3:], "big")
                        self.assertEqual(ecu, 0x18 if did == 0xDA12 else 0x12)
                        if mode == "timeout":
                            time.sleep(.8)
                            return
                        if mode == "oversized":
                            conn.sendall(struct.pack(">IH", 500000, 1))
                            return
                        if mode == "heartbeat":
                            conn.sendall(frame(0x12, bytes([0, 0xF4])))
                            # Read the full reply, including its declared body.
                            alive_header = exact(conn, 6)
                            alive_length, alive_ctrl = struct.unpack(">IH", alive_header)
                            self.assertEqual(alive_ctrl, 0x13)
                            self.assertEqual(alive_length, 2)
                            self.assertEqual(exact(conn, alive_length), bytes([0, 0xF4]))
                        conn.sendall(frame(2, req))  # ack
                        # Wrong source with a valid-looking payload must be ignored.
                        conn.sendall(frame(1, bytes([0x99, 0xF4, 0x62]) + req[3:] + b"\0\0"))
                        conn.sendall(frame(1, bytes([ecu, 0xF4, 0x7F, 0x22, 0x78])))
                        if mode == "no_trans" and did == 0xDA12:
                            result = bytes([ecu, 0xF4, 0x7F, 0x22, 0x31])
                        else:
                            values = CAR_VALUES if mode == "car_snapshot" else VALUES
                            payload = values[did]
                            if mode == "extra_pressure" and did == 0x4205:
                                payload += b"\xaa"
                            if mode == "extra_iat" and did == 0x580F:
                                payload += b"\xaa"
                            result = bytes([ecu, 0xF4, 0x62]) + req[3:] + payload
                        if mode == "paced":
                            time.sleep(.030)
                        raw = frame(1, result)
                        # Fragment both header and body to catch partial-read bugs.
                        for part in (raw[:2], raw[2:7], raw[7:]):
                            conn.sendall(part)
            except Exception as exc:
                errors.append(exc)

        t = threading.Thread(target=gateway, daemon=True)
        t.start()
        try:
            result = subprocess.run([BIN, "--host", "127.0.0.1", "--port", str(server.getsockname()[1]),
                                     "--udp-port", str(udp.getsockname()[1]), "--cycles", str(cycles), *extra],
                                    capture_output=True, text=True, timeout=10, env=env)
            self.assertEqual(result.returncode, 0, result.stderr)
            packets = [udp.recv(512).decode().split() for _ in range(cycles)]
            packet = packets[-1]
            self.assertEqual(packet[0], "NBG3")
            self.assertGreater(int(packet[4]), 0)
            t.join(2)
            self.assertFalse(t.is_alive())
            if errors:
                raise errors[0]
            return packet, requests, result.stderr
        finally:
            udp.close()
            server.close()

    def test_recorded_values_and_fragmented_transport(self):
        p, requests, _ = self.run_collector()
        self.assertEqual(len(requests), 9)
        self.assertEqual(int(p[2]), 0)  # not demo
        self.assertEqual(int(p[3]), 255)
        values = [float(p[i]) for i in (5, 7, 9, 11, 13)]
        self.assertAlmostEqual(values[0], 0)
        self.assertAlmostEqual(values[1], 4174*16/65535*14.7, places=5)
        self.assertAlmostEqual(values[2], 7.8)
        self.assertAlmostEqual(values[3], 49.5)
        self.assertAlmostEqual(values[4], 94)
        self.assertAlmostEqual(float(p[15]), 89.14)
        self.assertAlmostEqual(float(p[17]), 2.497)
        self.assertAlmostEqual(float(p[19]), 33.25)

    def test_fast_channels_and_staggered_slow_updates(self):
        packet, requests, trace = self.run_collector("paced", ("--trace",), cycles=32)
        self.assertEqual(int(packet[3]), 255)
        self.assertEqual(len(packet), 21)  # eight pairs of values and ages
        self.assertNotIn(bytes.fromhex("f4120152"), requests)
        for did in ("4205", "5889", "4A49", "56D7", "586F"):
            samples = re.findall(rf"RAW mono_ms=(\d+) ecu=12 did={did}", trace)
            self.assertEqual(len(samples), 32)
            self.assertLess((int(samples[-1])-int(samples[1]))/30, 215)
        for did in ("580F", "DA12", "4300"):
            samples = re.findall(rf"RAW mono_ms=(\d+) ecu=\w+ did={did}", trace)
            gaps = [int(b)-int(a) for a,b in zip(samples[1:], samples[2:])]
            self.assertTrue(gaps)
            self.assertTrue(all(1700 < gap < 2300 for gap in gaps), (did,gaps))

    def test_rejected_transmission_is_missing(self):
        p, _, _ = self.run_collector("no_trans")
        self.assertEqual(int(p[3]), 239)

    def test_heartbeat_reply_uses_full_ediabaslib_shape(self):
        p, requests, _ = self.run_collector("heartbeat")
        self.assertEqual(len(requests), 9)
        self.assertEqual(int(p[3]), 255)

    def test_extra_did_bytes_do_not_become_valid_readings(self):
        for mode, expected_mask in [("extra_pressure", 254), ("extra_iat", 247)]:
            with self.subTest(mode=mode):
                p, requests, _ = self.run_collector(mode)
                self.assertEqual(len(requests), 9)
                self.assertEqual(int(p[3]), expected_mask)

    def test_timeout_is_bounded(self):
        start = time.monotonic()
        p, _, _ = self.run_collector("timeout")
        self.assertEqual(int(p[3]), 0)
        self.assertLess(time.monotonic()-start, 2)

    def test_oversized_frame_is_rejected(self):
        p, _, _ = self.run_collector("oversized")
        self.assertEqual(int(p[3]), 0)

    def test_this_cars_snapshot_and_ambiguous_timing_trace(self):
        p, requests, trace = self.run_collector("car_snapshot", ("--trace",))
        self.assertEqual(len(requests), 9)
        self.assertEqual(int(p[3]), 251)  # timing alone remains unavailable
        self.assertAlmostEqual(float(p[5]), .027761, places=5)
        self.assertAlmostEqual(float(p[7]), 14.775593, places=5)
        self.assertEqual(float(p[11]), 57)
        self.assertEqual(float(p[13]), 42)
        self.assertIn("did=4A49 payload=FFFF unavailable reason=ambiguous-FFFF", trace)
        self.assertEqual(trace.count("RAW mono_ms="), 9)

    def test_no_trans_flag_never_queries_egs(self):
        p, requests, _ = self.run_collector(extra=("--no-trans",))
        self.assertEqual(int(p[3]), 239)
        self.assertEqual(len(requests), 8)
        self.assertTrue(all(r[1] == 0x12 for r in requests))

    def run_most(self, args, fake="up", cycles=12):
        """Run with the in-process fake DSI; return every packet, the exit status and stderr."""
        env = dict(os.environ)
        env.pop("ID6_FAKE_MOST", None)
        if fake:
            env["ID6_FAKE_MOST"] = fake
        with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as udp:
            udp.bind(("127.0.0.1", 0))
            udp.settimeout(.5)
            r = subprocess.run([BIN, *args, "--udp-port", str(udp.getsockname()[1]), "--cycles", str(cycles)],
                               capture_output=True, text=True, timeout=20, env=env)
            packets = []
            try:
                while True:
                    packets.append(udp.recv(512).decode().split())
            except socket.timeout:
                pass
        return packets, r.returncode, r.stderr

    @staticmethod
    def closed_port():
        with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as s:
            s.bind(("127.0.0.1", 0))
            return s.getsockname()[1]

    def assert_most_values(self, p):
        self.assertEqual(int(p[3]), 239)  # every gauge but the EGS transmission temperature
        self.assertAlmostEqual(float(p[7]), 4174*16/65535*14.7, places=5)
        self.assertAlmostEqual(float(p[9]), 7.8)
        self.assertAlmostEqual(float(p[11]), 49.5)
        self.assertAlmostEqual(float(p[15]), 89.14)
        self.assertAlmostEqual(float(p[19]), 33.25)

    def test_unreachable_gateway_switches_to_most(self):
        packets, rc, err = self.run_most(["--host", "127.0.0.1", "--port", str(self.closed_port())])
        self.assertEqual(rc, 0, err)
        self.assertEqual([int(p[3]) for p in packets[:4]], [0]*4)  # the fifth failed connect opens MOST
        self.assertIn("transport: MOST", err)
        self.assert_most_values(packets[-1])
        self.assertIn("FAKE-MOST exit unregister_target=1 unregister_client=1 free=1", err)

    def test_failed_most_open_is_closed_and_hsfz_retried(self):
        start = time.monotonic()
        packets, rc, err = self.run_most(["--host", "127.0.0.1", "--port", str(self.closed_port())],
                                         fake="no-server", cycles=12)
        self.assertEqual(rc, 0, err)
        self.assertLess(time.monotonic()-start, 8)
        self.assertIn("MOST open failed: no server", err)
        self.assertEqual(err.count("MOST open failed"), 1)  # backed off, not retried every tick
        self.assertIn("free=1", err)  # the failed open's DSI client was freed
        self.assertTrue(all(int(p[3]) == 0 for p in packets))

    def test_missing_gateway_file_uses_most(self):
        packets, rc, err = self.run_most(["--trace"], cycles=3)
        self.assertEqual(rc, 0, err)
        self.assert_most_values(packets[-1])
        self.assertNotIn("did=DA12", err)
        self.assertIn("ecu=12 did=4205 payload=31E5", err)

    def test_boostdeck_skips_egs_on_most(self):
        packets, rc, err = self.run_most(["--transport", "most", "--trace"], cycles=4)
        self.assertEqual(rc, 0, err)
        self.assertNotIn("did=DA12", err)
        self.assertNotIn("ecu=18", err)
        self.assertTrue(all(int(p[3]) & (1 << 4) == 0 for p in packets))  # transmission temperature invalid
        self.assertEqual(int(packets[-1][3]), 239)

    def test_working_gateway_never_touches_most(self):
        env = dict(os.environ, ID6_FAKE_MOST="up")
        p, requests, err = self.run_collector(cycles=3, env=env)
        self.assertEqual(int(p[3]), 255)
        self.assertNotIn("FAKE-MOST", err)
        _, rc, err = self.run_most(["--transport", "hsfz", "--host", "127.0.0.1",
                                    "--port", str(self.closed_port())], cycles=10)
        self.assertEqual(rc, 0, err)
        self.assertNotIn("FAKE-MOST", err)

    def test_missing_dsi_library_keeps_emitting_no_data(self):
        packets, rc, err = self.run_most(["--transport", "most"], fake=None, cycles=4)
        self.assertEqual(rc, 0, err)
        self.assertEqual(len(packets), 4)
        self.assertTrue(all(int(p[3]) == 0 for p in packets))
        self.assertIn("MOST open failed: no library", err)
        self.assertEqual(err.count("FAKE-MOST bind"), 1)  # not retried every tick

    def test_lost_most_link_exits_for_a_fresh_worker(self):
        packets, rc, err = self.run_most(["--transport", "most"], fake="drop", cycles=50)
        self.assertEqual(rc, 1, err)
        self.assertLess(len(packets), 50)

    def test_busy_tunnel_keeps_fresh_values(self):
        """A read skipped for a busy lease leaves a still-fresh gauge shown."""
        env = dict(os.environ, ID6_FAKE_MOST="up")
        env.pop("ID6_FAKE_MOST_DELAY_MS", None)
        with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as udp:
            udp.bind(("127.0.0.1", 0))
            udp.settimeout(1)
            proc = subprocess.Popen([BIN, "--transport", "most", "--udp-port", str(udp.getsockname()[1]),
                                     "--cycles", "30"], stderr=subprocess.DEVNULL, env=env)
            masks, held = [], False
            try:
                while proc.poll() is None or len(masks) < 30:
                    try:
                        mask = int(udp.recv(512).decode().split()[3])
                    except socket.timeout:
                        break
                    masks.append(mask)
                    if mask == 239 and not held:
                        held = True
                        lock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
                        lock.bind(("127.0.0.1", 42861))
                        threading.Timer(1.0, lock.close).start()
            finally:
                if proc.poll() is None:
                    proc.kill()
                proc.wait()
        self.assertTrue(held, masks)
        shown = masks[masks.index(239):]
        # Boost needs a fresh ambient read too, so only the other MOST gauges are checked.
        self.assertTrue(all(m & 238 == 238 for m in shown), shown)

    def test_hsfz_needs_host(self):
        r = subprocess.run([BIN, "--transport", "hsfz"], capture_output=True)
        self.assertEqual(r.returncode, 2)

    def test_sigterm_closes_most_session(self):
        env = dict(os.environ, ID6_FAKE_MOST="up")
        with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as udp:
            udp.bind(("127.0.0.1", 0))
            udp.settimeout(3)
            proc = subprocess.Popen([BIN, "--session", "--udp-port", str(udp.getsockname()[1])],
                                    stderr=subprocess.PIPE, text=True, env=env)
            try:
                deadline = time.monotonic() + 3
                while time.monotonic() < deadline:
                    if int(udp.recv(512).decode().split()[3]) == 239:
                        break
                else:
                    self.fail("no MOST values")
                start = time.monotonic()
                proc.send_signal(signal.SIGTERM)
                self.assertEqual(proc.wait(5), 0)
                self.assertLess(time.monotonic()-start, 3)
            finally:
                if proc.poll() is None:
                    proc.kill()
                _, err = proc.communicate()
        self.assertIn("FAKE-MOST exit unregister_target=1 unregister_client=1 free=1", err)

    def test_demo_never_uses_vehicle(self):
        with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as udp:
            udp.bind(("127.0.0.1", 0))
            udp.settimeout(2)
            r = subprocess.run([BIN, "--demo", "--cycles", "1", "--udp-port", str(udp.getsockname()[1])],
                               capture_output=True, timeout=2)
            self.assertEqual(r.returncode, 0)
            self.assertEqual(udp.recv(512).decode().split()[2:4], ["1", "255"])
        r = subprocess.run([BIN, "--demo", "--host", "127.0.0.1"], capture_output=True)
        self.assertEqual(r.returncode, 2)


if __name__ == "__main__":
    unittest.main()
