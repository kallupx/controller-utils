import array
import fcntl
import json
import os
import socket
import sys
import tempfile
import unittest
from concurrent.futures import Future
from contextlib import nullcontext
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

    def send(self, seq=1, stride=4, *, peer=None, exposure="ir", timestamp=1.0):
        header = {"seq": seq, "time": timestamp, "exposure": exposure, "views": [["left", stride]]}
        (peer or self.sender).sendmsg(
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
            fcntl.fcntl(self.fd, fcntl.F_ADD_SEALS, fcntl.F_SEAL_WRITE | fcntl.F_SEAL_SHRINK)
            polls = 0
            live = None

            def poll(track):
                nonlocal polls, live
                polls += 1
                self.assertLess(polls, 16)
                if polls == 2:
                    listener.accept()[0].close()  # Make room for the worker.
                if polls == 4:
                    listener.accept()[0].close()  # Provider restarted before its handshake.
                if polls == 6:
                    with listener.accept()[0] as peer:
                        peer.sendall(b'{"version":1}')
                if polls == 9:
                    live = listener.accept()[0]
                    self.addCleanup(live.close)
                    live.settimeout(1)
                    live.sendall(b'{"version":1}')
                    self.send(1, peer=live, exposure="scene")
                    self.send(2, peer=live, timestamp=0.5)
                    self.send(3, peer=live)
                return True

            with (
                patch.object(self.raw, "address", return_value=path),
                patch.object(self.worker, "raw_time", return_value=1),
                patch.object(
                    self.raw, "parse_calibration", return_value=(self.cameras, "config")
                ) as parse,
                self.worker.camera_feed("HEADSET", SimpleNamespace(poll=poll)) as feed,
            ):
                self.assertEqual(feed[1:], (self.cameras, "config", 3))
                self.assertEqual(
                    [json.loads(live.recv(64))["release"] for _ in range(3)], [1, 2, 3]
                )
            self.assertEqual(parse.call_count, 2)
            parse.assert_called_with({"version": 1}, "HEADSET")

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
            with (
                tempfile.TemporaryDirectory() as directory,
                patch.object(self.raw, "address", return_value=directory + "/missing.sock"),
                patch("frame_camera_runtime.Path", return_value=Path(directory)),
                patch.object(parent, "poll", side_effect=[True, True, False]),
                patch.object(worker.time, "monotonic", return_value=0) as clock,
                patch.object(
                    worker.time, "sleep", side_effect=lambda _: setattr(clock, "return_value", 21)
                ),
                patch.object(worker, "emit") as emit,
                worker.camera_feed("HEADSET", parent) as feed,
            ):
                self.assertIsNone(feed)
            emit.assert_called_once()
            self.assertEqual(emit.call_args.args, ("waiting",))
            self.assertIn("Restart Frame", emit.call_args.kwargs["message"])

    def test_busy_palm_process_retries_before_opening_the_camera(self):
        worker = self.worker
        with (
            patch.object(worker.signal, "signal"),
            patch.object(worker.Parent, "poll", return_value=True),
            patch.object(
                worker,
                "FramePalmModel",
                side_effect=[BlockingIOError(), BlockingIOError(), nullcontext()],
            ),
            patch.object(worker.time, "monotonic", return_value=0) as clock,
            patch.object(
                worker.time, "sleep", side_effect=lambda _: setattr(clock, "return_value", 11)
            ),
            patch.object(
                worker.frame_debug,
                "capture",
                side_effect=lambda *_: ("123", clock.return_value > 0, 0),
            ),
            patch.object(worker.frame_debug, "send") as logged,
            patch.object(worker, "emit") as emit,
            patch.object(worker, "camera_feed", return_value=nullcontext(None)) as feed,
        ):
            worker.main("HEADSET")
        feed.assert_called_once()
        emit.assert_called_once()
        self.assertIn("palm tracking", emit.call_args.kwargs["message"])
        self.assertEqual(
            sum("Palm helper launch failed" in call.args[1] for call in logged.call_args_list), 1
        )

    def test_request_ready_rearms_dark_baseline(self):
        worker = self.worker
        parent = worker.Parent()
        parent.dark_since = 1
        track = SimpleNamespace(
            led=worker.LedTracker(dict.fromkeys(("a", "b", "c", "d"))),
            lit_requested=True,
            first_ir=1,
            enabled_at=None,
        )
        track.led.baselines["a"] = 5
        with (
            patch.object(worker.select, "select", side_effect=[([0], [], []), ([], [], [])]),
            patch.object(os, "read", return_value=b'{"op":"request_ready"}\n'),
            patch.object(worker, "emit") as emit,
        ):
            self.assertTrue(parent.poll(track))
        emit.assert_called_once_with("ready")
        self.assertIsNone(parent.dark_since)
        self.assertIsNone(track.first_ir)
        self.assertFalse(track.lit_requested)
        self.assertFalse(any(track.led.baselines.values()))

    def test_live_peer_close_and_stall_request_a_new_session(self):
        worker = self.worker
        fcntl.fcntl(self.fd, fcntl.F_ADD_SEALS, fcntl.F_SEAL_WRITE | fcntl.F_SEAL_SHRINK)
        for case in ("scene", "stale", "quiet", "closed"):
            if case in ("scene", "stale"):
                self.send(2, exposure="scene" if case == "scene" else "ir", timestamp=1)
            elif case == "closed":
                self.sender.close()
            polls = [True, True, False] if case == "closed" else [True, True, True, False]
            with (
                self.subTest(case=case),
                patch.object(worker.signal, "signal"),
                patch.object(worker.Parent, "poll", side_effect=polls),
                patch.object(worker, "FramePalmModel", return_value=nullcontext()),
                patch.object(worker, "CameraTracking"),
                patch.object(
                    worker,
                    "camera_feed",
                    return_value=nullcontext((self.receiver, self.cameras, {}, 1)),
                ),
                patch.object(
                    worker.select,
                    "select",
                    return_value=([self.receiver] if case != "quiet" else [], [], []),
                ),
                patch.object(worker, "raw_time", return_value=2 if case == "stale" else 1),
                patch.object(worker.time, "monotonic", side_effect=[0, 0, 0, 0, 6]),
                patch.object(worker, "emit") as emit,
            ):
                worker.main("HEADSET")
            self.assertEqual(
                [args.args[0] for args in emit.call_args_list], ["ready", "disconnected"]
            )
            if case in ("scene", "stale"):
                self.assertEqual(json.loads(self.sender.recv(64)), {"release": 2})

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
