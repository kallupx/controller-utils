#!/usr/bin/env python3
"""Install a shared-camera package for the active Steam Frame channel."""

import argparse
import fcntl
import hashlib
import json
import os
import platform
import re
import runpy
import shutil
import signal
import stat
import subprocess
import sys
import tempfile
import time
from contextlib import contextmanager
from pathlib import Path, PurePosixPath

VR = Path("/opt/steamvr/bin/linuxarm64")
PALM = Path(
    "/opt/steamvr/drivers/cv/resources/minControllerFeatureNNModel/K2V3/"
    "2024-12-13_16-02_StereoSQRT2-half-WithLongSleevesSynthetic_ch199_partial_AU_WU.onnx"
)
BASE = Path.home() / ".local/share/controller-utils"
DEBUG = None
UNITS = Path.home() / ".local/share/systemd/user"
RULE = Path("/etc/udev/rules.d/70-controller-utils.rules")
KEEP = Path("/etc/atomic-update.conf.d/controller-utils-frame.conf")
DESKTOP = Path.home() / ".local/share/applications/org.controllerutils.frame.desktop"
DATASETS = Path.home() / ".config/openvr/config/cv/xrservice/datasets"
PERSIST = Path("/persist")
LEGACY_UPDATER = Path("/var/lib/controller-utils/controller-utils-updater")
LEGACY_UNIT = "controller-utils-update.service"
LEGACY_UNIT_TEXT = """[Unit]
Description=Install verified Controller Utils updates
Before=steamvr.service

[Service]
Type=oneshot
ExecStartPre=/usr/bin/sudo -n /var/lib/controller-utils/controller-utils-updater --repair
ExecStart=/usr/bin/sudo -n /var/lib/controller-utils/controller-utils-updater --apply
TimeoutStartSec=180

[Install]
WantedBy=steamvr.service
"""
LEGACY = tuple(
    map(
        Path,
        (
            "/var/lib/controller-utils",
            "/var/lib/controller-utils-updates",
            "/etc/systemd/system/controller-utils-maintenance.service",
            "/etc/systemd/system/multi-user.target.wants/controller-utils-maintenance.service",
            "/etc/atomic-update.conf.d/controller-utils.conf",
        ),
    )
)


def command(*args, capture=False, check=True, input=None):
    return subprocess.run(
        args, check=check, text=True, capture_output=capture, timeout=180, input=input
    )


def systemctl(*args, check=True, capture=False):
    return command("systemctl", "--user", *args, check=check, capture=capture)


def verify_package(package):
    record = json.loads((package / "frame-package.json").read_bytes())
    if (
        record["schema"] != 1
        or record["channel"] not in ("stable", "beta")
        or not re.fullmatch(r"[0-9]{8}\.[0-9]+", record["steamos_build"])
    ):
        raise ValueError("Invalid Frame package")
    files = record["files"]
    actual = set()
    for file in package.rglob("*"):
        name = file.relative_to(package).as_posix()
        if file.is_symlink() or not (file.is_file() or file.is_dir()):
            raise ValueError(f"Unexpected package entry {name}")
        if file.is_file() and name != "frame-package.json":
            actual.add(name)
    if actual != set(files):
        raise ValueError("Files do not match the package manifest")
    for name, digest in files.items():
        path = PurePosixPath(name)
        if path.is_absolute() or ".." in path.parts:
            raise ValueError("Invalid package path")
        with (package / name).open("rb") as source:
            if hashlib.file_digest(source, "sha256").hexdigest() != digest:
                raise ValueError(f"Package checksum failed for {name}")
    return record["channel"], record["steamos_build"]


