#!/usr/bin/env python3
"""Stage a standalone shared-camera package for one SteamOS channel."""

import argparse
import hashlib
import json
import os
import re
import stat
import subprocess
import tarfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
MAX_CONTENTS = 256 * 1024 * 1024
ASSETS = (
    "controller.stl",
    "controller.surface",
    "controller.surface.json",
    "controller.buttons",
    "controller.buttons.json",
    "controller-icon.png",
    "controller-icon.svg",
    "controller-icon-license.txt",
    "controller.json",
    "README.md",
    "license.txt",
)

DRIVER_FILES = (
    "driver.vrdrivermanifest",
    "bin/linuxarm64/driver_controllerutils_native.so",
    "resources/input/sc26_profile.json",
    "resources/input/vrcompositor_bindings_sc26.json",
    "resources/rendermodels/sc26_input/sc26_input.obj",
)

OPTICAL_FILES = (
    "frame_camera_hook.so",
    "frame_input_hook.so",
    "frame_palm_inference",
    "frame_camera_runtime.py",
    "frame_camera_worker.py",
    "frame_debug.py",
    "frame_raw_camera.py",
    "frame_led_tracking.py",
    "frame_palm_bootstrap.py",
    "frame_camera_calibration.py",
)
STEAMOS_BUILDS = {"stable": "20261022.6150929", "beta": "20261125.6090751"}


def read_source(path, limit=MAX_CONTENTS):
    parts = path.relative_to(ROOT).parts
    if not parts or any(part in (".", "..") for part in parts):
        raise ValueError("Invalid release source path")
    directory = os.open(ROOT, os.O_RDONLY | os.O_DIRECTORY | os.O_NOFOLLOW | os.O_CLOEXEC)
    try:
        for part in parts[:-1]:
            child = os.open(
                part, os.O_RDONLY | os.O_DIRECTORY | os.O_NOFOLLOW | os.O_CLOEXEC, dir_fd=directory
            )
            os.close(directory)
            directory = child
        descriptor = os.open(
            parts[-1], os.O_RDONLY | os.O_NOFOLLOW | os.O_NONBLOCK | os.O_CLOEXEC, dir_fd=directory
        )
        with os.fdopen(descriptor, "rb") as source:
            before = os.fstat(source.fileno())
            if not stat.S_ISREG(before.st_mode) or not 0 <= before.st_size <= limit:
                raise ValueError("Invalid or oversized release source")
            contents = source.read(limit + 1)
            after = os.fstat(source.fileno())
            if (
                len(contents) != before.st_size
                or before.st_size != after.st_size
                or before.st_mtime_ns != after.st_mtime_ns
                or before.st_ctime_ns != after.st_ctime_ns
            ):
                raise ValueError("Release source changed while staging")
            return contents
    finally:
        os.close(directory)


def check_stable_abi(binary):
    output = subprocess.check_output(["readelf", "--version-info", binary], text=True)
    versions = {
        tuple(map(int, match.split("."))) for match in re.findall(r"Name: GLIBC_([0-9.]+)", output)
    }
    if any(version > (2, 39) for version in versions):
        raise ValueError(f"{binary.name} uses a libc newer than SteamOS stable")


def stage(destination, channel, build_id=None):
    if channel not in STEAMOS_BUILDS:
        raise ValueError("Select stable or beta")
    build_id = build_id or STEAMOS_BUILDS[channel]
    if not re.fullmatch(r"[0-9]{8}\.[0-9]+", build_id):
        raise ValueError("Invalid SteamOS build ID")
    files = {
        "app/controller-utils": "build/controller-utils",
        "app/release.json": "release.json",
        "app/frame_update.py": "packaging/bootstrap.py",
        "app/assets/update-public.pem": "assets/update-public.pem",
        "app/LICENSE": "LICENSE",
        "app/third_party/openvr/LICENSE": "third_party/openvr/LICENSE",
        **{"app/assets/" + name: "assets/" + name for name in ASSETS},
        **{"app/native-driver/" + name: "build/native-driver/" + name for name in DRIVER_FILES},
        **{"app/optical/" + name: "build/optical/" + name for name in OPTICAL_FILES},
        "controller-utils": "build/frame-launcher",
        "frame-compatibility": "build/frame-compatibility",
        "install.py": "packaging/frame_install.py",
        "70-controller-utils.rules": "packaging/70-controller-utils.rules",
        "controller-utils-frame.conf": "packaging/controller-utils-frame.conf",
    }
    executable = {
        "app/controller-utils",
        "app/optical/frame_palm_inference",
        "controller-utils",
        "frame-compatibility",
    }
    binaries = executable | {name for name in files if name.endswith(".so")}
    payload = {}
    for name, source in files.items():
        data = read_source(ROOT / source)
        if name in binaries and (data[:6] != b"\x7fELF\x02\x01" or data[18:20] != b"\xb7\x00"):
            raise ValueError(f"{source} is not Linux ARM64")
        payload[name] = data
    destination.mkdir(parents=True, exist_ok=False)
    for name, data in payload.items():
        target = destination / name
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_bytes(data)
        target.chmod(0o755 if name in executable else 0o644)
        if channel == "stable" and name in binaries:
            check_stable_abi(target)
    manifest = {
        "schema": 1,
        "channel": channel,
        "steamos_build": build_id,
        "files": {name: hashlib.sha256(data).hexdigest() for name, data in payload.items()},
    }
    (destination / "frame-package.json").write_text(json.dumps(manifest, indent=2) + "\n")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--channel", choices=("stable", "beta"), required=True)
    parser.add_argument("--build-id", help="Override the SteamOS build for the selected channel")
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--archive", type=Path)
    args = parser.parse_args()
    stage(args.output, args.channel, args.build_id)
    if args.archive:
        with tarfile.open(args.archive, "x:gz") as archive:
            archive.add(args.output, arcname=args.output.name)
