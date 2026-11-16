#!/usr/bin/env python3
"""Build and sign a native ARM64 update feed, then check it with the native updater."""

import argparse
import hashlib
import io
import json
import os
import re
import stat
import subprocess
import tarfile
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
GITHUB = "https://github.com/kallupx/controller-utils/releases/latest/download/"
MAX_ARCHIVE = 128 * 1024 * 1024
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


PACKAGE_FILES = (
    "docs/controller-utils.png",
    "controller-utils-launcher",
    "install.sh",
    "uninstall.sh",
    "README.md",
    "packaging/controller-utils-update.service",
    "packaging/controller-utils-updater.sudoers",
    "packaging/70-controller-utils.rules",
)

DRIVER_FILES = (
    "driver.vrdrivermanifest",
    "bin/linuxarm64/driver_controllerutils_native.so",
    "resources/input/sc26_profile.json",
    "resources/input/vrcompositor_bindings_sc26.json",
    "resources/rendermodels/sc26_input/sc26_input.obj",
)
HELPER_FILES = (
    "controller-utils-helper",
    "controller-utils-updater",
    "native-tracker/adapter.json",
    "native-tracker/loader.bin",
    "native-tracker/libnative_tracker.so",
    "native-tracker/manifest.json",
)


def read(path, limit=MAX_CONTENTS):
    info = path.lstat()
    if not stat.S_ISREG(info.st_mode) or info.st_size > limit:
        raise ValueError("Expected a bounded regular release file: " + str(path))
    data = path.read_bytes()
    if len(data) > limit:
        raise ValueError("Release file exceeded its size limit: " + str(path))
    return data


def sign(document, destination, key):
    subprocess.run(
        [
            "/usr/bin/openssl",
            "pkeyutl",
            "-sign",
            "-rawin",
            "-inkey",
            str(key),
            "-passin",
            "pass:",
            "-in",
            str(document),
            "-out",
            str(destination),
        ],
        check=True,
        timeout=5,
    )


def public_key(key):
    info = key.lstat()
    if (
        not stat.S_ISREG(info.st_mode)
        or info.st_mode & 0o077
        or info.st_uid != os.getuid()
        or info.st_nlink != 1
        or info.st_size > 16384
    ):
        raise ValueError("Keep the signing key in a private regular file owned by you")
    return subprocess.check_output(
        ["/usr/bin/openssl", "pkey", "-in", str(key), "-passin", "pass:", "-pubout"], timeout=5
    )


