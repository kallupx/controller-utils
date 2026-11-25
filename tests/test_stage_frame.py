import ast
import contextlib
import io
import json
import os
import runpy
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path
from types import SimpleNamespace
from unittest.mock import Mock, patch

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))
import stage_frame  # noqa: E402
from frame_camera_runtime import NUMPY, numpy_python  # noqa: E402


def arm64_file(path):
    header = b"\x7fELF\x02\x01" + bytes(12) + b"\xb7\x00"
    return header + path.name.encode()


class FramePackageTest(unittest.TestCase):
    def test_setup_requires_camera_socket_unless_kernel_devices_are_missing(self):
        ready = runpy.run_path(str(ROOT / "packaging/frame_install.py"))["ready"]
        drivers = [
            f"@controller-utils.{name}.v1.{os.getuid()}" for name in ("optical", "input", "beam")
        ]
        camera = "/test/controller-utils-camera.sock"
        optical, input_gate, beam = drivers
        both = ("input", "beam")
        for warning, sockets, gates, succeeds in [
            (None, drivers, both, False),
            (None, [*drivers, camera], both, True),
            ("Restart Frame", drivers, both, True),
            ("Restart Frame", drivers[:-1], both, False),
            (None, [optical, beam, camera], ("beam",), True),
            (None, [optical, beam, camera], both, False),
            (None, [optical, beam], ("beam",), False),
            (None, [optical, input_gate, camera], ("beam",), False),
            (None, [optical, input_gate, camera], ("input",), True),
            (None, [optical, camera], ("input",), False),
            (None, [optical, camera], (), True),
            (None, [optical], (), False),
            (None, [input_gate, beam, camera], (), False),
        ]:
            entries = "\n".join(f"0: 0 0 00010000 0 0 0 {name}" for name in sockets)
            with (
                self.subTest(warning=warning, sockets=sockets, gates=gates),
                patch.dict(os.environ, XDG_RUNTIME_DIR="/test"),
                patch("runpy.run_path", return_value={"camera_device_warning": lambda: warning}),
                patch.object(Path, "read_text", return_value=entries),
                patch("time.monotonic", side_effect=[0, 0, 91]),
                patch("time.sleep"),
                contextlib.redirect_stderr(io.StringIO()) as notice,
            ):
                if succeeds:
                    ready(gates)
                else:
                    with self.assertRaisesRegex(RuntimeError, "connections"):
                        ready(gates)
                self.assertEqual(notice.getvalue().strip(), warning or "")

    def test_concurrent_setup_explains_lock_and_allows_retry_after_release(self):
        setup_lock = runpy.run_path(str(ROOT / "packaging/frame_install.py"))["setup_lock"]
        with tempfile.TemporaryDirectory() as directory:
            data = Path(directory)
            with setup_lock(data):
                with self.assertRaisesRegex(RuntimeError, "Another Controller Utils setup"):
                    with setup_lock(data):
                        self.fail("Two installers acquired the same lock")
            with setup_lock(data):
                pass

    def test_preflight_accepts_a_new_build_on_the_same_channel(self):
        main = runpy.run_path(str(ROOT / "packaging/frame_install.py"))["main"]
        output = io.StringIO()

        def command(*args, **kwargs):
            return subprocess.CompletedProcess(
                args,
                0,
                stdout={"tracked-branch": "stable", "tracked-variant": "vr"}.get(args[-1], ""),
            )

        with (
            patch.dict(
                main.__globals__,
                {
                    "verify_package": lambda _: ("stable", "20261022.6150929"),
                    "command": command,
                    "LEGACY": (),
                    "calibration": lambda *args: "{}",
                },
            ),
            patch.object(sys, "argv", ["install.py", "--check"]),
            patch("os.geteuid", return_value=1000),
            patch("platform.machine", return_value="aarch64"),
            patch.object(Path, "read_text", return_value="BUILD_ID=20261029.6150930\n"),
            patch("subprocess.run") as palm,
            contextlib.redirect_stdout(output),
        ):
            main()
        palm.assert_called_once()
        self.assertIn("Running 20261029.6150930", output.getvalue())
        self.assertIn("Frame stable package preflight passed", output.getvalue())

    def test_setup_only_skips_the_unsupported_menu_features(self):
        main = runpy.run_path(str(ROOT / "packaging/frame_install.py"))["main"]
        for code, gates in [
            (0, ("input", "beam")),
            (3, ("beam",)),
            (4, ("input",)),
            (5, ()),
            (1, None),
            (2, None),
            (6, None),
            (-9, None),
        ]:
            install = Mock()

            def command(*args, **kwargs):
                value = {"tracked-branch": "beta", "tracked-variant": "vr"}.get(args[-1])
                return subprocess.CompletedProcess(args, code if value is None else 0, stdout=value)

            with (
                self.subTest(code=code),
                patch.dict(
                    main.__globals__,
                    verify_package=lambda _: ("beta", "test"),
                    calibration=lambda *args: "calibration",
                    command=command,
                    install=install,
                ),
                patch.object(sys, "argv", ["install.py", "--unattended"]),
                patch("os.geteuid", return_value=1000),
                patch("platform.machine", return_value="aarch64"),
                patch.object(Path, "read_text", return_value="BUILD_ID=test\n"),
                patch("subprocess.run") as palm,
                contextlib.redirect_stderr(io.StringIO()) as notice,
            ):
                if gates is not None:
                    main()
                    self.assertEqual(
                        install.call_args.args[1:], ("calibration", True, False, gates)
                    )
                    self.assertEqual(
                        "menu pointer unavailable" in notice.getvalue(), "input" not in gates
                    )
                    self.assertEqual(
                        "laser may stay visible" in notice.getvalue(), "beam" not in gates
                    )
                    palm.assert_called_once()
                else:
                    with self.assertRaises(subprocess.CalledProcessError):
                        main()
                    install.assert_not_called()
                    palm.assert_not_called()

    def test_installer_and_worker_support_stable_python(self):
        scripts = [
            ROOT / "packaging/bootstrap.py",
            ROOT / "packaging/frame_install.py",
            *(ROOT / "tools" / name for name in stage_frame.OPTICAL_FILES if name.endswith(".py")),
        ]
        for script in scripts:
            ast.parse(script.read_text(), filename=str(script), feature_version=(3, 12))

    def test_channel_packages_are_independent_and_contain_only_the_shared_camera_backend(self):
        verify = runpy.run_path(str(ROOT / "packaging/frame_install.py"))["verify_package"]
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            stable, beta = root / "stable", root / "beta"
            with (
                patch.object(stage_frame, "read_source", side_effect=arm64_file),
                patch.object(stage_frame, "check_stable_abi") as check_abi,
            ):
                stage_frame.stage(stable, "stable")
                stable_checks = check_abi.call_count
                stage_frame.stage(beta, "beta")
            self.assertGreater(stable_checks, 0)
            self.assertEqual(check_abi.call_count, stable_checks)
            self.assertEqual(verify(stable), ("stable", stage_frame.STEAMOS_BUILDS["stable"]))
            self.assertEqual(verify(beta), ("beta", stage_frame.STEAMOS_BUILDS["beta"]))
            manifest = json.loads((stable / "frame-package.json").read_bytes())
            self.assertIn("app/optical/frame_camera_hook.so", manifest["files"])
            self.assertIn(
                "app/native-driver/bin/linuxarm64/driver_controllerutils_native.so",
                manifest["files"],
            )
            self.assertFalse(
                any(
                    "native-tracker" in name or "controller-utils-helper" in name
                    for name in manifest["files"]
                )
            )
            (beta / "app/controller-utils").write_bytes(b"changed")
            self.assertEqual(verify(stable), ("stable", stage_frame.STEAMOS_BUILDS["stable"]))
            with self.assertRaisesRegex(ValueError, "checksum"):
                verify(beta)

    def test_camera_uses_the_installed_python_after_switching_channels(self):
        with tempfile.TemporaryDirectory() as directory:
            data = Path(directory)

            def provision(*args):
                if args[1:3] == ("-m", "venv"):
                    executable = args[-1] / "bin/python"
                    executable.parent.mkdir(parents=True)
                    executable.touch()

            with patch("frame_camera_runtime.run", side_effect=provision) as run:
                for tag in ("cpython-312", "cpython-314", "cpython-312"):
                    with patch.object(sys.implementation, "cache_tag", tag):
                        python = numpy_python(data)
                    self.assertEqual(python, data / f"numpy-{NUMPY}-{tag}/bin/python")
                    self.assertTrue(python.is_file())
                self.assertEqual(run.call_count, 6)

    def test_desktop_artifacts_cannot_be_staged_for_frame(self):
        with tempfile.TemporaryDirectory() as directory:
            with patch.object(stage_frame, "read_source", return_value=b"desktop"):
                with self.assertRaisesRegex(ValueError, "ARM64"):
                    stage_frame.stage(Path(directory) / "package", "stable")

    def test_stable_package_rejects_a_beta_libc_requirement(self):
        with patch.object(stage_frame.subprocess, "check_output", return_value="Name: GLIBC_2.43"):
            with self.assertRaisesRegex(ValueError, "newer than SteamOS stable"):
                stage_frame.check_stable_abi(Path("controller-utils"))

    def test_installer_restores_the_previous_app_if_a_hook_fails_to_start(self):
        source = ROOT / "packaging/frame_install.py"
        install = runpy.run_path(str(source))["install"]
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            base, package = root / "installed", root / "package"
            app = base / "app"
            (app / "optical").mkdir(parents=True)
            (app / "controller-utils").write_bytes(b"previous app")
            (package / "app/optical").mkdir(parents=True)
            (package / "app/controller-utils").write_bytes(b"new app")
            (package / "controller-utils").write_bytes(b"new launcher")
            rule = root / "70-controller-utils.rules"
            rule.write_bytes(b"udev rules")
            (package / rule.name).write_bytes(rule.read_bytes())
            keep = root / "controller-utils-frame.conf"
            (package / keep.name).write_text("retention")
            desktop = root / "org.controllerutils.frame.desktop"

            def command(*args, **kwargs):
                if args[-1] == "--prepare-registration":
                    desktop.write_text("new entry")

            config = base / "app-data/camera-calibration.json"
            config.parent.mkdir()
            config.write_text("previous calibration")

            def fail(required_gates):
                raise RuntimeError("beam hook failed")

            with patch.dict(
                install.__globals__,
                {
                    "BASE": base,
                    "UNITS": root / "systemd",
                    "RULE": rule,
                    "KEEP": keep,
                    "DESKTOP": desktop,
                    "systemctl": lambda *args, **kwargs: SimpleNamespace(
                        returncode=int(args[0] == "is-enabled")
                    ),
                    "command": command,
                    "ready": fail,
                },
            ):
                with self.assertRaisesRegex(RuntimeError, "beam hook failed"):
                    install(package, "new calibration")
            self.assertEqual((app / "controller-utils").read_bytes(), b"previous app")
            self.assertEqual(config.read_text(), "previous calibration")
            self.assertFalse((base / "native-driver").is_symlink())
            self.assertFalse(desktop.exists())
            self.assertFalse(keep.exists())

    def test_restart_accepts_a_replacement_service_job(self):
        code = runpy.run_path(str(ROOT / "packaging/frame_install.py"))["start_service"]
        canceled = SimpleNamespace(returncode=1)
        with (
            patch.dict(
                code.__globals__,
                systemctl=lambda *a, **kw: (
                    canceled if a[0] == "start" else SimpleNamespace(stdout=next(states))
                ),
            ),
            patch("time.sleep"),
        ):
            states = iter(["activating", "active"])
            code("steamvr.service")

    def test_fresh_install_owns_and_removes_its_policy(self):
        install = runpy.run_path(str(ROOT / "packaging/frame_install.py"))["install"]
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            base, package = root / "installed", root / "package"
            desktop = root / "org.controllerutils.frame.desktop"
            rule, keep = root / "70-controller-utils.rules", root / "controller-utils-frame.conf"
            (package / "app/optical").mkdir(parents=True)
            (package / "app/controller-utils").write_text("app")
            (package / "controller-utils").write_text("launcher")
            for path in (rule, keep):
                (package / path.name).write_text(path.name)
            calls = []

            def command(*args, **kwargs):
                calls.append(args)
                if args[-1] == "--prepare-registration":
                    desktop.write_text("entry")
                if args[-1] == "--register" and registration_fails:
                    raise RuntimeError("registration failed")
                if args[-1] == "--unregister":
                    desktop.unlink()
                if args[:2] == ("sudo", "install"):
                    args[-1].write_bytes(args[-2].read_bytes())
                if args[:2] == ("sudo", "rm"):
                    for path in args[4:]:
                        path.unlink(missing_ok=True)
                return SimpleNamespace(returncode=int(args[0] == "is-enabled"))

            with patch.dict(
                install.__globals__,
                BASE=base,
                UNITS=root / "systemd",
                DESKTOP=desktop,
                RULE=rule,
                KEEP=keep,
                validate_calibration=lambda *args: None,
                ready=lambda required_gates: None,
                command=command,
                systemctl=command,
            ):
                registration_fails = True
                with self.assertRaisesRegex(RuntimeError, "registration failed"):
                    install(package, "calibration")
                self.assertFalse(any(path.exists() for path in (rule, keep, desktop, base / "app")))

                registration_fails = False
                install(package, "calibration")
                self.assertTrue(all(path.is_file() for path in (rule, keep, desktop)))
                install.__globals__["uninstall"](package)
                self.assertTrue(any("removedriver" in call for call in calls))
                self.assertFalse(any(path.exists() for path in (rule, keep, desktop, base / "app")))
                self.assertEqual(
                    (base / "app-data/camera-calibration.json").read_text(), "calibration"
                )

    def test_legacy_install_requires_its_ownership_checked_uninstaller(self):
        detect = runpy.run_path(str(ROOT / "packaging/frame_install.py"))["legacy_installation"]
        with tempfile.TemporaryDirectory() as directory:
            legacy = Path(directory) / "controller-utils-maintenance.service"
            legacy.touch()
            with patch.dict(detect.__globals__, LEGACY=(legacy,)):
                with self.assertRaisesRegex(ValueError, "original Controller Utils uninstaller"):
                    detect()

    def test_factory_calibration_uses_the_serial_and_pixel_convention(self):
        factory = runpy.run_path(str(ROOT / "tools/frame_camera_calibration.py"))["factory"]
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            (root / "device_config.json").write_text('{"device_serial_number":"HEADSET"}')
            rig = {
                "metadata": {"device_serial": "HEADSET", "pixel_coordinates_convention": "integer"},
                "cameras": [
                    {
                        "sourceCamera": "slam_left",
                        "serial": "CAMERA",
                        "intrinsics": [{"cameraModel": "kb", "cx": 120, "cy": 100}],
                    }
                ],
            }
            (root / "xrservice.json").write_text(json.dumps(rig))
            value = factory(root)
            self.assertEqual(value["rig"]["metadata"]["pixel_coordinates_convention"], "float")
            self.assertEqual(value["rig"]["cameras"][0]["intrinsics"], {"cx": 120.5, "cy": 100.5})
            self.assertEqual(value["metadata"]["cameras"]["slam_left"]["serialNumber"], "CAMERA")

    def exercise_legacy_upgrade(self, migrate):
        install = runpy.run_path(str(ROOT / "packaging/frame_install.py"))["install"]
        code = install.__globals__
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            base, package = root / "installed", root / "package"
            rule, keep = root / "70-controller-utils.rules", root / "controller-utils-frame.conf"
            units = root / "systemd"
            units.mkdir()
            unit = units / code["LEGACY_UNIT"]
            unit.write_text(code["LEGACY_UNIT_TEXT"])
            (base / "app").mkdir(parents=True)
            (base / "app/controller-utils").write_text("old app")
            data = base / "app-data"
            data.mkdir()
            settings = data / "settings.json"
            settings.write_text('{"automatic_updates":false}')
            (package / "app/optical").mkdir(parents=True)
            (package / "app/controller-utils").write_text("new app")
            (package / "controller-utils").write_text("launcher")
            for path in (rule, keep):
                (package / path.name).write_text(path.name)
            rule.write_text(rule.name)
            calls = []

            def command(*args, **kwargs):
                calls.append(args)
                if args[-1] == "--repair-status":
                    return SimpleNamespace(
                        returncode=0, stdout="controller-utils-frame-migration-v1"
                    )
                if args[:2] == ("sudo", "install"):
                    args[-1].write_bytes(args[-2].read_bytes())
                if args == ("sudo", str(code["LEGACY_UPDATER"]), "--uninstall"):
                    rule.unlink()
                if args == ("sudo", "-n", str(code["LEGACY_UPDATER"]), "--confirm"):
                    keep.write_bytes((package / keep.name).read_bytes())
                return SimpleNamespace(returncode=0)

            with patch.dict(
                code,
                BASE=base,
                UNITS=units,
                DESKTOP=root / "app.desktop",
                RULE=rule,
                KEEP=keep,
                legacy_installation=lambda: True,
                validate_calibration=lambda *args: None,
                ready=lambda required_gates: calls.append(("ready", required_gates)),
                command=command,
                systemctl=command,
            ):
                with self.assertRaisesRegex(ValueError, "administrator authorization"):
                    install(package, "calibration", unattended=True)
                self.assertFalse(calls)
                install(package, "calibration", migrate=migrate, required_gates=("input",))
            updater = str(code["LEGACY_UPDATER"])
            retire = (
                ("sudo", "-n", updater, "--confirm")
                if migrate
                else ("sudo", updater, "--uninstall")
            )
            self.assertEqual(("sudo", "-v") in calls, not migrate)
            self.assertLess(calls.index(("ready", ("input",))), calls.index(retire))
            self.assertEqual(calls.count(("ready", ("input",))), 2)
            self.assertTrue(rule.is_file() and keep.is_file())
            self.assertFalse(unit.exists())
            self.assertEqual((base / "app/controller-utils").read_text(), "new app")
            self.assertEqual(settings.read_text(), '{"automatic_updates":false}')
            self.assertEqual((data / "camera-calibration.json").read_text(), "calibration")

    def test_upgrade_from_legacy_preserves_settings_and_retires_old_services(self):
        for migrate in (False, True):
            with self.subTest(migrate=migrate):
                self.exercise_legacy_upgrade(migrate)
