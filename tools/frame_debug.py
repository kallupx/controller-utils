"""Opt-in support events and plain-text exports for Steam Frame."""

import errno
import json
import os
import platform
import re
import socket
import subprocess
import sys
import tempfile
import traceback
from datetime import UTC, datetime
from pathlib import Path

DATA = Path.home() / ".local/share/controller-utils/app-data"
LIMIT = 1500


def capture(data=DATA):
    try:
        with (data / "debug-capture.json").open("rb") as marker:
            value = json.loads(marker.read(257))
        token = value["id"]
        recording, stopped = value["recording"], value["stopped"]
        if (
            isinstance(token, str)
            and re.fullmatch(r"[0-9]{1,17}", token)
            and type(recording) is bool
            and type(stopped) is int
            and 0 <= stopped < 10**17
        ):
            return token, recording, stopped
    except (OSError, ValueError, KeyError, TypeError):
        pass
    return "", False, 0


def send(token, message):
    # Use the same journal fields as the native app, including from an interactive terminal.
    if not token:
        return
    line = " ".join(message.split())[:600]
    packet = f"SYSLOG_IDENTIFIER=controller-utils-debug\nCU_CAPTURE_ID={token}\nPRIORITY=6\nMESSAGE={line}\n"
    try:
        with socket.socket(socket.AF_UNIX, socket.SOCK_DGRAM) as peer:
            peer.setblocking(False)
            peer.sendto(packet.encode(), "/run/systemd/journal/socket")
    except OSError:
        pass  # Diagnostics must never hold up a camera buffer or a setup operation.


def record(message, data=DATA):
    token, active, _ = capture(data)
    if active:
        send(token, message)


def failure(component, exc, data=DATA):
    token, active, _ = capture(data)
    if not active:
        return
    frames = traceback.extract_tb(exc.__traceback__, limit=-1)
    location = (
        f" at {Path(frames[0].filename).name}/{frames[0].name}:{frames[0].lineno}" if frames else ""
    )
    detail = type(exc).__name__
    if isinstance(exc, OSError):
        detail += f" errno={exc.errno}"
        if exc.errno == errno.EAGAIN:
            try:
                group = next(
                    line[3:]
                    for line in Path("/proc/self/cgroup").read_text().splitlines()
                    if line.startswith("0::")
                )
                directory = Path("/sys/fs/cgroup") / group.lstrip("/")
                for metric in ("pids.current", "pids.max"):
                    value = (directory / metric).read_text().strip()
                    if value.isdecimal() or value == "max":
                        detail += f" {metric}={value}"
            except (OSError, StopIteration):
                pass
    if isinstance(exc, subprocess.CalledProcessError):
        detail += f" exit={exc.returncode}"
        if isinstance(exc.cmd, (list, tuple)) and exc.cmd:
            detail += f" program={Path(exc.cmd[0]).name}"
    send(token, f"{component} failed: {detail}{location}")


def xr_events(path):
    try:
        with path.open("rb") as stream:
            stream.seek(max(0, os.fstat(stream.fileno()).st_size - 262144))
            lines = stream.read(262144).decode(errors="replace").splitlines()
    except OSError:
        return ["XRService log unavailable"]
    result = []
    for line in lines:
        # Extract known diagnostics. The rest of Valve's log can contain account and room data.
        if match := re.search(
            r"XRService version: (\d{4}-\d\d-\d\d \d\d:\d\d:\d\d [a-fA-F0-9]{4,40})\b", line
        ):
            event = f"version={match[1]}"
        elif match := re.search(r"Cached (\d{1,2}) camera paths", line):
            event = f"registered cameras={match[1]}"
        elif match := re.search(r"ISP (enabled|disabled) for tracking cameras", line):
            event = f"tracking camera ISP={match[1]}"
        elif match := re.search(r"Setting up cameras camerasToUse: ([01]{4})\b", line):
            event = f"tracking camera mask={match[1]}"
        elif match := re.search(r"Set upper camera driver (not in-sync|in-sync)\b", line):
            event = f"upper cameras {match[1]}"
        elif match := re.search(
            r"Received passthrough camera connection event \(connected=([01])\)", line
        ):
            event = f"passthrough accessory connected={match[1]}"
        elif match := re.search(r"Tracking cameras streaming (paused|resumed|started)", line):
            event = f"stream {match[1]}"
        elif "FATAL ERROR" in line:
            event = "fatal"
            if match := re.search(
                r"get camera path for (upper_left|upper_right|slam_left|slam_right|passthrough_left|passthrough_right)",
                line,
            ):
                event += f" missing camera={match[1]}"
            if match := re.search(r"\b(VIDIOC_[A-Z_]+)\b", line):
                event += f" operation={match[1]}"
            if match := re.search(r"errno (\d{1,4})", line):
                event += f" errno={match[1]}"
        else:
            continue
        stamp = re.search(
            r"\b(?:[A-Z][a-z]{2} [A-Z][a-z]{2} +\d{1,2} \d{4} )?\d{2}:\d{2}:\d{2}(?:\.\d+)?", line
        )
        result.append(f"XRService {stamp[0] if stamp else ''} {event}")
    return result[-10:]