def calibration(source, package=None):
    choices = (
        BASE / "app-data/camera-calibration.json",
        BASE / "app/optical/camera-calibration.json",
    )
    if source is None:
        source = next((path for path in choices if path.is_file()), None)
    contents = source.read_bytes() if source is not None else None
    if contents is None and package:
        datasets = (
            path.parent
            for path in DATASETS.glob("*.dataset/sensorRigCalibration.json")
            if (path.parent / "snapshots/BeginningSnapshot/cameraMetadata.json").is_file()
            and (path.parent / f"{path.parent.stem}_device_config.json").is_file()
        )
        latest = max(datasets, default=None)
        if latest:
            contents = command(
                sys.executable,
                "-B",
                package / "app/optical/frame_camera_calibration.py",
                latest,
                capture=True,
            ).stdout.encode()
        elif (PERSIST / "xrservice.json").is_file() and (PERSIST / "device_config.json").is_file():
            contents = command(
                sys.executable,
                "-B",
                package / "app/optical/frame_camera_calibration.py",
                "--factory",
                PERSIST,
                capture=True,
            ).stdout.encode()
    if contents is None:
        raise ValueError("Supply this headset's camera calibration with --calibration")
    if not 0 < len(contents) <= 131072:
        raise ValueError("Invalid calibration size")
    value = json.loads(contents)
    cameras = value["rig"]["cameras"]
    if (
        value["version"] != 1
        or not isinstance(value["device"]["device_serial_number"], str)
        or not value["device"]["device_serial_number"]
        or not isinstance(cameras, list)
        or len(cameras) < 4
    ):
        raise ValueError("Invalid Frame camera calibration")
    for name in ("slam_left", "slam_right", "upper_left", "upper_right"):
        serial = value["metadata"]["cameras"][name]["serialNumber"]
        if not serial or not serial.isalnum() or not serial.isupper() and not serial.isdigit():
            raise ValueError(f"Invalid camera serial for {name}")
    return json.dumps(value, separators=(",", ":"), allow_nan=False) + "\n"


def start_service(unit):
    result = systemctl("start", unit, check=False)
    if result.returncode == 0:
        return
    deadline = time.monotonic() + 20
    while time.monotonic() < deadline:
        state = systemctl("show", unit, "-p", "ActiveState", "--value", capture=True).stdout.strip()
        if state == "active":
            return
        if state != "activating":
            break
        time.sleep(0.2)
    result.check_returncode()


def start_services():
    start_service("steamvr.service")
    systemctl("reset-failed", "steam.service", check=False)
    start_service("steam.service")


def stop_services(*, check=True):
    # Steam wants SteamVR and can queue a competing start during shutdown.
    systemctl(
        "--job-mode=replace-irreversibly",
        "stop",
        "steam.service",
        "steamvr.service",
        "gamescope-session.service",
        check=check,
    )


def ready():
    runtime = Path(os.environ.get("XDG_RUNTIME_DIR", f"/run/user/{os.getuid()}"))
    expected = {
        f"@controller-utils.{name}.v1.{os.getuid()}" for name in ("optical", "input", "beam")
    }
    camera_runtime = runpy.run_path(str(BASE / "app/optical/frame_camera_runtime.py"))
    if warning := camera_runtime["camera_device_warning"]():
        # XRService can abort before creating its socket when the kernel has no camera devices.
        print(warning, file=sys.stderr, flush=True)
        if DEBUG:
            DEBUG["record"]("Setup: " + warning)
    else:
        expected.add(str(runtime / "controller-utils-camera.sock"))
    deadline = time.monotonic() + 90
    listening = set()
    while time.monotonic() < deadline:
        entries = (
            line.split(maxsplit=7) for line in Path("/proc/net/unix").read_text().splitlines()
        )
        listening = {parts[7] for parts in entries if len(parts) == 8 and parts[3] == "00010000"}
        if expected <= listening:
            return
        time.sleep(0.2)
    if DEBUG:
        DEBUG["record"](
            "Setup timed out waiting for "
            + " ".join(
                sorted(
                    "camera" if name.startswith("/") else name.split(".")[1]
                    for name in expected - listening
                )
            )
        )
    raise RuntimeError("SteamVR did not start all Controller Utils connections")


def atomic_text(path, content, mode):
    path.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.NamedTemporaryFile(dir=path.parent, mode="w", delete=False) as out:
        temporary = Path(out.name)
        try:
            out.write(content)
            out.flush()
            os.fchmod(out.fileno(), mode)
            os.replace(temporary, path)
        finally:
            temporary.unlink(missing_ok=True)


