"""Estimate a gamepad starting pose from its stereo LED position and two scene images."""

import math
import subprocess
from dataclasses import dataclass
from pathlib import Path

import numpy as np


class FramePalmModel:
    """Use the headset's installed network and ONNX Runtime in a persistent worker."""

    model = Path(
        "/opt/steamvr/drivers/cv/resources/minControllerFeatureNNModel/K2V3/"
        "2024-12-13_16-02_StereoSQRT2-half-WithLongSleevesSynthetic_ch199_partial_AU_WU.onnx"
    )

    def __enter__(self):
        self.process = subprocess.Popen(
            [Path(__file__).with_name("frame_palm_inference"), self.model],
            stdin=subprocess.PIPE,
            stdout=subprocess.PIPE,
        )
        return self

    def __call__(self, images):
        if images.shape != (2, 128, 128) or images.dtype != np.uint8:
            raise ValueError("Expected two 128 by 128 grayscale palm crops")
        self.process.stdin.write(images.tobytes())
        self.process.stdin.flush()
        size = 4 * 2 * 23 * 3 * 4
        data = self.process.stdout.read(size)
        if len(data) != size:
            raise RuntimeError("Palm inference stopped before both camera results arrived")
        return np.frombuffer(data, dtype="<f4").reshape(4, 2, 23, 3)

    def __exit__(self, exc_type, exc, traceback):
        try:
            self.process.stdin.close()
        finally:
            self.process.stdout.close()
            try:
                self.process.wait(timeout=3)
            except subprocess.TimeoutExpired:
                self.process.kill()
                self.process.wait()


def unit(vector):
    length = np.linalg.norm(vector)
    if not np.isfinite(length) or length < 1e-6:
        raise ValueError("Undefined direction")
    return vector / length


def sample_gray(image, x, y):
    """Bilinear interpolation with neutral padding outside the camera image."""
    rows, cols = image.shape
    valid = (x >= 0) & (x < cols - 1) & (y >= 0) & (y < rows - 1)
    x = np.clip(x, 0, cols - 2)
    y = np.clip(y, 0, rows - 2)
    ix = x.astype(int)
    iy = y.astype(int)
    dx = x - ix
    dy = y - iy
    top = image[iy, ix] * (1 - dx) + image[iy, ix + 1] * dx
    bottom = image[iy + 1, ix] * (1 - dx) + image[iy + 1, ix + 1] * dx
    return np.where(valid, top * (1 - dy) + bottom * dy, 127).round().astype(np.uint8)


class Camera:
    def __init__(self, config):
        if (
            config["cameraModel"] != "kb"
            or config["extrinsics"]["__units"] != "mm"
            or config.get("preRotateClockwiseByDeg", 0)
            or config.get("parent")
        ):
            raise ValueError("Unsupported tracking camera calibration")
        e = config["extrinsics"]
        self.origin = np.asarray(e["position"]) * 0.001
        x, z = np.array(e["plus_x"]), np.array(e["plus_z"])
        self.axes = np.array([x, np.cross(z, x), z])
        self.shape = (config["height"], config["width"])
        k = config["intrinsics"]
        self.focal = np.array([k["fx"], k["fy"]])
        self.center = np.array([k["cx"], k["cy"]])
        self.distortion = np.array([k[f"k{i}"] for i in range(1, 5)])
        if (
            self.origin.shape != (3,)
            or not np.isfinite(self.origin).all()
            or not np.allclose(self.axes @ self.axes.T, np.eye(3), atol=1e-4)
            or not np.isfinite([*self.focal, *self.center, *self.distortion]).all()
            or np.any(self.focal <= 0)
        ):
            raise ValueError("Invalid camera calibration")

    def project(self, points):
        rays = (points - self.origin) @ self.axes.T
        radius = np.linalg.norm(rays[..., :2], axis=-1)
        angle = np.arctan2(radius, rays[..., 2])
        t = angle * angle
        a, b, c, d = self.distortion
        factor = angle * (1 + t * (a + t * (b + t * (c + t * d))))
        xy = rays[..., :2] * (factor / np.maximum(radius, 1e-12))[..., None]
        return xy * self.focal + self.center