def snapshot(data):
    try:
        os_info = platform.freedesktop_os_release()
    except OSError:
        os_info = {}
    try:
        channel = subprocess.check_output(
            ["/usr/bin/atomupd-manager", "tracked-branch"],
            text=True,
            stderr=subprocess.DEVNULL,
            timeout=3,
        ).strip()
    except (OSError, subprocess.SubprocessError):
        channel = ""
    if channel not in ("beta", "stable", "main", "preview"):
        channel = "unknown"
    try:
        version = json.loads((data.parent / "app/release.json").read_bytes())["version"]
    except (OSError, ValueError, KeyError, TypeError):
        version = "unknown"
    result = [
        f"Controller Utils {version} | SteamOS {os_info.get('VERSION_ID', 'unknown')} build {os_info.get('BUILD_ID', 'unknown')} | channel={channel} | kernel={platform.release()}",
        f"Camera subdevices={len(list(Path('/sys/class/video4linux').glob('v4l-subdev*')))} calibration_file={(data / 'camera-calibration.json').is_file()}",
    ]
    try:
        config = Path.home() / ".config/openvr/config/cv/xrservice/XRServiceSettings.json"
        mode = json.loads(config.read_bytes())["xrSessionSettings"][
            "lightSourceFrequencyDetectionMode"
        ]
        if isinstance(mode, str) and re.fullmatch(
            r"(force)?(50|60)hz|auto(matic|detect)?|default|detect", mode, re.I
        ):
            result.append("Anti-Flicker mode=" + mode)
        else:
            result.append("Anti-Flicker mode unrecognized")
    except (OSError, ValueError, KeyError, TypeError):
        result.append("Anti-Flicker mode unavailable")
    try:
        settings = json.loads((data / "settings.json").read_bytes())
        fields = ("tracking_enabled", "brightness", "smoothing_ms", "automatic_updates")
        result.append(
            "App settings "
            + " ".join(f"{k}={settings[k]}" for k in fields if type(settings.get(k)) in (int, bool))
        )
    except (OSError, ValueError, TypeError, AttributeError):
        pass
    uid = os.getuid()
    runtime = Path(os.environ.get("XDG_RUNTIME_DIR", f"/run/user/{uid}"))
    try:
        paths = {
            f[7]
            for line in Path("/proc/net/unix").read_text().splitlines()
            if len(f := line.split(maxsplit=7)) == 8 and f[3] == "00010000"
        }
        endpoints = {k: f"@controller-utils.{k}.v1.{uid}" for k in ("optical", "input", "beam")}
        endpoints["camera"] = str(runtime / "controller-utils-camera.sock")
        result.append(
            "Listening sockets " + " ".join(f"{k}={v in paths}" for k, v in endpoints.items())
        )
    except OSError:
        result.append("Socket state unavailable")
    logs = Path.home() / ".local/share/Steam/logs"
    current = logs / "xrservice.txt"
    result.append("Recent XRService entries (original log times, may predate recording)")
    token, _, stopped = capture(data)
    if token:
        # SteamVR replaces the current log on each restart, including after a camera crash.
        for previous in sorted(logs.glob("XRService-????.??.??_??-??-??.log"))[-3:]:
            try:
                modified = previous.stat().st_mtime_ns // 1000
                if (
                    int(token) <= modified
                    and (not stopped or modified <= stopped)
                    and previous.resolve() != current.resolve()
                ):
                    result.extend(xr_events(previous))
            except OSError:
                continue
    result.extend(xr_events(current))
    return result