def legacy_installation():
    if not any(item.exists() or item.is_symlink() for item in LEGACY):
        return False
    if not LEGACY_UPDATER.is_file() or LEGACY_UPDATER.resolve() != LEGACY_UPDATER:
        raise ValueError("The original Controller Utils uninstaller is missing.")
    info = LEGACY_UPDATER.stat()
    if info.st_uid != 0 or not stat.S_ISREG(info.st_mode) or info.st_mode & 0o022:
        raise ValueError("The original Controller Utils uninstaller is not root-owned.")
    return True


def retire_legacy(sources, receipt, migrate=False, missing=()):
    stop_services()
    try:
        if migrate:
            command("sudo", "-n", str(LEGACY_UPDATER), "--confirm")
            if not all(policy_present(path, source) for path, source in sources.items()):
                raise RuntimeError("Legacy migration did not install the Frame policies")
            created = {RULE.name, *(path.name for path in missing)}
        else:
            command("sudo", str(LEGACY_UPDATER), "--uninstall")
            created = set()
            if not policy_present(RULE, sources[RULE]):
                command("sudo", "install", "-m", "0644", sources[RULE], RULE)
                reload_rules()
                created.add(RULE.name)
        if created:
            owned = set(json.loads(receipt.read_text())) if receipt.is_file() else set()
            atomic_text(receipt, json.dumps(sorted(owned | created)), 0o600)
    finally:
        start_services()
    ready()


def validate_calibration(package, data, camera_config):
    command(
        sys.executable,
        "-B",
        package / "app/optical/frame_camera_runtime.py",
        "--validate-calibration",
        data,
        input=camera_config,
    )


def policy_sources(package):
    return {path: package / path.name for path in (RULE, KEEP)}


def policy_present(path, source):
    if path.is_symlink() or (path.exists() and path.read_bytes() != source.read_bytes()):
        raise ValueError(f"Existing policy differs from the package at {path}")
    return path.exists()


def reload_rules():
    command("sudo", "udevadm", "control", "--reload-rules")
    command("sudo", "udevadm", "trigger", "--subsystem-match=hidraw")


def remove_policies(paths):
    if paths:
        command("sudo", "rm", "-f", "--", *paths)
        if RULE in paths:
            reload_rules()


def service_files():
    data = BASE / "app-data"
    camera_env, input_env = data / "camera-hook.env", data / "input-hook.env"
    return (
        camera_env,
        input_env,
        {
            UNITS
            / "steamvr.service.d/90-controller-utils-camera.conf": f"[Service]\nEnvironmentFile=-{camera_env}\n",
            UNITS
            / "steam.service.d/90-controller-utils-input.conf": f"[Service]\nEnvironmentFile=-{input_env}\n",
        },
    )


@contextmanager
def setup_lock(data):
    with (data / "frame-install.lock").open("a") as lock:
        try:
            fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
        except BlockingIOError:
            raise RuntimeError(
                "Another Controller Utils setup is running. Wait for it to finish before trying again."
            ) from None
        yield