def build(root, requirements, key, output, notes):
    if len(notes) > 240:
        raise ValueError("Release notes must fit 240 characters")
    if (
        set(requirements) != {"backend", "driver", "helper", "runtime"}
        or requirements["backend"] != "native-tracker"
        or set(requirements["driver"]) != set(DRIVER_FILES)
        or set(requirements["helper"]) != set(HELPER_FILES)
        or set(requirements["runtime"]) != {"XRService"}
    ):
        raise ValueError("Invalid release component list")
    public = public_key(key)
    checker = ROOT / "build/release_check"
    if not checker.is_file():
        raise ValueError("Build the native release checker with make build/release_check")
    version_bytes = read(root / "release.json", 65536)
    version = json.loads(version_bytes)
    if (
        not isinstance(version, dict)
        or set(version) != {"version", "sequence", "platform"}
        or not isinstance(version["version"], str)
        or not re.fullmatch(r"[0-9]{1,6}\.[0-9]{1,6}\.[0-9]{1,6}", version["version"])
        or type(version["sequence"]) is not int
        or not 1 <= version["sequence"] <= 2**63 - 1
        or version["platform"] != "linuxarm64"
    ):
        raise ValueError("Invalid installed release information")
    payload = {"assets/" + name: read(root / "assets" / name) for name in ASSETS}
    payload.update(
        {
            "controller-utils": read(root / "controller-utils"),
            "release.json": version_bytes,
            "LICENSE": read(root / "LICENSE", 65536),
            "third_party/openvr/LICENSE": read(root / "third_party/openvr/LICENSE", 65536),
            "assets/update-channel.json": (json.dumps({"url": GITHUB}) + "\n").encode(),
            "assets/update-public.pem": public,
        }
    )
    payload.update({name: read(root / name) for name in PACKAGE_FILES})
    for field, directory in (("driver", "native-driver"), ("helper", "compatibility")):
        for name, expected in requirements[field].items():
            data = read(root / directory / name)
            if hashlib.sha256(data).hexdigest() != expected:
                raise ValueError("Native release payload does not match its fingerprint: " + name)
            payload[directory + "/" + name] = data
    if public != read(root / "assets/update-public.pem", 16384):
        raise ValueError("Signing key does not match the installed update trust key")
    if sum(map(len, payload.values())) > MAX_CONTENTS:
        raise ValueError("Release contents exceed the update size limit")
    output.mkdir(parents=True, exist_ok=True)
    artifact = (
        "controller-utils-"
        + version["version"]
        + "-"
        + str(version["sequence"])
        + "-linuxarm64.tar.gz"
    )
    names = (artifact, "latest.json", "latest.sig", "latest-stable.json", "latest-stable.sig")
    if any(os.path.lexists(output / name) for name in names):
        raise ValueError("Use a new output directory, never overwrite a published release")
    with tempfile.TemporaryDirectory(prefix="release-", dir=ROOT / "build") as temporary:
        directory = Path(temporary)
        path = directory / artifact
        with tarfile.open(path, "x:gz") as archive:
            for name, data in sorted(payload.items()):
                member = tarfile.TarInfo(name)
                member.size = len(data)
                member.mode = (
                    0o755
                    if name
                    in {
                        "controller-utils",
                        "controller-utils-launcher",
                        "install.sh",
                        "uninstall.sh",
                        "compatibility/controller-utils-helper",
                        "compatibility/controller-utils-updater",
                    }
                    else 0o644
                )
                archive.addfile(member, io.BytesIO(data))
        if path.stat().st_size > MAX_ARCHIVE:
            raise ValueError("Release archive exceeds the update size limit")
        metadata = {
            "schema": 5,
            **version,
            "expires": 0,
            "artifact": artifact,
            "size": path.stat().st_size,
            "sha256": hashlib.sha256(path.read_bytes()).hexdigest(),
            "requirements": requirements,
            "notes": notes,
        }
        manifest, signature = directory / "latest.json", directory / "latest.sig"
        manifest.write_text(json.dumps(metadata, sort_keys=True, indent=2) + "\n")
        sign(manifest, signature, key)
        runtimes = json.loads(read(ROOT / "driver/native_layout.json", 65536))["runtimes"]
        stable_requirements = {
            **requirements,
            "runtime": {"XRService": runtimes["stable"]["xr_sha256"]},
        }
        stable_metadata = {**metadata, "requirements": stable_requirements}
        stable_manifest, stable_signature = (
            directory / "latest-stable.json",
            directory / "latest-stable.sig",
        )
        stable_manifest.write_text(json.dumps(stable_metadata, sort_keys=True, indent=2) + "\n")
        sign(stable_manifest, stable_signature, key)
        trust = directory / "public.pem"
        trust.write_bytes(public)
        subprocess.run(
            [str(checker), str(directory), str(trust), str(directory / "verified")],
            check=True,
            timeout=30,
            stdout=subprocess.PIPE,
        )
        subprocess.run(
            [str(checker), "--metadata", str(stable_manifest), str(stable_signature), str(trust)],
            check=True,
            timeout=5,
            stdout=subprocess.PIPE,
        )
        created = []
        try:
            for name in names:
                with (output / name).open("xb") as destination:
                    created.append(output / name)
                    destination.write((directory / name).read_bytes())
                    destination.flush()
                    os.fsync(destination.fileno())
        except BaseException:
            for path in reversed(created):
                path.unlink()
            raise
    return {
        "version": version["version"],
        "sequence": version["sequence"],
        "archive": str(output / artifact),
        "bytes": metadata["size"],
        "files": len(payload),
        "feed": GITHUB,
        "contains_headset_calibration": False,
    }


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--app-stage", type=Path, required=True)
    parser.add_argument("--requirements", type=Path, required=True)
    parser.add_argument("--key", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--notes", default="Controller Utils update")
    args = parser.parse_args()
    print(
        json.dumps(
            build(
                args.app_stage,
                json.loads(args.requirements.read_text()),
                args.key,
                args.output,
                args.notes,
            ),
            indent=2,
        )
    )


if __name__ == "__main__":
    main()