def journal(matches, limit, fields, *time_range):
    output = subprocess.check_output(
        [
            "/usr/bin/journalctl",
            "--quiet",
            "--no-pager",
            "-o",
            "json",
            "--output-fields=" + fields,
            "-n",
            str(limit),
            *time_range,
            *matches,
        ],
        timeout=8,
        stderr=subprocess.DEVNULL,
    )
    return [json.loads(line) for line in output.splitlines()]


def stamp(us):
    return datetime.fromtimestamp(int(us) / 1000000, UTC).isoformat(timespec="milliseconds")


def export(data, token, stopped, downloads):
    sections = [
        "Controller Utils debug log",
        f"Capture started {stamp(token)}",
        f"Capture stopped {stamp(stopped)}" if stopped else "Capture still active",
        "",
        "System at export",
        *snapshot(data),
        "",
        f"Capture events (latest {LIMIT}, UTC)",
    ]
    complete = True
    try:
        rows = journal(
            [
                f"_UID={os.getuid()}",
                "SYSLOG_IDENTIFIER=controller-utils-debug",
                "CU_CAPTURE_ID=" + token,
            ],
            LIMIT,
            "MESSAGE",
        )
        for row in rows:
            msg = row.get("MESSAGE")
            if isinstance(msg, str):
                sections.append(
                    stamp(row["__REALTIME_TIMESTAMP"])
                    + "  "
                    + " ".join(msg.split())[:600].replace(str(Path.home()) + "/", "~/")
                )
        if not rows:
            sections.append("No capture events available in the journal.")
            complete = False
    except (OSError, ValueError, KeyError, subprocess.SubprocessError):
        sections.append("Capture journal unavailable.")
        complete = False
    sections.extend(["", "XRService crashes during capture (latest 12, UTC)"])
    try:
        time_range = ["--since=@" + str(int(token) // 1000000)]
        if stopped:
            time_range.append("--until=@" + str(stopped // 1000000 + 1))
        rows = journal(
            [
                f"COREDUMP_UID={os.getuid()}",
                "COREDUMP_EXE=/opt/steamvr/drivers/cv/bin/linuxarm64/XRService",
            ],
            12,
            "COREDUMP_SIGNAL",
            *time_range,
        )
        sections.extend(
            stamp(row["__REALTIME_TIMESTAMP"])
            + f"  XRService signal={row.get('COREDUMP_SIGNAL', '?')}"
            for row in rows
            if int(token) <= int(row["__REALTIME_TIMESTAMP"])
            and (not stopped or int(row["__REALTIME_TIMESTAMP"]) <= stopped)
        )
        if sections[-1].startswith("XRService crashes"):
            sections.append("None recorded.")
    except (OSError, ValueError, KeyError, subprocess.SubprocessError):
        sections.append("Crash journal unavailable.")
    downloads.mkdir(mode=0o700, parents=True, exist_ok=True)
    target = downloads / (
        "controller-utils-debug-"
        + datetime.fromtimestamp(int(token) / 1000000, UTC).strftime("%Y%m%d-%H%M%S-%f")
        + ".txt"
    )
    if not complete and target.exists():
        raise OSError(f"Journal unavailable. Previous report kept in Downloads: {target.name}")
    with tempfile.NamedTemporaryFile(
        mode="w", encoding="utf-8", dir=downloads, delete=False
    ) as report:
        temp = Path(report.name)
        try:
            report.write("\n".join(sections) + "\n")
            report.flush()
            os.replace(temp, target)
        finally:
            temp.unlink(missing_ok=True)
    return target, complete


def main(args):
    mode = args[0] if args else "save"
    data = Path(args[1]) if len(args) > 1 else DATA
    token, active, stopped = capture(data)
    if not token:
        raise ValueError("Enable Debug logging in the App tab first.")
    if mode == "snapshot" and active:
        for line in snapshot(data):
            send(token, line)
        return "Recording. Reproduce the issue, then turn logging off."
    if mode != "save":
        raise ValueError("Unknown debug operation.")
    target, complete = export(data, token, stopped, Path.home() / "Downloads")
    return (
        f"Saved in Downloads: {target.name}"
        if complete
        else f"Journal unavailable or empty. Saved system details in Downloads: {target.name}"
    )


if __name__ == "__main__":
    try:
        print(json.dumps({"message": main(sys.argv[1:])}), flush=True)
    except (OSError, ValueError, subprocess.SubprocessError) as exc:
        print(json.dumps({"message": f"Could not prepare the debug log: {exc}"}), flush=True)
        sys.exit(1)