def install(package, camera_config, unattended=False, migrate=False):
    data, app = BASE / "app-data", BASE / "app"
    data.mkdir(parents=True, exist_ok=True, mode=0o700)
    with setup_lock(data):
        validate_calibration(package, data, camera_config)
        sources = policy_sources(package)
        legacy = legacy_installation()
        missing = [path for path, source in sources.items() if not policy_present(path, source)]
        if migrate and (
            not legacy
            or command(
                "sudo", "-n", str(LEGACY_UPDATER), "--repair-status", capture=True
            ).stdout.strip()
            != "controller-utils-frame-migration-v1"
        ):
            raise ValueError("The signed legacy migration authorization is unavailable.")
        if unattended and (legacy or missing):
            raise ValueError("This upgrade needs administrator authorization. Run install.py.")
        if not migrate and (legacy or missing):
            command("sudo", "-v")
        driver = BASE / "native-driver"
        if (driver.exists() or driver.is_symlink()) and (
            not driver.is_symlink() or os.readlink(driver) != "app/native-driver"
        ):
            raise ValueError("Existing driver path belongs to another installation")
        if app.is_symlink():
            raise ValueError("Existing app directory is a symlink")
        backup = Path(tempfile.mkdtemp(prefix="previous-frame-", dir=BASE))
        stage = backup / "replacement"
        shutil.copytree(package / "app", stage)
        launcher = BASE / "controller-utils"
        config = data / "camera-calibration.json"
        camera_env, input_env, drop_ins = service_files()
        receipt = data / "frame-policies.json"
        owned = set(json.loads(receipt.read_text())) if receipt.is_file() else set()
        managed = {launcher, config, camera_env, input_env, DESKTOP, receipt, *drop_ins}
        old = {}
        for item in managed:
            if item.is_symlink() or (item.exists() and not item.is_file()):
                raise ValueError(f"Unsafe setup path {item}")
            if item.is_file():
                saved = backup / item.name
                shutil.copy2(item, saved)
                old[item] = saved
        updater = LEGACY_UNIT
        legacy_enabled = systemctl("is-enabled", "--quiet", updater, check=False).returncode == 0
        moved = installed = driver_created = False
        created_policies = []
        try:
            if legacy_enabled:
                systemctl("disable", updater)
            stop_services()
            if app.exists():
                app.rename(backup / "app")
                moved = True
            stage.rename(app)
            installed = True
            shutil.copy2(package / "controller-utils", backup / "launcher-next")
            os.replace(backup / "launcher-next", launcher)
            atomic_text(config, camera_config, 0o600)
            optical = app / "optical"
            atomic_text(
                camera_env,
                f"LD_PRELOAD={optical}/frame_camera_hook.so:{optical}/frame_input_hook.so\n"
                f"CONTROLLER_UTILS_CAMERA_CALIBRATION={config}\n",
                0o600,
            )
            atomic_text(input_env, f"LD_PRELOAD={optical}/frame_input_hook.so\n", 0o600)
            for path, contents in drop_ins.items():
                atomic_text(path, contents, 0o644)
            if not driver.is_symlink():
                driver.symlink_to("app/native-driver", target_is_directory=True)
                driver_created = True
            command(VR / "vrpathreg", "adddriver", driver)
            command(app / "controller-utils", "--prepare-registration")
            systemctl("daemon-reload")
            start_services()
            ready()
            for path in () if migrate else missing:
                command("sudo", "install", "-m", "0644", sources[path], path)
                created_policies.append(path)
            if RULE in created_policies:
                reload_rules()
            if created_policies:
                atomic_text(
                    receipt,
                    json.dumps(sorted(owned | {path.name for path in created_policies})),
                    0o600,
                )
            if not moved:
                command(app / "controller-utils", "--register")
        except BaseException:
            stop_services(check=False)
            if installed:
                app.rename(backup / "failed")
            if moved:
                (backup / "app").rename(app)
            if driver_created:
                command(VR / "vrpathreg", "removedriver", driver, check=False)
                driver.unlink()
            for item in managed:
                if item in old:
                    os.replace(old[item], item)
                else:
                    item.unlink(missing_ok=True)
            if legacy_enabled:
                systemctl("enable", updater, check=False)
            systemctl("daemon-reload")
            try:
                remove_policies(created_policies)
            finally:
                start_services()
            raise
        if legacy:
            retire_legacy(sources, receipt, migrate, missing)
        old_unit = UNITS / LEGACY_UNIT
        if (
            not old_unit.is_symlink()
            and old_unit.is_file()
            and old_unit.read_text() == LEGACY_UNIT_TEXT
        ):
            old_unit.unlink()
            systemctl("daemon-reload")
    print(f"Controller Utils installed. Previous files are in {backup}.")


