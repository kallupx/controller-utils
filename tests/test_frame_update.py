import contextlib
import fcntl
import hashlib
import io
import json
import os
import runpy
import subprocess
import tarfile
import tempfile
import unittest
from pathlib import Path
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[1]
UPDATER = runpy.run_path(str(ROOT / "packaging/bootstrap.py"))


class FrameUpdateTest(unittest.TestCase):
    def setUp(self):
        work = tempfile.TemporaryDirectory()
        self.addCleanup(work.cleanup)
        self.work = Path(work.name)
        self.root = self.work / "app"
        self.data = self.work / "app-data"
        self.feed = self.work / "feed"
        self.data.mkdir()
        self.feed.mkdir()
        assets = self.root / "assets"
        assets.mkdir(parents=True)
        self.key = self.work / "key.pem"
        subprocess.run(
            ["openssl", "genpkey", "-algorithm", "Ed25519", "-out", self.key], check=True
        )
        subprocess.run(
            ["openssl", "pkey", "-in", self.key, "-pubout", "-out", assets / "update-public.pem"],
            check=True,
        )
        self.marker = self.data / "installed"
        self.release = {"version": "0.3.0", "sequence": 15, "platform": "linuxarm64"}
        self.current = {"version": "0.2.0", "sequence": 14, "platform": "linuxarm64"}
        self.data.joinpath("calibration").write_text("preserve this")

    def test_busy_updater_preserves_the_running_operations_status(self):
        (self.root / "release.json").write_text(json.dumps(self.release))
        status = self.data / "frame-update.json"
        status.write_text('{"state":"busy","message":"Installing update…"}')
        before = status.read_bytes()
        main = UPDATER["main"]
        with (
            (self.data / "frame-update.lock").open("a") as lock,
            patch.dict(
                main.__globals__,
                __file__=str(self.root / "frame_update.py"),
                frame_channel=lambda: "beta",
            ),
            patch("sys.argv", ["frame_update.py", "check"]),
            contextlib.redirect_stderr(io.StringIO()) as stderr,
        ):
            fcntl.flock(lock, fcntl.LOCK_EX)
            self.assertEqual(main(), 1)
        self.assertIn("already running", stderr.getvalue())
        self.assertEqual(status.read_bytes(), before)

    def sign(self, channel, mode="--unattended"):
        filename = UPDATER["artifact"](channel, self.release["version"])
        package = self.work / filename.removesuffix("-linuxarm64.tar.gz")
        (package / "app").mkdir(parents=True)
        (package / "app/release.json").write_text(json.dumps(self.release))
        (package / "install.py").write_text(
            "import sys\nfrom pathlib import Path\n"
            f"assert sys.argv[1:] == {[mode] if mode else []!r}\n"
            f"Path({str(self.marker)!r}).touch()\n"
        )
        archive = self.feed / filename
        with tarfile.open(archive, "w:gz") as contents:
            contents.add(package, arcname=package.name)
        metadata, signature = (self.feed / name for name in UPDATER["paths"](channel))
        metadata.write_text(
            json.dumps(
                dict(
                    schema=1,
                    channel=channel,
                    **self.release,
                    artifact=filename,
                    size=archive.stat().st_size,
                    sha256=hashlib.sha256(archive.read_bytes()).hexdigest(),
                )
            )
        )
        subprocess.run(
            [
                "openssl",
                "pkeyutl",
                "-sign",
                "-inkey",
                self.key,
                "-rawin",
                "-in",
                metadata,
                "-out",
                signature,
            ],
            check=True,
        )
        return archive

    def test_signed_update_stages_and_applies_for_its_channel(self):
        self.sign("beta")
        self.sign("stable")

        def download(url, path, maximum):
            if "api.github.com" in url:
                path.write_text(
                    json.dumps(
                        [
                            {
                                "tag_name": "v0.4.0",
                                "draft": False,
                                "assets": [{"name": name} for name in UPDATER["paths"]("stable")],
                            },
                            {
                                "tag_name": "v0.3.0",
                                "draft": False,
                                "assets": [{"name": name} for name in UPDATER["paths"]("beta")],
                            },
                        ]
                    )
                )
            else:
                self.assertIn("/download/v0.3.0/", url)
                contents = (self.feed / path.name).read_bytes()
                self.assertLessEqual(len(contents), maximum)
                path.write_bytes(contents)

        with patch.dict(UPDATER["check"].__globals__, download=download):
            UPDATER["check"](self.root, self.data, "beta", self.current)
        self.assertFalse(self.marker.exists())
        self.assertEqual(
            json.loads((self.data / "frame-update.json").read_text())["state"], "ready"
        )
        UPDATER["apply"](self.root, self.data, "beta", self.current)
        self.assertTrue(self.marker.exists())
        self.assertEqual((self.data / "calibration").read_text(), "preserve this")

    def test_bootstrap_uses_signed_packages_for_install_and_removal(self):
        self.assertEqual(UPDATER["PUBLIC_KEY"], (ROOT / "assets/update-public.pem").read_text())
        self.sign("stable", mode=None)
        self.sign("beta", mode="--uninstall")
        base = self.work / ".local/share/controller-utils"
        (base / "app").mkdir(parents=True)
        (base / "app/release.json").write_text(json.dumps(self.release))

        def download(url, path, maximum):
            if url == UPDATER["API"]:
                path.write_text(
                    json.dumps(
                        [
                            {
                                "tag_name": "v0.3.0",
                                "draft": False,
                                "assets": [{"name": name} for name in UPDATER["paths"]("stable")],
                            }
                        ]
                    )
                )
            else:
                self.assertIn("/download/v0.3.0/", url)
                path.write_bytes((self.feed / path.name).read_bytes())

        def setup(data, package, uninstall):
            self.assertEqual(data, base / "app-data")
            self.assertEqual(os.environ["XDG_RUNTIME_DIR"], f"/run/user/{os.getuid()}")
            self.assertNotIn("DBUS_SESSION_BUS_ADDRESS", os.environ)
            command = ["/usr/bin/env", "python3", package / "install.py"]
            if uninstall:
                command.append("--uninstall")
            return subprocess.run(command, check=True).returncode

        key = (self.root / "assets/update-public.pem").read_text()
        for channel, arguments in (("stable", []), ("beta", ["--uninstall"])):
            with (
                self.subTest(channel=channel),
                patch.object(Path, "home", return_value=self.work),
                patch("os.geteuid", return_value=1000),
                patch("platform.machine", return_value="aarch64"),
                patch.dict(
                    os.environ,
                    XDG_RUNTIME_DIR="/run/user/1000/nested_plasma",
                    DBUS_SESSION_BUS_ADDRESS="unix:path=/tmp/desktop-bus",
                ),
                patch.dict(
                    UPDATER["bootstrap"].__globals__,
                    PUBLIC_KEY=key,
                    download=download,
                    frame_setting={"tracked-variant": "vr", "tracked-branch": channel}.__getitem__,
                    terminal_setup=setup,
                ),
            ):
                self.assertEqual(UPDATER["bootstrap"](arguments), 0)
                self.assertTrue(self.marker.is_file())
                self.marker.unlink()

    def test_signature_and_archive_are_checked_before_install(self):
        archive = self.sign("stable")
        metadata = self.feed / UPDATER["paths"]("stable")[0]
        value = UPDATER["manifest"](self.feed, self.root / "assets/update-public.pem", "stable")
        original = metadata.read_bytes()
        metadata.write_bytes(original + b" ")
        with self.assertRaises(subprocess.CalledProcessError):
            UPDATER["manifest"](self.feed, self.root / "assets/update-public.pem", "stable")
        archive.write_bytes(b"x" * value["size"])
        with self.assertRaisesRegex(ValueError, "checksum"):
            UPDATER["verify_archive"](archive, value)
        self.assertFalse(self.marker.exists())

    def test_legacy_bridge_installs_its_pinned_release(self):
        self.sign("beta", "--legacy-migration")

        def download(url, path, maximum):
            self.assertIn("/download/v0.3.0/", url)
            content = (self.feed / path.name).read_bytes()
            self.assertLessEqual(len(content), maximum)
            path.write_bytes(content)

        with patch.dict(UPDATER["migrate"].__globals__, download=download):
            UPDATER["migrate"](self.root, self.data, "beta", self.release)
        self.assertTrue(self.marker.exists())
        self.assertEqual((self.data / "calibration").read_text(), "preserve this")

    def test_archive_cannot_write_outside_its_directory(self):
        archive = self.work / "invalid.tar.gz"
        with tarfile.open(archive, "w:gz") as contents:
            contents.add(self.key, arcname="../escape")
        with self.assertRaisesRegex(ValueError, "contents"):
            UPDATER["extract"](archive, self.feed)
        self.assertFalse((self.work / "escape").exists())
