import errno
import json
import os
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools"))
import frame_debug as debug  # noqa: E402


class FrameDebugTest(unittest.TestCase):
    def test_recording_is_explicit_and_errors_identify_the_failing_operation(self):
        with tempfile.TemporaryDirectory() as directory, patch.object(debug, "send") as send:
            data = Path(directory)
            marker = data / "debug-capture.json"
            for previous in (
                None,
                "{",
                "[]",
                '{"id":"123","recording":1,"stopped":0}',
                '{"id":"123","recording":false,"stopped":456}',
            ):
                if previous is not None:
                    marker.write_text(previous)
                debug.record("Camera ready", data)
                debug.failure("Camera", OSError(errno.EAGAIN, "private value"), data)
                send.assert_not_called()
            marker.write_text('{"id":"123","recording":true,"stopped":0}')
            try:
                raise BlockingIOError(errno.EAGAIN, "private value")
            except BlockingIOError as error:
                debug.failure("Palm helper launch", error, data)
            token, message = send.call_args.args
            self.assertEqual(token, "123")
            self.assertIn("Palm helper launch failed: BlockingIOError errno=11", message)
            self.assertIn("test_frame_debug.py/", message)
            self.assertNotIn("private value", message)

    def test_export_is_private_scoped_and_can_be_resaved_after_stopping(self):
        token = "1795540000000000"
        with (
            tempfile.TemporaryDirectory() as directory,
            patch.object(Path, "home", return_value=Path(directory)),
        ):
            home = Path(directory)
            data, downloads = home / "app-data", home / "Downloads"
            data.mkdir()
            xr = home / ".local/share/Steam/logs/xrservice.txt"
            xr.parent.mkdir(parents=True)
            (data / "debug-capture.json").write_text(
                json.dumps(dict(id=token, recording=True, stopped=0))
            )
            previous = xr.parent / "XRService-2026.11.24_09-00-00.log"
            previous.write_text(
                "Tue Nov 24 2026 09:03:00 FATAL ERROR: VIDIOC_STREAMON returned errno 11 PRIVATE\n"
            )
            modified = (int(token) + 1000) * 1000
            os.utime(previous, ns=(modified, modified))
            older = xr.parent / "XRService-2026.11.23_09-00-00.log"
            older.write_text("09:00:00 FATAL ERROR: VIDIOC_STREAMOFF returned errno 90\n")
            os.utime(older, ns=(modified - 2000000, modified - 2000000))
            xr.write_text(
                "09:14:00 Cached 5 camera paths room=PRIVATE\n"
                "09:14:01 Received passthrough camera connection event (connected=0) PRIVATE\n"
                "09:14:02 ISP enabled for tracking cameras (main VFE available) PRIVATE\n"
                "09:14:02 Setting up cameras camerasToUse: 1111 PRIVATE\n"
                "09:14:02 Set upper camera driver not in-sync PRIVATE\n"
                "09:14:03 FATAL ERROR: Couldn't get camera path for upper_left camera; private serial PRIVATE\n"
                "PRIVATE controller coordinates\n"
            )
            settings = home / ".config/openvr/config/cv/xrservice/XRServiceSettings.json"
            settings.parent.mkdir(parents=True)
            settings.write_text(
                '{"xrSessionSettings":{"lightSourceFrequencyDetectionMode":"force60Hz"},"serial":"PRIVATE"}'
            )
            (data / "settings.json").write_text('{"brightness":70,"nickname":"PRIVATE"}')
            with patch.object(
                debug,
                "journal",
                side_effect=[
                    [
                        {
                            "MESSAGE": "Camera ready",
                            "__REALTIME_TIMESTAMP": token,
                            "UNRELATED": "PRIVATE",
                        }
                    ],
                    [],
                ],
            ) as read:
                target, complete = debug.export(data, token, 0, downloads)
            self.assertTrue(complete)
            self.assertIn("CU_CAPTURE_ID=" + token, read.call_args_list[0].args[0])
            self.assertIn(
                "COREDUMP_EXE=/opt/steamvr/drivers/cv/bin/linuxarm64/XRService",
                read.call_args_list[1].args[0],
            )
            self.assertEqual(target.stat().st_mode & 0o777, 0o600)
            report = target.read_text()
            for part in (
                "Capture still active",
                "Camera ready",
                "registered cameras=5",
                "passthrough accessory connected=0",
                "tracking camera ISP=enabled",
                "tracking camera mask=1111",
                "upper cameras not in-sync",
                "missing camera=upper_left",
                "Tue Nov 24 2026 09:03:00 fatal operation=VIDIOC_STREAMON errno=11",
                "Anti-Flicker mode=force60Hz",
                "brightness=70",
            ):
                self.assertIn(part, report)
            self.assertNotIn("PRIVATE", report)
            self.assertNotIn("STREAMOFF", report)
            with patch.object(
                debug, "journal", side_effect=subprocess.TimeoutExpired("journalctl", 8)
            ):
                with self.assertRaisesRegex(OSError, "Previous report kept"):
                    debug.export(data, token, int(token) + 5000, downloads)
                self.assertEqual(target.read_text(), report)
                partial, complete = debug.export(data, str(int(token) + 1), 0, downloads)
                self.assertFalse(complete)
                self.assertIn("Capture journal unavailable", partial.read_text())
            with patch.object(
                debug,
                "journal",
                side_effect=[[{"MESSAGE": "Camera ready", "__REALTIME_TIMESTAMP": token}], []],
            ):
                saved, complete = debug.export(data, token, int(token) + 5000, downloads)
            self.assertTrue(complete)
            self.assertEqual(set(downloads.iterdir()), {saved, partial})
            self.assertEqual(target, saved)
            self.assertIn("Capture stopped", saved.read_text())
