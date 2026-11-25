"""Read Frame cameras while the Controller Utils session owns the gamepad."""

import json
import os
import select
import signal
import socket
import struct
import sys
import time
import traceback
from collections import deque
from concurrent.futures import ThreadPoolExecutor
from contextlib import ExitStack, contextmanager

os.environ["OPENBLAS_NUM_THREADS"] = "1"

import frame_debug
import frame_raw_camera as raw
import numpy as np  # noqa: E402
from frame_camera_runtime import camera_device_warning
from frame_led_tracking import LedTracker, centroids
from frame_palm_bootstrap import FramePalmModel, PalmBootstrap


def raw_time():
    return time.clock_gettime(time.CLOCK_MONOTONIC_RAW)


def emit(op, **fields):
    if op in ("waiting", "ready", "light", "seed", "disconnected"):
        frame_debug.record("Camera " + op + (" " + fields["message"] if op == "waiting" else ""))
    print(json.dumps(dict(op=op, **fields), allow_nan=False), flush=True)


class CameraTracking:
    def __init__(self, cams, config, infer, executor=None):
        self.led = LedTracker(cams)
        self.palm = PalmBootstrap(config, infer)
        self.front = tuple(k for k in cams)[:2]
        self.head = self.palm.head
        self.enabled_at = None
        self.lit_requested = False
        self.fix = None
        self.seeded = False
        self.needs_seed = True
        self.check_resume = False
        self.first_ir = None
        self.gravity = deque(maxlen=128)
        self.priors = deque(maxlen=12)
        self.scene = None
        self.executor = executor
        self.pending = None
        self.reset_palm = False

    def reacquire(self):
        self.needs_seed = True
        self.check_resume = False
        self.scene = None
        self.reset_palm = True

    def accept_alignment(self):
        self.seeded = True
        self.needs_seed = False
        self.check_resume = False
        self.scene = None

    def seed(self, pose):
        self.accept_alignment()
        transform = np.column_stack(
            [self.head[:, :3] @ pose.rotation, self.head[:, :3] @ pose.led + self.head[:, 3]]
        )
        return dict(op="seed", time=pose.time, transform=transform.ravel().tolist())

    def exposure(self, kind, t, images, timely, borrowed=False):
        events = []
        if self.lit_requested and (
            not timely(t) or (kind == 38 and (self.enabled_at is None or t < self.enabled_at))
        ):
            return events
        if kind == 38:
            if self.first_ir is None:
                self.first_ir = t
            illuminated = self.enabled_at is not None and t >= self.enabled_at
            prior = None
            if self.priors and (
                self.check_resume or (self.seeded and self.fix and 0 <= t - self.fix[0] < 0.8)
            ):
                stamp, point = min(self.priors, key=lambda entry: abs(t - entry[0]))
                if abs(t - stamp) < 0.045:
                    prior = point
            fix = self.led.update(
                t,
                {k: centroids(im) for k, im in images.items()},
                illuminated,
                prior,
                images,
                resume=self.check_resume and prior is not None,
            )
            if (
                not self.lit_requested
                and t - self.first_ir > 0.5
                and sum(v >= 5 for v in self.led.baselines.values()) >= 2
            ):
                self.lit_requested = True
                events.append(dict(op="light"))
            if fix:
                p, views = fix
                self.fix = t, p
                if self.pending is not None and self.pending.done():
                    pose = self.pending.result()
                    self.pending = None
                    if (
                        pose
                        and self.needs_seed
                        and not self.reset_palm
                        and 0 <= t - pose.time < 0.45
                    ):
                        events.append(self.seed(pose))
                if (self.seeded or self.check_resume) and timely(t):
                    events.append(
                        dict(
                            op="led",
                            time=t,
                            point=(self.head[:, :3] @ p + self.head[:, 3]).tolist(),
                            views=len(views),
                        )
                    )
            elif self.seeded and self.led.bearing is not None and timely(t):
                origin, ray = self.led.bearing
                events.append(
                    dict(
                        op="bearing",
                        time=t,
                        origin=(self.head[:, :3] @ origin + self.head[:, 3]).tolist(),
                        direction=(self.head[:, :3] @ ray).tolist(),
                    )
                )
        elif self.needs_seed and all(k in images for k in self.front):
            # Inference can outlive a producer lease. Retain only the two startup images.
            self.scene = t, [images[k].copy() if borrowed else images[k] for k in self.front]
        if (
            not self.needs_seed
            or self.scene is None
            or self.fix is None
            or self.pending is not None
        ):
            return events
        scene, images = self.scene
        ir, p = self.fix
        if abs(scene - ir) > 0.020 or not timely(scene):
            return events
        self.scene = None
        g = next((g for ts, g in reversed(self.gravity) if 0 <= scene - ts < 0.06), None)
        if self.reset_palm:
            self.palm.previous = None
            self.palm.streak = 0
            self.reset_palm = False
        if self.executor is not None:
            self.pending = self.executor.submit(self.palm.update, scene, images, ir, p, g)
        elif pose := self.palm.update(scene, images, ir, p, g):
            events.append(self.seed(pose))
        return events