def uninstall(package):
    data, app = BASE / "app-data", BASE / "app"
    driver, launcher = BASE / "native-driver", BASE / "controller-utils"
    camera_env, input_env, drop_ins = service_files()
    with setup_lock(data):
        if (
            not app.is_dir()
            or app.is_symlink()
            or not driver.is_symlink()
            or os.readlink(driver) != "app/native-driver"
            or launcher.is_symlink()
            or launcher.read_bytes() != (package / launcher.name).read_bytes()
        ):
            raise ValueError("Use the package matching the installed Controller Utils build")
        receipt = data / "frame-policies.json"
        owned = set(json.loads(receipt.read_text())) if receipt.is_file() else set()
        sources = policy_sources(package)
        paths = [
            path
            for path, source in sources.items()
            if path.name in owned and policy_present(path, source)
        ]
        for drop, contents in drop_ins.items():
            if drop.is_symlink() or (drop.exists() and drop.read_text() != contents):
                raise ValueError(f"Existing Steam service configuration differs at {drop}")
        if paths:
            command("sudo", "-v")
        backup = Path(tempfile.mkdtemp(prefix="removed-frame-", dir=BASE))
        files = (app, launcher, camera_env, input_env, receipt, *drop_ins)
        driver_removed = False
        command(app / "controller-utils", "--unregister")
        try:
            stop_services()
            remove_policies(paths)
            command(VR / "vrpathreg", "removedriver", driver)
            driver_removed = True
            driver.unlink()
            for path in files:
                if path.exists():
                    path.rename(backup / path.name)
            systemctl("daemon-reload")
            start_services()
        except BaseException:
            stop_services(check=False)
            for path in files:
                saved = backup / path.name
                if saved.exists():
                    saved.rename(path)
            try:
                if driver_removed:
                    if not driver.is_symlink():
                        driver.symlink_to("app/native-driver", target_is_directory=True)
                    command(VR / "vrpathreg", "adddriver", driver)
                for path in paths:
                    if not policy_present(path, sources[path]):
                        command("sudo", "install", "-m", "0644", sources[path], path)
                if RULE in paths:
                    reload_rules()
            finally:
                systemctl("daemon-reload")
                start_services()
            command(app / "controller-utils", "--register")
            raise
    print(f"Controller Utils was removed. Settings and calibration remain in {data}.")


def main():
    global DEBUG
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--calibration", type=Path)
    action = parser.add_mutually_exclusive_group()
    action.add_argument("--check", action="store_true", help="Check the Frame without installing")
    action.add_argument("--uninstall", action="store_true", help="Remove Controller Utils")
    action.add_argument("--unattended", action="store_true", help=argparse.SUPPRESS)
    action.add_argument("--legacy-migration", action="store_true", help=argparse.SUPPRESS)
    args = parser.parse_args()
    if os.geteuid() == 0:
        raise ValueError("Run this installer as the Steam Frame user")
    package = Path(__file__).resolve().parent
    channel, build_id = verify_package(package)
    if (BASE / "app-data/debug-capture.json").is_file():
        DEBUG = runpy.run_path(str(package / "app/optical/frame_debug.py"))
        mode = "check" if args.check else "uninstall" if args.uninstall else "install"
        DEBUG["record"](f"Setup {mode} started package_channel={channel} package_build={build_id}")
    branch = command("atomupd-manager", "tracked-branch", capture=True).stdout.strip()
    variant = command("atomupd-manager", "tracked-variant", capture=True).stdout.strip()
    if (
        platform.machine() != "aarch64"
        or variant != "vr"
        or (branch != channel and not args.uninstall)
    ):
        raise ValueError(f"This package requires Steam Frame's {channel} channel")
    if args.uninstall:
        uninstall(package)
        if DEBUG:
            DEBUG["record"]("Uninstall finished")
        return
    installed = {
        key: value.strip('"')
        for key, _, value in (
            line.partition("=") for line in Path("/etc/os-release").read_text().splitlines()
        )
    }
    if installed.get("BUILD_ID") != build_id:
        print(
            f"Package tested on SteamOS {build_id}. Running {installed.get('BUILD_ID', 'unknown')}."
        )
    camera_config = calibration(args.calibration, package)
    if DEBUG:
        DEBUG["record"]("Setup loaded camera calibration")
    command(package / "frame-compatibility")
    subprocess.run(
        [package / "app/optical/frame_palm_inference", PALM],
        stdin=subprocess.DEVNULL,
        check=True,
        timeout=30,
    )
    if DEBUG:
        DEBUG["record"]("Setup compatibility check and CPU palm model passed")
    if args.check:
        print(f"Frame {channel} package preflight passed")
    else:
        install(package, camera_config, args.unattended, args.legacy_migration)
    if DEBUG:
        DEBUG["record"]("Setup finished")


if __name__ == "__main__":
    signal.signal(signal.SIGHUP, signal.SIG_IGN)
    try:
        main()
    except (OSError, ValueError, RuntimeError, KeyError, subprocess.SubprocessError) as error:
        if DEBUG:
            DEBUG["failure"]("Setup", error)
        sys.exit(str(error))
