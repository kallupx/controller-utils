#!/usr/bin/env python3
"""Build the pinned ARM64 adapter and its loader, never access a live process."""

import argparse
import hashlib
import json
import re
import struct
import subprocess
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
pins = re.search(
    r"XrServiceHashes\s*\{(.*?)\};",
    (ROOT / "src/steamvr/compatibility.h").read_text(),
    re.S,
)
if not pins:
    raise ValueError("Missing pinned XRService fingerprints")
XR_HASHES = set(re.findall(r'"([a-f0-9]{64})"', pins.group(1)))
CAVE, CONTROL = 0x315ABB0, 0x343FF60
CAVE_LIMIT = 0x315B000
LIBRARY = "/var/lib/controller-utils/native-tracker/libnative_tracker.so"
SITES = (
    0xEAFC50,
    0xEB6F30,
    0xEB17A0,
    0xEB0B00,
    0xEB8250,
    0xEB7FE0,
    0xEB3880,
    0x2637680,
    0xEB4280,
    0xEC2270,
    0x25E6A70,
)
MODE_SITE = 0xED358C
CONFIG_SITE = 0xF2D888
PATCH_SITES = (*SITES, MODE_SITE, CONFIG_SITE)


def branch(source, destination):
    delta = destination - source
    if delta % 4 or not -(1 << 27) <= delta < 1 << 27:
        raise ValueError("unaligned or out-of-range AArch64 branch")
    return struct.pack("<I", 0x14000000 | ((delta // 4) & 0x3FFFFFF))


def source(words):
    lines = [".section .text", ".global ct_cave", "ct_cave:"]
    # Every stub tail-calls: native unwind data still describes the original frame.
    for index in range(len(SITES)):
        lines += [
            f".global route_{index}",
            f"route_{index}:",
            "ldr x16, =control",
            f"add x16, x16, #{32 + index * 8 if index < 6 else 24}",
            "ldar x16, [x16]",
        ]
        if index == 0:
            lines += ["cbz x16, loader", "br x16"]
        else:
            lines += [f"cbz x16, trampoline_{index}"]
            if index >= 6:
                lines += [f"ldr x16, [x16, #{(index - 6) * 8}]"]
            lines += ["br x16"]
    lines += [
        "loader:",
        "ldr x17, =control",
        "ldr w16, [x17, #12]",
        "cmp w16, #1",
        "b.ne trampoline_0",
        "add x17, x17, #152",
        "1:",
        "ldaxr w16, [x17]",
        "cbnz w16, 2f",
        "mov w16, #1",
        "stlxr w9, w16, [x17]",
        "cbnz w9, 1b",
        "sub sp, sp, #208",
        "stp x0, x1, [sp]",
        "stp x2, x3, [sp, #16]",
        "stp x4, x5, [sp, #32]",
        "stp x6, x7, [sp, #48]",
        "stp x8, x30, [sp, #64]",
        "stp q0, q1, [sp, #80]",
        "stp q2, q3, [sp, #112]",
        "stp q4, q5, [sp, #144]",
        "stp q6, q7, [sp, #176]",
        "adr x0, library_path",
        "mov w1, #2",
        "ldr x16, =0xc48440",
        "blr x16",
        "cbz x0, failed",
        "adr x1, attach_symbol",
        "ldr x16, =0xc48360",
        "blr x16",
        "cbz x0, failed",
        "mov x16, x0",
        "ldr x0, =control",
        "ldr x1, [sp]",
        "adr x2, trampolines",
        "blr x16",
        "b loaded",
        "failed:",
        "ldr x17, =control",
        "mov w16, #16",
        "str w16, [x17, #116]",
        "mov w16, #5",
        "str w16, [x17, #112]",
        "loaded:",
        "ldp q0, q1, [sp, #80]",
        "ldp q2, q3, [sp, #112]",
        "ldp q4, q5, [sp, #144]",
        "ldp q6, q7, [sp, #176]",
        "ldp x0, x1, [sp]",
        "ldp x2, x3, [sp, #16]",
        "ldp x4, x5, [sp, #32]",
        "ldp x6, x7, [sp, #48]",
        "ldp x8, x30, [sp, #64]",
        "add sp, sp, #208",
        "ldr x16, =control",
        "add x16, x16, #32",
        "ldar x16, [x16]",
        "cbz x16, trampoline_0",
        "br x16",
        "2:",
        "clrex",
        "b trampoline_0",
    ]
    for index, word in enumerate(words):
        lines += [
            f".global trampoline_{index}",
            f"trampoline_{index}:",
            f".inst {word:#x}",
            f"b resume_{index}",
        ]
    lines += [".balign 8", "trampolines:"]
    lines += [f".quad trampoline_{i}" for i in range(len(SITES))]
    # ed31c0 refreshes settings on every frame. Keep its calibration updates,
    # overriding only the requested alternate-mode bit for our owned manager.
    lines += [
        ".global route_mode",
        "route_mode:",
        "ldr x16, =control",
        "add x16, x16, #88",
        "ldar x16, [x16]",
        "cbz x16, 1f",
        "cmp x19, x16",
        "b.ne 1f",
        "mov w20, wzr",
        "1: and w9, w20, #1",
        "b resume_mode",
    ]
    # A restarted service can cache ID16 before our private manager exists.
    # Re-emit its config on reconnect without appending another cache entry.
    lines += [
        ".global route_config",
        "route_config:",
        "b.eq 1f",
        "b resume_config",
        "1: cmp w28, #16",
        "b.ne 2f",
        "ldr w9, [sp, #0x3f4]",
        "cmp w9, #1",
        "b.ne 2f",
        "cbnz w25, 2f",
        "add x20, sp, #0xb0",
        "add x26, sp, #0x98",
        "b publish_config",
        "2: b duplicate_config",
    ]
    lines += [
        "library_path:",
        f'.asciz "{LIBRARY}"',
        "attach_symbol:",
        '.asciz "ct_tracker_attach"',
        ".balign 8",
        ".ltorg",
    ]
    return "\n".join(lines) + "\n"


def pinned_bytes(binary, address, size):
    # This range is the pinned RX segment, including its private tail-page alias.
    offset = 0xA35C2C + address - 0xC45C2C
    return binary[offset : offset + size]


def build(output, xrservice=None):
    layout = json.loads((ROOT / "driver/native_layout.json").read_text())
    runtimes = layout["runtimes"]
    if (
        set(runtimes) != {"stable", "beta"}
        or {entry["xr_sha256"] for entry in runtimes.values()} != XR_HASHES
        or layout["cave"] != CAVE
    ):
        raise ValueError("Native layout does not match the runtime pin")
    caves = {}
    for entry in runtimes.values():
        fingerprint, cave = entry["xr_sha256"], entry["cave_original"]
        if fingerprint in caves and caves[fingerprint] != cave:
            raise ValueError("Runtime profiles disagree about the same executable")
        caves[fingerprint] = cave
    binary = Path(xrservice).read_bytes() if xrservice else None
    fingerprint = (
        hashlib.sha256(binary).hexdigest() if binary is not None else runtimes["beta"]["xr_sha256"]
    )
    if fingerprint not in XR_HASHES:
        raise ValueError("saved XRService hash mismatch")

    def original(address, size):
        data = bytes.fromhex(
            caves[fingerprint] if address == CAVE else layout["sites"][str(address)]
        )
        if len(data) < size:
            raise ValueError("Audited native layout is too short")
        data = data[:size]
        if binary is not None and pinned_bytes(binary, address, size) != data:
            raise ValueError("Audited native layout differs from XRService")
        return data

    output.mkdir(parents=True, exist_ok=True)
    originals = [original(site, 16) for site in SITES]
    words = [struct.unpack_from("<I", data)[0] for data in originals]
    mode_original = original(MODE_SITE, 16)
    if mode_original[:4] != bytes.fromhex("89020012"):
        raise ValueError("native mode selector instruction differs")
    # Only stack saves are copied into the trampoline. Never relocate PC-relative code.
    expected = (
        0xFC190FE8,
        0x6DB73BEF,
        0xD10383FF,
        0xD106C3FF,
        0xA9BC7BFD,
        0xFC1C0FE8,
        0xD10583FF,
        0xFC160FEE,
        0xD10683FF,
        0xD102C3FF,
        0xD10143FF,
    )
    if tuple(words) != expected:
        raise ValueError("native entry instruction differs: " + str([hex(x) for x in words]))
    assembly, script = output / "loader.S", output / "loader.ld"
    assembly.write_text(source(words))
    script.write_text(
        "SECTIONS { .text "
        + hex(CAVE)
        + " : { *(.text) } }\n"
        + "control = "
        + hex(CONTROL)
        + ";\n"
        + "".join(f"resume_{i} = {site + 4:#x};\n" for i, site in enumerate(SITES))
        + f"resume_mode = {MODE_SITE + 4:#x};\n"
        + f"resume_config = {CONFIG_SITE + 4:#x};\n"
        + "duplicate_config = 0xf2dd70;\npublish_config = 0xf2d974;\n"
    )
    subprocess.run(
        [
            "clang",
            "--target=aarch64-linux-gnu",
            "-c",
            str(assembly),
            "-o",
            str(output / "loader.o"),
        ],
        check=True,
    )
    subprocess.run(
        [
            "ld.lld",
            "-e",
            "ct_cave",
            "-T",
            str(script),
            str(output / "loader.o"),
            "-o",
            str(output / "loader.elf"),
        ],
        check=True,
    )
    subprocess.run(
        [
            "llvm-objcopy",
            "-O",
            "binary",
            "--only-section=.text",
            str(output / "loader.elf"),
            str(output / "loader.bin"),
        ],
        check=True,
    )
    symbols = subprocess.check_output(
        ["llvm-nm", "--defined-only", str(output / "loader.elf")], text=True
    )
    addresses = {line.split()[2]: int(line.split()[0], 16) for line in symbols.splitlines()}
    loader = (output / "loader.bin").read_bytes()
    if len(loader) > CAVE_LIMIT - CAVE:
        raise ValueError("loader exceeds pinned RX padding")
    library = output / "libnative_tracker.so"
    subprocess.run(
        [
            "clang++",
            "--target=aarch64-linux-gnu",
            "-std=c++17",
            "-O2",
            "-Wall",
            "-Wextra",
            "-Werror",
            "-fPIC",
            "-shared",
            "-nostdlib",
            "-fuse-ld=lld",
            "-fvisibility=hidden",
            "-fno-stack-protector",
            "-mno-outline-atomics",
            "-Wl,--eh-frame-hdr,-z,relro,-z,now,-soname,libnative_tracker.so",
            str(ROOT / "driver/native_tracker.cpp"),
            "-o",
            str(library),
        ],
        check=True,
    )
    manifest = {
        "version": 2,
        "control": CONTROL,
        "cave": CAVE,
        "library_path": LIBRARY,
        "loader_sha256": hashlib.sha256(loader).hexdigest(),
        "library_sha256": hashlib.sha256(library.read_bytes()).hexdigest(),
        "runtime_originals": {
            key: bytes.fromhex(cave)[: len(loader)].hex() for key, cave in caves.items()
        },
        "sites": [
            {
                "address": site,
                "original": original.hex(),
                "redirect": branch(site, addresses[f"route_{i}"]).hex(),
                "trampoline": addresses[f"trampoline_{i}"],
            }
            for i, (site, original) in enumerate(zip(SITES, originals))
        ],
    }
    original(CAVE, len(loader))
    if any(
        len(bytes.fromhex(cave)) != len(loader) for cave in manifest["runtime_originals"].values()
    ):
        raise ValueError("Audited native loader padding is too short")
    manifest["sites"].append(
        {
            "address": MODE_SITE,
            "original": mode_original.hex(),
            "redirect": branch(MODE_SITE, addresses["route_mode"]).hex(),
        }
    )
    config_original = original(CONFIG_SITE, 16)
    if struct.unpack_from("<I", config_original)[0] != 0x54002740:
        raise ValueError("native configuration cache branch differs")
    manifest["sites"].append(
        {
            "address": CONFIG_SITE,
            "original": config_original.hex(),
            "redirect": branch(CONFIG_SITE, addresses["route_config"]).hex(),
        }
    )
    (output / "adapter.json").write_text(json.dumps(manifest, indent=2) + "\n")
    (output / "manifest.json").write_text(
        json.dumps(
            {
                name: hashlib.sha256((output / name).read_bytes()).hexdigest()
                for name in ("adapter.json", "loader.bin", "libnative_tracker.so")
            },
            indent=2,
        )
        + "\n"
    )
    return {
        "loader_bytes": len(loader),
        "library_bytes": library.stat().st_size,
        "directory": str(output),
    }


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=Path, default=ROOT / "build/native-tracker")
    parser.add_argument("--xrservice", type=Path)
    args = parser.parse_args()
    if args.xrservice and not args.xrservice.is_file():
        parser.error("Supply a local copy of the pinned XRService executable")
    print(json.dumps(build(args.output, args.xrservice)))
