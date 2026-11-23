import array
import fcntl
import json
import os
import socket
import sys
import tempfile
import unittest
from concurrent.futures import Future
from pathlib import Path
from types import SimpleNamespace
from unittest.mock import patch

try:
    import numpy as np
except ImportError:
    np = None


@unittest.skipUnless(np is not None, "Frame raw camera tests need NumPy")
class RawCameraTest(unittest.TestCase):
    def setUp(self):
        with patch.object(
            sys, "path", [str(Path(__file__).resolve().parents[1] / "tools"), *sys.path]
        ):
            import frame_camera_worker as worker
            import frame_raw_camera as raw
        self.worker = worker
        self.raw = raw
        self.sender, self.receiver = socket.socketpair(type=socket.SOCK_SEQPACKET)
        self.addCleanup(self.sender.close)
        self.addCleanup(self.receiver.close)
        self.cameras = {"left": SimpleNamespace(shape=(2, 3))}
        self.fd = os.memfd_create("synthetic-camera", os.MFD_CLOEXEC | os.MFD_ALLOW_SEALING)
        self.addCleanup(os.close, self.fd)
        os.write(self.fd, bytes([1, 2, 3, 0, 4, 5, 6, 0]))

    def send(self, seq=1, stride=4):
        header = {"seq": seq, "time": 1.0, "exposure": "ir", "views": [["left", stride]]}
        self.sender.sendmsg(
            [json.dumps(header).encode()],
            [(socket.SOL_SOCKET, socket.SCM_RIGHTS, array.array("i", [self.fd]))],
        )

    def test_borrow_then_release(self):
        fcntl.fcntl(self.fd, fcntl.F_ADD_SEALS, fcntl.F_SEAL_WRITE | fcntl.F_SEAL_SHRINK)
        self.send()

        def consume(kind, t, images):
            self.assertEqual((kind, t), (38, 1.0))
            self.assertFalse(images["left"].flags.writeable)
            np.testing.assert_array_equal(images["left"], [[1, 2, 3], [4, 5, 6]])
            return ["seen"]

        self.assertEqual(self.raw.frame(self.receiver, self.cameras, 0, consume), (1, ["seen"]))
        self.assertEqual(json.loads(self.sender.recv(100)), {"release": 1})
        self.send()
        with self.assertRaisesRegex(ValueError, "Invalid raw camera group"):
            self.raw.frame(self.receiver, self.cameras, 1, consume)

    def test_require_stable_memory_and_valid_shape(self):
        self.send()
        with self.assertRaisesRegex(ValueError, "Unsealed"):
            self.raw.frame(self.receiver, self.cameras, 0, None)
        self.send(stride=5)
        with self.assertRaisesRegex(ValueError, "too short"):
            self.raw.frame(self.receiver, self.cameras, 0, None)

    def test_wrong_headset(self):
        self.sender.send(
            json.dumps({"version": 1, "device": {"device_serial_number": "different"}}).encode()
        )
        with self.assertRaisesRegex(ValueError, "does not match"):
            self.raw.calibration(self.receiver, "HEADSET")

    def test_full_backlog_then_delayed_camera_calibration(self):
        with (
            tempfile.TemporaryDirectory() as directory,
            socket.socket(socket.AF_UNIX, socket.SOCK_SEQPACKET) as listener,
        ):
            path = directory + "/camera.sock"
            listener.bind(path)
            listener.listen(0)
            listener.settimeout(1)
            with socket.socket(socket.AF_UNIX, socket.SOCK_SEQPACKET) as abandoned:
                abandoned.connect(path)
            polls = 0

            def poll(track):
                nonlocal polls
                polls += 1
                if polls == 2:
                    listener.accept()[0].close()  # Make room for the worker.
                if polls == 4:
                    listener.accept()[0].close()  # Provider restarted before its handshake.
                if polls == 6:
                    with listener.accept()[0] as peer:
                        peer.sendall(b'{"version":1}')
                return True

            with (
                patch.object(self.raw, "address", return_value=path),
                patch.object(
                    self.raw, "parse_calibration", return_value=("cams", "config")
                ) as parse,
                self.worker.camera_feed("HEADSET", SimpleNamespace(poll=poll)) as feed,
            ):
                self.assertEqual(feed[1:], ("cams", "config"))
            parse.assert_called_once_with({"version": 1}, "HEADSET")

    def test_camera_wait_honors_stop_and_reports_a_persistently_missing_provider(self):
        worker = self.worker
        parent = worker.Parent()
        with patch.object(worker.select, "select", return_value=([], [], [])):
            parent.last -= 30
            self.assertTrue(parent.poll(None))  # The app sends heartbeats only after ready.
            with self.assertRaisesRegex(TimeoutError, "heartbeat"):
                parent.poll(object())
            parent.stopped = True
            with worker.camera_feed("HEADSET", parent) as feed:
                self.assertIsNone(feed)
            parent.stopped = False
            with (
                patch.object(worker.time, "monotonic", side_effect=[0, 21]),
                self.assertRaisesRegex(TimeoutError, "restart SteamVR"),
            ):
                with worker.camera_feed("HEADSET", parent):
                    self.fail("A missing provider opened a feed")

    def test_read_only_shared_backing_with_offset(self):
        offset = os.sysconf("SC_PAGE_SIZE")
        os.pwrite(self.fd, bytes([7, 8, 9, 0, 1, 2, 3, 0]), offset)
        fcntl.fcntl(self.fd, fcntl.F_ADD_SEALS, fcntl.F_SEAL_SHRINK)
        fd = os.open(f"/proc/self/fd/{self.fd}", os.O_RDONLY)
        self.addCleanup(os.close, fd)
        header = {
            "seq": 1,
            "time": 1.0,
            "exposure": "ir",
            "storage": "udmabuf",
            "views": [["left", 4, offset]],
        }

        def send(descriptor):
            self.sender.sendmsg(
                [json.dumps(header).encode()],
                [(socket.SOL_SOCKET, socket.SCM_RIGHTS, array.array("i", [descriptor]))],
            )

        def consume(kind, t, images):
            self.assertFalse(images["left"].flags.writeable)
            np.testing.assert_array_equal(images["left"], [[7, 8, 9], [1, 2, 3]])

        send(fd)
        self.raw.frame(self.receiver, self.cameras, 0, consume)
        self.assertEqual(json.loads(self.sender.recv(100)), {"release": 1})
        send(self.fd)
        with self.assertRaisesRegex(ValueError, "read-only"):
            self.raw.frame(self.receiver, self.cameras, 0, consume)

    def test_illumination_handoff_does_not_update_dark_baseline(self):
        worker = self.worker
        with patch.object(worker, "PalmBootstrap", return_value=SimpleNamespace(head=None)):
            tracker = worker.CameraTracking(
                dict.fromkeys(("left", "right", "top", "bottom")), {}, None
            )

        tracker.lit_requested = True
        image = {"left": np.full((2, 3), 255, dtype=np.uint8)}
        with patch.object(tracker.led, "update", return_value=None) as update:
            self.assertEqual(tracker.exposure(38, 1.0, image, lambda _: True), [])
            tracker.enabled_at = 1.1
            self.assertEqual(tracker.exposure(38, 1.05, image, lambda _: True), [])
            update.assert_not_called()
            tracker.exposure(38, 1.2, image, lambda _: True)
            self.assertTrue(update.call_args.args[2])

    def test_restored_alignment_streams_led_without_using_a_pending_palm(self):
        worker = self.worker
        head = np.column_stack([np.eye(3), np.zeros(3)])
        with patch.object(worker, "PalmBootstrap", return_value=SimpleNamespace(head=head)):
            tracker = worker.CameraTracking(
                dict.fromkeys(("left", "right", "top", "bottom")), {}, None
            )

        tracker.lit_requested = tracker.check_resume = True
        tracker.enabled_at = 1.0
        fix = (np.array([0, 0, -0.5]), ("left", "right"))
        tracker.priors.append((1.1, fix[0]))
        with patch.object(tracker.led, "update", return_value=fix) as update:
            self.assertEqual(
                [event["op"] for event in tracker.exposure(38, 1.1, {}, bool)], ["led"]
            )
            np.testing.assert_array_equal(update.call_args.args[3], fix[0])
            self.assertTrue(update.call_args.kwargs["resume"])
            tracker.pending = Future()
            tracker.pending.set_result(SimpleNamespace(time=1.1))
            tracker.accept_alignment()
            self.assertFalse(tracker.needs_seed)
            self.assertEqual(
                [event["op"] for event in tracker.exposure(38, 1.2, {}, bool)], ["led"]
            )
            tracker.reacquire()
            self.assertTrue(tracker.needs_seed)

    def test_calibration_requires_four_models(self):
        with self.assertRaises(KeyError):
            self.raw.parse_calibration(
                {
                    "version": 1,
                    "device": {"device_serial_number": "HEADSET"},
                    "rig": {"cameras": []},
                    "metadata": {"cameras": {}},
                },
                "HEADSET",
            )
