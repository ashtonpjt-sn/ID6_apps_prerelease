"""Verify the two new workers on a simulated HSFZ gateway."""
from pathlib import Path
import os
import signal
import socket
import struct
import subprocess
import threading
import time
import unittest

from test_transport import exact, frame

BUILD = Path(__file__).resolve().parents[1] / "build"


class DriveAppsTest(unittest.TestCase):
    def run_worker(self, worker, args, transactions, env=None, stream="stdout"):
        errors = []
        with socket.socket() as listener:
            listener.bind(("127.0.0.1", 0))
            listener.listen()
            listener.settimeout(3)

            def gateway():
                try:
                    conn, _ = listener.accept()
                    with conn:
                        conn.settimeout(3)
                        for request, responses in transactions:
                            size, control = struct.unpack(">IH", exact(conn, 6))
                            self.assertEqual(control, 1)
                            self.assertEqual(exact(conn, size), bytes.fromhex("f412"+request))
                            for response in responses:
                                conn.sendall(frame(1, bytes.fromhex("12f4"+response)))
                        self.assertEqual(conn.recv(1), b"", "Unexpected repeat or extra command")
                except Exception as error:
                    errors.append(error)

            thread = threading.Thread(target=gateway)
            thread.start()
            result = subprocess.run(
                [BUILD/worker, "--host", "127.0.0.1", "--port", str(listener.getsockname()[1]), *args],
                capture_output=True, text=True, check=True, timeout=6, env=env)
            thread.join(4)
            self.assertFalse(thread.is_alive())
            if errors:
                raise errors[0]
            return getattr(result, stream)

    def test_map_two_has_only_one_request_and_requires_matching_echo(self):
        result = self.run_worker("mhd-maps", ["--map", "2"],
                                 [("2f0101", ["7f2f78", "6f0100", "6f0101"])])
        self.assertEqual(result, "Confirmed: Map 2\n")

    def test_wrong_echo_does_not_confirm_and_refusal_is_reported(self):
        result = self.run_worker("mhd-maps", ["--map", "1"], [("2f0100", ["6f0101"])])
        self.assertIn("Map result unknown", result)
        result = self.run_worker("mhd-maps", ["--map", "1"], [("2f0100", ["7f2f31"])])
        self.assertIn("refused by DME (NRC 31)", result)

    def test_maps_three_and_four_use_their_own_slot_bytes(self):
        for slot, index in ((1, "00"), (3, "02"), (4, "03")):
            with self.subTest(slot=slot):
                result = self.run_worker("mhd-maps", ["--map", str(slot)],
                                         [("2f01"+index, ["6f01"+index])])
                self.assertEqual(result, f"Confirmed: Map {slot}\n")

    def test_shift_worker_decodes_live_rpm_and_speed(self):
        with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as listener:
            listener.bind(("127.0.0.1", 0))
            listener.settimeout(2)
            self.run_worker("shift-deck", ["--cycles", "1", "--udp-port", str(listener.getsockname()[1])],
                            [("010c", ["410c61a8"]), ("010d", ["410d64"])])
            sampled_ms, rpm, kph, gear, reserved = struct.unpack("=Qiiii", listener.recv(64))
            self.assertGreater(sampled_ms, 0)
            self.assertEqual((rpm,kph,gear,reserved), (6250,100,0,0))


    def test_shift_worker_waits_for_a_busy_gateway(self):
        with socket.socket() as gateway, socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as lock:
            gateway.bind(("127.0.0.1", 0))
            gateway.listen()
            gateway.settimeout(0.2)
            lock.bind(("127.0.0.1", 42861))
            with subprocess.Popen(
                [BUILD/"shift-deck", "--host", "127.0.0.1",
                 "--port", str(gateway.getsockname()[1]), "--cycles", "6"],
                stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
            ) as worker:
                try:
                    with self.assertRaises(TimeoutError):
                        gateway.accept()
                    self.assertIsNone(worker.poll())
                    lock.close()
                    gateway.settimeout(2)
                    conn, _ = gateway.accept()
                    with conn:
                        conn.settimeout(2)
                        length, ctrl = struct.unpack(">IH", exact(conn, 6))
                        self.assertEqual(ctrl, 1)
                        self.assertEqual(exact(conn, length), bytes.fromhex("f412010c"))
                finally:
                    if worker.poll() is None:
                        worker.terminate()

    @staticmethod
    def closed_port():
        with socket.socket() as s:
            s.bind(("127.0.0.1", 0))
            return s.getsockname()[1]

    @staticmethod
    def most_env():
        return dict(os.environ, ID6_FAKE_MOST="up")

    def test_shiftdeck_most_fallback(self):
        with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as udp:
            udp.bind(("127.0.0.1", 0))
            udp.settimeout(5)
            r = subprocess.run(
                [BUILD/"shift-deck", "--host", "127.0.0.1", "--port", str(self.closed_port()),
                 "--udp-port", str(udp.getsockname()[1]), "--cycles", "40"],
                capture_output=True, text=True, timeout=20, env=self.most_env())
            _, rpm, kph, _, _ = struct.unpack("=Qiiii", udp.recv(64))
        self.assertEqual(r.returncode, 0, r.stderr)
        self.assertEqual((rpm, kph), ((0x1A*256+0xF8)//4, 60))
        self.assertIn("transport: MOST", r.stderr)

    def test_shiftdeck_hsfz_never_binds(self):
        with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as listener:
            listener.bind(("127.0.0.1", 0))
            listener.settimeout(2)
            err = self.run_worker("shift-deck", ["--cycles", "1", "--udp-port", str(listener.getsockname()[1])],
                                  [("010c", ["410c61a8"]), ("010d", ["410d64"])], env=self.most_env(), stream="stderr")
        self.assertNotIn("FAKE-MOST bind", err)

    def test_mhd_hsfz_never_binds(self):
        err = self.run_worker("mhd-maps", ["--map", "2"], [("2f0101", ["6f0101"])],
                              env=self.most_env(), stream="stderr")
        self.assertNotIn("FAKE-MOST bind", err)

    def start_most(self, worker, udp_port, extra=(), env=None):
        return subprocess.Popen([BUILD/worker, *extra, "--transport", "most", "--udp-port", str(udp_port)],
                                stdout=subprocess.DEVNULL, stderr=subprocess.PIPE, text=True,
                                env=env or self.most_env())

    @staticmethod
    def stop_all(procs):
        for p in procs:
            if p.poll() is None:
                p.send_signal(signal.SIGTERM)
        codes = []
        for p in procs:
            try:
                codes.append(p.wait(5))
            except subprocess.TimeoutExpired:
                p.kill()
                codes.append("hung")
        return codes, [p.communicate()[1] for p in procs]

    def shift_frames(self, udp, seconds):
        count, end = 0, time.monotonic()+seconds
        while time.monotonic() < end:
            try:
                udp.recv(64)
                count += 1
            except socket.timeout:
                pass
        return count

    def test_shiftdeck_keeps_a_fair_share_of_a_slow_tunnel(self):
        env = dict(self.most_env(), ID6_FAKE_MOST_DELAY_MS="80")
        with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as us, \
                socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as ub:
            for u in (us, ub):
                u.bind(("127.0.0.1", 0))
                u.settimeout(0.2)
            shift = self.start_most("shift-deck", us.getsockname()[1], env=env)
            procs = [shift]
            try:
                self.shift_frames(us, 1)
                alone = self.shift_frames(us, 5)
                procs.append(self.start_most("boostdeck-collector", ub.getsockname()[1], ["--session"], env=env))
                self.shift_frames(us, 1)
                shared = self.shift_frames(us, 5)
                self.assertIsNone(procs[1].poll())
            finally:
                codes, errs = self.stop_all(procs)
        self.assertEqual(codes, [0]*len(procs), errs)
        self.assertGreater(alone, 15)
        self.assertGreaterEqual(shared, 0.4*alone, (alone, shared))

    def test_shiftdeck_sigterm_on_most(self):
        with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as udp:
            udp.bind(("127.0.0.1", 0))
            udp.settimeout(3)
            proc = self.start_most("shift-deck", udp.getsockname()[1])
            try:
                udp.recv(64)
                start = time.monotonic()
                proc.send_signal(signal.SIGTERM)
                self.assertEqual(proc.wait(5), 0)
                self.assertLess(time.monotonic()-start, 3)
            finally:
                if proc.poll() is None:
                    proc.kill()
                err = proc.communicate()[1]
        self.assertIn("FAKE-MOST exit unregister_target=1 unregister_client=1 free=1", err)

    def test_shiftdeck_and_boostdeck_share_most(self):
        busy = []
        stop = threading.Event()

        def probe():
            while not stop.is_set():
                with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as s:
                    try:
                        s.bind(("127.0.0.1", 42861))
                    except OSError:
                        busy.append(1)
                time.sleep(0.001)

        with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as us, \
                socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as ub:
            for u in (us, ub):
                u.bind(("127.0.0.1", 0))
                u.settimeout(0.5)
            shift = self.start_most("shift-deck", us.getsockname()[1])
            boost = self.start_most("boostdeck-collector", ub.getsockname()[1], ["--session"])
            prober = threading.Thread(target=probe)
            prober.start()
            shift_ok = boost_ok = 0
            try:
                end = time.monotonic()+5
                while time.monotonic() < end:
                    for u, name in ((us, "s"), (ub, "b")):
                        try:
                            data = u.recv(512)
                        except socket.timeout:
                            continue
                        if name == "s":
                            shift_ok += struct.unpack("=Qiiii", data[:24])[1] == 1726
                        else:
                            boost_ok += int(data.decode().split()[3]) != 0
                    self.assertIsNone(shift.poll())
                    self.assertIsNone(boost.poll())
            finally:
                stop.set()
                prober.join()
                for p in (shift, boost):
                    if p.poll() is None:
                        p.send_signal(signal.SIGTERM)
                codes = []
                for p in (shift, boost):
                    try:
                        codes.append(p.wait(3))
                    except subprocess.TimeoutExpired:
                        p.kill()
                        codes.append("hung")
                errs = [p.communicate()[1] for p in (shift, boost)]
            self.assertEqual(codes, [0, 0], errs)
            self.assertGreater(shift_ok, 3)
            self.assertGreater(boost_ok, 3)
            self.assertTrue(busy, "the lease was never seen held")
            for err in errs:
                self.assertNotIn("deadlock", err.lower())


if __name__ == "__main__":
    unittest.main()