@dataclass
class StartingPose:
    time: float
    led: np.ndarray
    # Columns are the gamepad's right, up and back axes in the camera rig.
    rotation: np.ndarray


class PalmBootstrap:
    """The caller supplies camera-order left/right and a contemporaneous stereo LED fix."""

    def __init__(self, calibration, infer):
        by_name = {v["sourceCamera"]: v for v in calibration["cameras"].values()}
        self.cameras = [Camera(by_name[k]) for k in ("slam_left", "slam_right")]
        self.origins = np.stack([camera.origin for camera in self.cameras])
        self.head = np.asarray(calibration["head_from_cal"]).reshape(3, 4)
        if not np.isfinite(self.head).all() or not np.allclose(
            self.head[:, :3] @ self.head[:, :3].T, np.eye(3), atol=1e-4
        ):
            raise ValueError("Invalid headset calibration")
        self.infer = infer
        self.previous = None
        self.streak = 0
        self.last_time = self.last_led_time = -math.inf

    def _triangulate(self, rays):
        rays = rays / np.maximum(np.linalg.norm(rays, axis=-1, keepdims=True), 1e-12)
        left, right = rays
        baseline = self.origins[0] - self.origins[1]
        b = np.sum(left * right, axis=-1)
        denom = 1 - b * b
        if np.any(denom < 1e-4):
            return None
        d, e = left @ baseline, right @ baseline
        s, t = (b * e - d) / denom, (e - b * d) / denom
        if min(s.min(), t.min()) < 0.08 or max(s.max(), t.max()) > 1.6:
            return None
        p = self.origins[0] + s[:, None] * left
        q = self.origins[1] + t[:, None] * right
        if np.median(np.linalg.norm(p - q, axis=1)) > 0.008:
            return None
        return (p + q) / 2

    def _features(self, patches, center, side, size):
        axis = ((np.arange(128) + 0.5) / 128 - 0.5) * size
        offset = (patches[0].shape[0] - 1) / 2
        x = (center[0] + axis) * 512 + offset
        y = (center[1] + axis) * 512 + offset
        if x.min() < 0 or x.max() >= 2 * offset or y.min() < 0 or y.max() >= 2 * offset:
            return None
        images = np.stack([sample_gray(im, x[None, :], y[:, None]) for im in patches])
        if np.any(images.std(axis=(1, 2)) < 5):
            return None
        if side < 0:
            images = images[::-1, :, ::-1].copy()
        result = self.infer(images)
        if result.shape != (4, 2, 23, 3) or not np.isfinite(result).all():
            raise ValueError("Invalid palm network result")
        # The first 17 predictions describe a multi-LED controller. Slots 17..22 are hand landmarks.
        coords = result[0, :, 17:, :2].copy()
        uncertainty = result[1, :, 17:, :2]
        if np.any(uncertainty <= 0) or np.mean(uncertainty) > 0.035:
            return None
        if side < 0:
            coords = coords[::-1]
            coords[..., 0] *= -1
        return coords * (size / 2) + center

    def _hand_at(self, patches, led, basis, side, size, x, y):
        center = np.array([side * x, y])
        obs = self._features(patches, center, side, size)
        if obs is None:
            return None
        # A blank or ambiguous image can also produce six plausible-looking numbers.
        # Check that the landmarks stay on the same pixels when we move the crop.
        other = self._features(patches, center + [0.085 * size, -0.065 * size], side, size)
        if other is None or np.median(np.linalg.norm(other - obs, axis=-1)) > 0.017:
            return None
        points = self._triangulate(led + ((obs + other) / 2) @ basis - self.origins[:, None])
        if points is None:
            return None
        center = points.mean(axis=0)
        if not 0.035 < np.linalg.norm(points[1] - points[3]) < 0.155:
            return None
        if not 0.04 < np.linalg.norm(center - led) < 0.19:
            return None
        return center

    def _hand(self, patches, led, basis, side):
        for size, x, y in ((0.31, 0.065, 0.032), (0.31, 0.08, 0.05), (0.38, 0.073, 0.035)):
            hand = self._hand_at(patches, led, basis, side, size, x, y)
            if hand is not None:
                return hand
        return None

    def _pose(self, images, led, timestamp, gravity):
        in_head = self.head[:, :3] @ led + self.head[:, 3]
        if not 0.10 < np.linalg.norm(in_head) < 1.4 or in_head[2] >= -0.03:
            return None
        forward = unit(in_head)
        right = np.cross(forward, [0, 1, 0])
        if np.linalg.norm(right) < 0.1:
            return None
        right = unit(right)
        basis = np.stack([right, np.cross(forward, right)]) @ self.head[:, :3]
        axis = (np.arange(320) - 159.5) / 512
        plane = led + axis[None, :, None] * basis[0] + axis[:, None, None] * basis[1]
        patches = []
        for camera, im in zip(self.cameras, images, strict=True):
            if im.shape != camera.shape or im.dtype != np.uint8:
                raise ValueError("Unexpected tracking-camera image format")
            xy = camera.project(plane)
            patches.append(sample_gray(im, xy[..., 0], xy[..., 1]))
        left = self._hand(patches, led, basis, -1)
        right_hand = self._hand(patches, led, basis, 1)
        if left is None and right_hand is None:
            return None
        if left is not None and right_hand is not None:
            span = right_hand - left
            if not 0.10 < np.linalg.norm(span) < 0.34:
                return None
            reference = np.array([1.0, 0, 0])
        else:
            if gravity is None:
                return None
            side = 1 if right_hand is not None else -1
            span = (right_hand if right_hand is not None else left) - led
            # Mean LED-to-palm offset for a normal grip, in gamepad coordinates (metres).
            reference = np.array([side * 0.095, -0.015, 0.075])
            if abs(np.linalg.norm(span) - np.linalg.norm(reference)) > 0.045:
                return None
        if gravity is not None:
            rig_up, body_up = gravity

            def horizontal_basis(up, direction):
                horizontal = direction - up * (up @ direction)
                if np.linalg.norm(horizontal) < 0.35 * np.linalg.norm(direction):
                    return None
                x = unit(horizontal)
                return np.column_stack((x, up, np.cross(x, up)))

            src = horizontal_basis(body_up, reference)
            dst = horizontal_basis(rig_up, span)
            if src is None or dst is None:
                return None
            return StartingPose(timestamp, led, dst @ src.T)
        x = unit(span)
        back = (left + right_hand) / 2 - led
        z = back - x * (x @ back)
        if not 0.025 < np.linalg.norm(z) < 0.14:
            return None
        z = unit(z)
        return StartingPose(timestamp, led, np.column_stack((x, np.cross(z, x), z)))

    def update(self, timestamp, images, led_timestamp, led, gravity=None):
        if not math.isfinite(timestamp) or timestamp <= self.last_time:
            raise ValueError("Expected increasing exposure timestamps")
        self.last_time = timestamp
        pose = None
        if (
            led is not None
            and math.isfinite(led_timestamp)
            and abs(led_timestamp - timestamp) <= 0.020
        ):
            led = np.asarray(led, dtype=float)
            if led.shape != (3,) or not np.isfinite(led).all():
                raise ValueError("Invalid LED position")
            if led_timestamp <= self.last_led_time:
                return None
            self.last_led_time = led_timestamp
            if gravity is not None:
                gravity = np.asarray(gravity, dtype=float)
                if (
                    gravity.shape != (2, 3)
                    or not np.isfinite(gravity).all()
                    or not np.allclose(np.linalg.norm(gravity, axis=1), 1, atol=1e-3)
                ):
                    raise ValueError("Expected unit up vectors in rig and gamepad coordinates")
            pose = self._pose(images, led, timestamp, gravity)
        if pose is None:
            self.previous = None
            self.streak = 0
            return None
        old = self.previous
        self.previous = pose
        consecutive = (
            old is not None
            # A busy worker can skip several scene exposures between inferences.
            and timestamp - old.time < 0.25
            and np.linalg.norm(led - old.led) < 0.08
            and np.trace(old.rotation.T @ pose.rotation) > 1 + 2 * math.cos(math.radians(25))
        )
        self.streak = self.streak + 1 if consecutive else 1
        return pose if self.streak >= 3 else None
