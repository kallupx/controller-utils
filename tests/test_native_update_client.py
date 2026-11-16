import hashlib
import io
import json
import os
import ssl
import struct
import subprocess
import sys
import tarfile
import threading
import time
import unittest
from pathlib import Path
from tempfile import TemporaryDirectory

import test_native_download as wire
import tls_fixture

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools"))
from package_release import PACKAGE_FILES

ROOT = Path(__file__).resolve().parents[1]
ARTIFACT = "controller-utils-fixture-linuxarm64.tar.gz"
DRIVER_FILES = (
    "driver.vrdrivermanifest",
    "bin/linuxarm64/driver_controllerutils_native.so",
    "resources/input/sc26_profile.json",
    "resources/input/vrcompositor_bindings_sc26.json",
    "resources/rendermodels/sc26_input/sc26_input.obj",
)
RUNTIME_FILES = ("XRService",)


class Handler(wire.Handler):
    def respond(self):
        if self.path.startswith("/kallupx/controller-utils/releases/"):
            name = self.path.rsplit("/", 1)[-1]
            if "/latest/" in self.path:
                target = (
                    "https://github.com/kallupx/controller-utils/releases/download/v0.1.1/" + name
                )
            else:
                target = (
                    "https://release-assets.githubusercontent.com/fixture/"
                    + name
                    + "?signature=test&expires=123"
                )
            self.send_response(302)
            self.send_header("Location", target)
            self.send_header("Content-Length", "3")
            self.end_headers()
            self.wfile.write(b"abc")
            return
        name = self.path.split("?", 1)[0].removeprefix("/fixture").removeprefix("/")
        if name in self.server.files:
            body = self.server.files[name]
            self.send_response(200)
            self.send_header("Content-Length", str(len(body)))
            self.send_header("Connection", "close")
            self.end_headers()
            if self.path == self.server.hold:
                self.server.held.set()
                time.sleep(4)
            self.wfile.write(body)
        else:
            self.send_response(404)
            self.send_header("Content-Length", "0")
            self.end_headers()


class NativeUpdateClientTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.directory = TemporaryDirectory(prefix="native-update-client-", dir=ROOT / "build")
        cls.work = Path(cls.directory.name)
        cls.certificate, private = tls_fixture.certificate(cls.work, domains=True)
        cls.key = cls.work / "signing-key.pem"
        subprocess.run(
            ["openssl", "genpkey", "-algorithm", "ED25519", "-out", str(cls.key)],
            check=True,
            capture_output=True,
            timeout=10,
        )
        cls.key.chmod(0o600)
        cls.public = subprocess.check_output(
            ["openssl", "pkey", "-in", str(cls.key), "-pubout"], timeout=10
        )
        cls.server = wire.Server(("127.0.0.1", 0), Handler)
        context = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
        context.load_cert_chain(cls.certificate, private)
        cls.server.socket = context.wrap_socket(
            cls.server.socket, server_side=True, do_handshake_on_connect=False
        )
        cls.server.base = f"https://127.0.0.1:{cls.server.server_port}"
        cls.server.requests = []
        cls.server.held = threading.Event()
        cls.thread = threading.Thread(target=cls.server.serve_forever, daemon=True)
        cls.thread.start()
        cls.executable = os.environ.get(
            "CONTROLLER_UTILS_REPLAY", str(ROOT / "build/native_replay")
        )

    @classmethod
    def tearDownClass(cls):
        cls.server.shutdown()
        cls.server.server_close()
        cls.thread.join()
        cls.directory.cleanup()

    def setUp(self):
        self.directory = TemporaryDirectory(prefix="case-", dir=self.work)
        self.addCleanup(self.directory.cleanup)
        self.base = Path(self.directory.name)
        self.root, self.data = self.base / "app", self.base / "data"
        (self.root / "assets").mkdir(parents=True)
        self.data.mkdir()
        self.environment = {**os.environ, "NO_PROXY": "*", "no_proxy": "*"}
        self.channel = self.server.base + "/"
        self.now = 1700000000
        self.requirements = {
            "backend": "native-tracker",
            "driver": dict.fromkeys(DRIVER_FILES, "b" * 64),
            "helper": dict.fromkeys(
                [
                    "controller-utils-helper",
                    "controller-utils-updater",
                    *[
                        "native-tracker/" + name
                        for name in (
                            "adapter.json",
                            "loader.bin",
                            "libnative_tracker.so",
                            "manifest.json",
                        )
                    ],
                ],
                "b" * 64,
            ),
            "runtime": dict.fromkeys(RUNTIME_FILES, "b" * 64),
        }
        (self.root / "release.json").write_text(json.dumps(self.release(1)))
        (self.root / "controller-utils").write_bytes(b"previous-native-program")
        (self.root / "assets/update-public.pem").write_bytes(self.public)
        (self.root / "assets/update-channel.json").write_text(json.dumps({"url": self.channel}))
        (self.root / "assets/tracking-calibration.json").write_bytes(b"private calibration")
        (self.root / "controller-utils.vrmanifest").write_text("private registration")
        self.server.requests.clear()
        self.server.hold = None
        self.server.held.clear()
        self.feed(2)

    def release(self, sequence):
        return {"version": f"0.1.{sequence - 1}", "sequence": sequence, "platform": "linuxarm64"}

    def feed(self, sequence):
        ident = b"\x7fELF\x02\x01\x01" + b"\0" * 9
        program = struct.pack(
            "<16sHHIQQQIHHHHHH", ident, 3, 183, 1, 0, 64, 0, 0, 64, 56, 1, 0, 0, 0
        ) + bytes(56)
        entries = {
            "controller-utils": program,
            "release.json": json.dumps(self.release(sequence)).encode(),
            "LICENSE": b"MIT fixture notice",
            "third_party/openvr/LICENSE": b"OpenVR fixture notice",
        }
        entries.update(
            {
                name: program if name == "controller-utils-launcher" else b"package fixture"
                for name in PACKAGE_FILES
            }
        )
        for name in (
            "controller.stl",
            "controller.surface",
            "controller.buttons",
            "controller-icon.png",
            "controller-icon-license.txt",
            "license.txt",
            "README.md",
        ):
            entries["assets/" + name] = b"fixture asset"
        entries["assets/update-public.pem"] = self.public
        entries["assets/update-channel.json"] = json.dumps({"url": self.channel}).encode()
        for field, folder in (("driver", "native-driver"), ("helper", "compatibility")):
            for name in self.requirements[field]:
                entries[folder + "/" + name] = program
                self.requirements[field][name] = hashlib.sha256(program).hexdigest()
        buffer = io.BytesIO()
        with tarfile.open(fileobj=buffer, mode="w:gz") as output:
            for name, body in entries.items():
                info = tarfile.TarInfo(name)
                info.size = len(body)
                output.addfile(info, io.BytesIO(body))
        archive = buffer.getvalue()
        self.metadata = {
            **self.release(sequence),
            "schema": 5,
            "expires": 4102444800,
            "artifact": ARTIFACT,
            "size": len(archive),
            "sha256": hashlib.sha256(archive).hexdigest(),
            "requirements": self.requirements,
            "notes": "Native fixture release",
        }
        self.server.files = {ARTIFACT: archive}
        self.sign()

    def sign(self):
        path = self.base / "latest.json"
        path.write_text(json.dumps(self.metadata))
        signature = subprocess.check_output(
            ["openssl", "pkeyutl", "-sign", "-rawin", "-inkey", str(self.key), "-in", str(path)],
            timeout=10,
        )
        self.server.files.update({"latest.json": path.read_bytes(), "latest.sig": signature})

    def history(self, **values):
        cache = self.data / "updates"
        cache.mkdir(exist_ok=True)
        (cache / "state.json").write_text(json.dumps(values))

    def run_client(self, steps=None, **values):
        request = {
            "component": "update_client",
            "root": str(self.root),
            "data": str(self.data),
            "ca": str(self.certificate),
            "now": self.now,
            "requirements": self.requirements,
            "machine": "aarch64",
            "lease_busy": False,
            "steps": steps or [{"op": "check", "manual": True}, {"op": "wait"}],
            **values,
        }
        completed = subprocess.run(
            [self.executable],
            input=json.dumps(request) + "\n",
            capture_output=True,
            text=True,
            check=True,
            timeout=30,
            env=self.environment,
        )
        result = json.loads(completed.stdout)
        self.assertNotIn("error", result, completed.stderr)
        self.assertEqual(
            result["fds_before"], result["fds_after"], result.get("descriptor_targets")
        )
        self.assertLess(result["maximum_poll"], 0.2)
        return result

    def test_background_check_stages_without_touching_the_app(self):
        result = self.run_client()
        self.assertEqual(
            result["snapshots"][-1]["message"],
            "Version 0.1.1 is ready. It will install when SteamVR next starts.",
        )
        self.assertEqual(result["release"]["sequence"], 1)
        self.assertEqual((self.root / "controller-utils").read_bytes(), b"previous-native-program")
        pending = self.data / "updates/pending"
        self.assertEqual((pending / ARTIFACT).read_bytes(), self.server.files[ARTIFACT])
        history = json.loads((self.data / "updates/state.json").read_text())
        self.assertEqual(
            (
                history["highest_sequence"],
                history["manual_sequence"],
                history["downloaded_sequence"],
            ),
            (2, 2, 2),
        )
        self.assertFalse((self.data / "updates/download").exists())

    def test_github_redirects_pinned_assets_and_long_lived_release(self):
        self.github_release()
        result = self.run_client(
            [{"op": "check", "manual": True}, {"op": "wait"}, {"op": "activate"}]
        )
        self.assertTrue(result["activated"])
        self.assertEqual(result["release"]["sequence"], 2)
        requests = [path for path, _ in self.server.requests]
        self.assertIn("/kallupx/controller-utils/releases/download/v0.1.1/latest.sig", requests)
        self.assertIn("/kallupx/controller-utils/releases/download/v0.1.1/" + ARTIFACT, requests)
        self.assertNotIn("/kallupx/controller-utils/releases/latest/download/latest.sig", requests)

    def github_release(self):
        self.channel = "https://github.com/kallupx/controller-utils/releases/latest/download/"
        (self.root / "assets/update-channel.json").write_text(json.dumps({"url": self.channel}))
        preload = str(ROOT / "build/fake_dns.so")
        if "sanitize" in Path(self.executable).parts:
            preload = (
                subprocess.check_output(["g++", "-print-file-name=libasan.so"], text=True).strip()
                + ":"
                + preload
            )
        self.environment.update(LD_PRELOAD=preload, CT_HTTPS_TEST_PORT=str(self.server.server_port))
        self.feed(2)
        self.metadata["expires"] = 0
        self.sign()

    def test_stable_github_release_uses_channel_metadata_and_shared_archive(self):
        self.github_release()
        self.server.files["latest-stable.json"] = self.server.files.pop("latest.json")
        self.server.files["latest-stable.sig"] = self.server.files.pop("latest.sig")
        result = self.run_client(
            [{"op": "check", "manual": True}, {"op": "wait"}, {"op": "activate"}],
            os_channel="stable",
        )
        self.assertTrue(result["activated"])
        self.assertEqual(result["release"]["sequence"], 2)
        requests = [path for path, _ in self.server.requests]
        self.assertIn(
            "/kallupx/controller-utils/releases/latest/download/latest-stable.json", requests
        )
        self.assertIn(
            "/kallupx/controller-utils/releases/download/v0.1.1/latest-stable.sig", requests
        )
        self.assertIn("/kallupx/controller-utils/releases/download/v0.1.1/" + ARTIFACT, requests)
        self.assertFalse(
            any(path.endswith("/latest.json") or path.endswith("/latest.sig") for path in requests)
        )

    def test_unsupported_os_channel_refuses_updates_before_downloading(self):
        self.github_release()
        result = self.run_client(os_channel="preview")
        self.assertEqual(
            result["snapshots"][-1]["message"], "Couldn't check for updates. Try again later."
        )
        self.assertEqual(self.server.requests, [])
        self.assertFalse((self.data / "updates/pending").exists())

    def test_supported_channel_switch_keeps_installed_version_and_update_history(self):
        self.github_release()
        self.run_client([{"op": "check", "manual": True}, {"op": "wait"}, {"op": "activate"}])
        self.server.files["latest-stable.json"] = self.server.files.pop("latest.json")
        self.server.files["latest-stable.sig"] = self.server.files.pop("latest.sig")
        self.server.requests.clear()
        result = self.run_client(os_channel="stable")
        self.assertEqual(result["snapshots"][-1]["message"], "You're up to date.")
        self.assertEqual(result["release"]["sequence"], 2)
        history = json.loads((self.data / "updates/state.json").read_text())
        self.assertEqual(history["highest_sequence"], 2)
        self.assertFalse(any(ARTIFACT in path for path, _ in self.server.requests))

    def test_missing_stable_metadata_does_not_fall_back_to_beta(self):
        self.github_release()
        result = self.run_client(os_channel="stable")
        self.assertEqual(result["snapshots"][-1]["message"], "No releases have been published yet.")
        self.assertFalse((self.data / "updates/pending").exists())
        self.assertTrue(all("latest-stable.json" in path for path, _ in self.server.requests))

    def test_empty_repository_reports_no_published_release(self):
        self.server.files = {}
        result = self.run_client()
        self.assertEqual(result["snapshots"][-1]["message"], "No releases have been published yet.")

    def test_complete_download_activation_and_confirmation(self):
        result = self.run_client(
            [{"op": "check", "manual": True}, {"op": "wait"}, {"op": "activate"}]
        )
        self.assertTrue(result["activated"])
        self.assertEqual(result["release"]["sequence"], 2)
        self.assertFalse((self.root / "assets/tracking-calibration.json").exists())
        self.assertEqual(
            json.loads((self.data / "updates/transaction.json").read_text())["phase"], "healthy"
        )

    def test_bad_download_preserves_the_previous_pending_update(self):
        pending = self.data / "updates/pending"
        pending.mkdir(parents=True)
        (pending / "preserve").write_text("earlier download")
        self.server.files[ARTIFACT] = b"tampered"
        result = self.run_client()
        self.assertIn("Couldn't", result["snapshots"][-1]["message"])
        self.assertEqual((pending / "preserve").read_text(), "earlier download")
        self.assertEqual(result["release"]["sequence"], 1)

    def test_signature_failure_prevents_archive_download(self):
        self.server.files["latest.sig"] = b"x" * 64
        result = self.run_client()
        self.assertIn("Couldn't", result["snapshots"][-1]["message"])
        self.assertEqual(
            [path for path, _ in self.server.requests], ["/latest.json", "/latest.sig"]
        )

    def test_incompatible_backend_is_reported(self):
        result = self.run_client(machine="x86_64")
        self.assertEqual(
            result["snapshots"][-1]["message"], "This release needs a native compatibility update."
        )
        self.assertFalse((self.data / "updates/pending").exists())

    def test_up_to_date_does_not_download_an_archive(self):
        self.feed(1)
        result = self.run_client()
        self.assertEqual(result["snapshots"][-1]["message"], "You're up to date.")
        self.assertEqual(len(self.server.requests), 2)

    def test_failed_automatic_release_requires_a_manual_retry(self):
        self.history(failed_sequence=2)
        result = self.run_client(
            [
                {"op": "check", "manual": False},
                {"op": "wait"},
                {"op": "check", "manual": True},
                {"op": "wait"},
            ]
        )
        self.assertEqual(
            result["snapshots"][1]["message"],
            "The previous version was restored. Choose Check now to retry.",
        )
        self.assertIn("is ready", result["snapshots"][-1]["message"])
        self.assertEqual(len(self.server.requests), 5)

    def test_corrupt_history_disables_checks_and_preserves_the_file(self):
        self.history()
        path = self.data / "updates/state.json"
        path.write_text("broken history")
        result = self.run_client()
        self.assertFalse(result["snapshots"][-1]["configured"])
        self.assertIn("history", result["snapshots"][-1]["message"])
        self.assertEqual(path.read_text(), "broken history")
        self.assertFalse(self.server.requests)

    def test_unconfigured_or_disabled_checks_do_no_network_work(self):
        result = self.run_client([{"op": "poll", "automatic": False}])
        self.assertFalse(result["snapshots"][-1]["busy"])
        self.assertFalse(self.server.requests)
        (self.root / "assets/update-channel.json").unlink()
        result = self.run_client([{"op": "poll", "automatic": True}])
        self.assertEqual(result["snapshots"][-1]["message"], "Update server is not configured.")
        self.assertFalse(result["snapshots"][-1]["configured"])

    def test_busy_maintenance_does_not_overwrite_another_updaters_state(self):
        self.history(message="preserve history", last_check=123)
        result = self.run_client(lease_busy=True)
        self.assertIn("Couldn't", result["snapshots"][-1]["message"])
        self.assertEqual(
            json.loads((self.data / "updates/state.json").read_text())["message"],
            "preserve history",
        )
        self.assertFalse(self.server.requests)

    def test_cancellation_is_prompt_and_polling_stays_responsive(self):
        self.server.hold = "/latest.json"
        result = self.run_client(
            [
                {"op": "check", "manual": True},
                {"op": "sleep", "seconds": 0.2},
                {"op": "close"},
                {"op": "check", "manual": True},
            ]
        )
        self.assertTrue(self.server.held.is_set())
        self.assertLess(result["close_time"], 1)
        self.assertFalse(result["snapshots"][-1]["busy"])
        self.assertEqual(result["snapshots"][-1]["message"], "Ready to check for updates.")
        self.assertFalse((self.data / "updates/download").exists())

    def test_twelve_hour_schedule_and_duplicate_check_suppression(self):
        self.history(last_check=self.now - 43200 + 1)
        result = self.run_client(
            [
                {"op": "poll", "automatic": True},
                {"op": "time", "now": self.now + 1},
                {"op": "poll", "automatic": True},
                {"op": "check", "manual": True},
                {"op": "wait"},
                {"op": "poll", "automatic": True},
            ]
        )
        self.assertFalse(result["snapshots"][0]["busy"])
        self.assertEqual(len(self.server.requests), 3)
        self.assertIn("is ready", result["snapshots"][-1]["message"])

    def test_repeated_checks_replace_pending_and_release_resources(self):
        result = self.run_client(
            [step for _ in range(10) for step in ({"op": "check", "manual": True}, {"op": "wait"})]
        )
        self.assertEqual(result["captures"], 10)
        self.assertEqual(len(self.server.requests), 30)
        self.assertEqual(len(list((self.data / "updates/pending").iterdir())), 3)

    def test_signed_rollback_is_rejected_using_persisted_history(self):
        self.history(highest_sequence=3)
        result = self.run_client()
        self.assertIn("Couldn't", result["snapshots"][-1]["message"])
        self.assertEqual(len(self.server.requests), 2)
        self.assertEqual(
            json.loads((self.data / "updates/state.json").read_text())["highest_sequence"], 3
        )
        self.assertFalse((self.data / "updates/pending").exists())

    def test_pending_symlink_is_preserved(self):
        cache = self.data / "updates"
        cache.mkdir()
        outside = self.base / "preserve"
        outside.mkdir()
        (outside / "file").write_text("preserve")
        (cache / "pending").symlink_to(outside, target_is_directory=True)
        result = self.run_client()
        self.assertIn("Couldn't", result["snapshots"][-1]["message"])
        self.assertTrue((cache / "pending").is_symlink())
        self.assertEqual((outside / "file").read_text(), "preserve")

    def test_backwards_clock_change_allows_a_fresh_check(self):
        self.history(last_check=self.now + 1)
        result = self.run_client([{"op": "poll", "automatic": True}, {"op": "wait"}])
        self.assertIn("is ready", result["snapshots"][-1]["message"])
        self.assertEqual(len(self.server.requests), 3)


if __name__ == "__main__":
    unittest.main()