class Parent:
    def __init__(self):
        self.buffer = b""
        self.last = time.monotonic()
        self.stopped = False
        self.dark_since = None

    def poll(self, track):
        while select.select([0], [], [], 0)[0]:
            chunk = os.read(0, 16384)
            if not chunk:
                self.stopped = True
                break
            self.buffer += chunk
            while b"\n" in self.buffer:
                line, self.buffer = self.buffer.split(b"\n", 1)
                msg = json.loads(line)
                op = msg["op"]
                if op == "stop":
                    self.stopped = True
                    return
                if op == "sample":
                    if track is not None and "up" in msg:
                        # Rig up = headset up rotated back through head_from_cal.
                        head_up, body_up = np.asarray(msg["up"]).reshape(2, 3)
                        track.gravity.append((msg["time"], [head_up @ track.head[:, :3], body_up]))
                    if track is not None and "prior" in msg:
                        point = np.asarray(msg["prior"], dtype=float)
                        if point.shape == (3,) and np.isfinite(point).all():
                            track.priors.append(
                                (msg["time"], (point - track.head[:, 3]) @ track.head[:, :3])
                            )
                elif op == "dark":
                    frame_debug.record("Controller LED off for baseline")
                    self.dark_since = msg["time"]
                elif op == "request_ready":
                    if track is not None:
                        # The host hasn't armed the LED. Collect a fresh dark baseline after its reply.
                        self.dark_since = None
                        track.led = LedTracker(track.led.cameras)
                        track.first_ir = track.enabled_at = None
                        track.lit_requested = False
                        emit("ready")
                elif op == "lit" and track is not None:
                    frame_debug.record("Controller LED on")
                    track.enabled_at = msg["time"]
                elif op == "reacquire" and track is not None:
                    track.reacquire()
                elif op == "check_resume" and track is not None:
                    track.check_resume = True
                elif op == "resume" and track is not None:
                    track.accept_alignment()
                else:
                    if op not in ("lit", "reacquire"):
                        raise ValueError("Unknown session message")
                self.last = time.monotonic()
            if len(self.buffer) > 16384:
                raise ValueError("Session message too large")
        if track is not None and not self.stopped and time.monotonic() - self.last > 4:
            raise TimeoutError("Camera session heartbeat stopped")
        return not self.stopped


@contextmanager
def camera_feed(headset, parent):
    # XRService accepts readers from its capture loop, which can stay paused in standby.
    next_notice = time.monotonic() + 2
    last_notice = None
    connect_errno = None
    next_error_log = 0

    def waiting(message):
        nonlocal next_notice, last_notice
        if not parent.poll(None):
            return False
        if time.monotonic() >= next_notice:
            message = camera_device_warning() or message
            if message != last_notice:
                emit("waiting", message=message)
                last_notice = message
            next_notice = time.monotonic() + 2
        return True

    while waiting(
        "Waiting for SteamVR to share its tracking cameras. Put Frame on or restart SteamVR."
    ):
        with socket.socket(socket.AF_UNIX, socket.SOCK_SEQPACKET) as sock:
            sock.settimeout(1)
            try:
                sock.connect(str(raw.address()))
            except (FileNotFoundError, ConnectionRefusedError, BlockingIOError) as error:
                now = time.monotonic()
                if connect_errno != error.errno or now >= next_error_log:
                    frame_debug.failure("Connecting to camera provider", error)
                    connect_errno = error.errno
                    next_error_log = now + 10
                time.sleep(0.1)
                continue
            connect_errno = None
            _, uid, _ = struct.unpack(
                "3i", sock.getsockopt(socket.SOL_SOCKET, socket.SO_PEERCRED, 12)
            )
            if uid != os.getuid():
                raise ValueError("Camera provider belongs to a different user")
            calibration = None
            seq = -1
            stats_since = None
            scene = rejected_ir = 0
            last_ir_age = None

            def first_ir(kind, t, images):
                nonlocal scene, rejected_ir, last_ir_age
                if kind != 38:
                    scene += 1
                    return 0
                last_ir_age = raw_time() - t
                if -0.02 <= last_ir_age <= 0.14:
                    return len(images)
                rejected_ir += 1
                return 0

            while waiting(
                "Waiting for infrared images from Frame's tracking cameras."
                if calibration is not None
                else "Waiting for SteamVR camera calibration. Put Frame on to wake it."
            ):
                if stats_since is not None and time.monotonic() - stats_since >= 5:
                    age = "none" if last_ir_age is None else str(round(last_ir_age * 1000))
                    frame_debug.record(
                        f"Camera startup scene={scene} rejected_ir={rejected_ir} last_ir_age_ms={age}"
                    )
                    stats_since = time.monotonic()
                    scene = rejected_ir = 0
                    last_ir_age = None
                if not select.select([sock], [], [], 0.1)[0]:
                    continue
                try:
                    if calibration is None:
                        calibration = raw.calibration(sock, headset)
                        stats_since = time.monotonic()
                        frame_debug.record(
                            f"Camera calibration accepted views={len(calibration[0])}"
                        )
                        continue
                    seq, live = raw.frame(sock, calibration[0], seq, first_ir)
                except EOFError:
                    frame_debug.record("Camera provider closed during startup")
                    break  # A provider can exit after calibration but before the first IR image.
                if live:
                    frame_debug.record(f"First fresh camera IR views={live}")
                    yield sock, *calibration, seq
                    return
    yield None


def main(headset):
    parent = Parent()

    def stop(signum, frame):
        parent.stopped = True

    signal.signal(signal.SIGTERM, stop)
    signal.signal(signal.SIGINT, stop)
    with ExitStack() as stack:
        frame_debug.record("Camera worker starting")
        busy = False
        next_error_log = 0
        while parent.poll(None):
            try:
                model = stack.enter_context(FramePalmModel())
                frame_debug.record("Palm helper started")
                break
            except BlockingIOError as exc:
                now = time.monotonic()
                if now >= next_error_log:
                    frame_debug.failure("Palm helper launch", exc)
                    next_error_log = now + 10
                if not busy:
                    emit(
                        "waiting",
                        message="SteamOS is busy. Waiting to start palm tracking. Close unused apps if this persists.",
                    )
                    busy = True
                time.sleep(0.25)
        else:
            return
        feed = stack.enter_context(camera_feed(headset, parent))
        if feed is None:
            return
        sock, cams, config, seq = feed
        pool = stack.enter_context(ThreadPoolExecutor(max_workers=1))
        track = CameraTracking(cams, config, model, pool)
        parent.last = time.monotonic()
        emit("ready")
        last_ir = time.monotonic()
        stats_since = last_ir
        token, recording, _ = frame_debug.capture()
        ir = scene = stale = fixes = 0
        ir_row_bytes = "none"

        def consume(kind, t, images):
            nonlocal last_ir, ir, scene, stale, fixes, ir_row_bytes
            if not -0.02 <= raw_time() - t <= 0.14:
                if recording:
                    stale += 1
                return []
            if recording:
                if kind == 38 and ir == 0:
                    ir_row_bytes = ",".join(str(im.strides[0]) for im in images.values())
                ir += kind == 38
                scene += kind != 38
            if kind == 38:
                last_ir = time.monotonic()
            if parent.dark_since is None or t < parent.dark_since:
                return []
            events = track.exposure(
                kind, t, images, lambda ts: -0.02 <= raw_time() - ts <= 0.14, borrowed=True
            )
            if recording and track.fix is not None and kind == 38 and track.fix[0] == t:
                fixes += 1
            return events

        while parent.poll(track):
            now = time.monotonic()
            if now - stats_since >= 5:
                current, active, _ = frame_debug.capture()
                if recording and active and token == current:
                    frame_debug.send(
                        token,
                        f"Camera {now - stats_since:.1f}s ir={ir} scene={scene} stale={stale} ir_row_bytes={ir_row_bytes} "
                        f"baseline_views={sum(n >= 5 for n in track.led.baselines.values())} "
                        f"led_fixes={fixes} lit={track.enabled_at is not None} palm_streak={track.palm.streak} aligned={track.seeded}",
                    )
                token, recording, stats_since = current, active, now
                ir = scene = stale = fixes = 0
                ir_row_bytes = "none"
            if now - last_ir > 5:
                frame_debug.record("Camera stream stalled: no fresh IR for five seconds")
                break
            if not select.select([sock], [], [], 0.05)[0]:
                continue
            try:
                seq, events = raw.frame(sock, cams, seq, consume)
            except EOFError:
                frame_debug.record("Camera provider disconnected while streaming")
                break
            for event in events:
                emit(**event)
        else:
            return
    # All camera leases and the palm helper are released before asking the app to rearm.
    emit("disconnected")
    while parent.poll(None):
        select.select([0], [], [], 0.1)


if __name__ == "__main__":
    try:
        main(sys.argv[1])
    except Exception as exc:
        frame_debug.failure("Camera worker", exc)
        traceback.print_exc()
        emit("error", message=str(exc))
        sys.exit(1)
