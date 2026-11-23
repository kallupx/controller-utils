import math
import unittest
from unittest.mock import patch

try:
    import numpy as np
except ImportError:
    np = None


@unittest.skipIf(np is None, "the Frame palm experiment needs NumPy")
class PalmBootstrapTest(unittest.TestCase):
    def setUp(self):
        from tools.frame_palm_bootstrap import PalmBootstrap

        def camera(side, x):
            return {
                "sourceCamera": "slam_" + side,
                "cameraModel": "kb",
                "width": 64,
                "height": 64,
                "intrinsics": dict(fx=30, fy=30, cx=32, cy=32, k1=0, k2=0, k3=0, k4=0),
                "extrinsics": {
                    "__units": "mm",
                    "position": [x, 0, 0],
                    "plus_x": [1, 0, 0],
                    "plus_z": [0, 0, 1],
                },
            }

        calibration = {
            "cameras": {"L": camera("left", -40), "R": camera("right", 40)},
            "head_from_cal": [1, 0, 0, 0, 0, -1, 0, 0, 0, 0, -1, 0],
        }
        self.track = PalmBootstrap(calibration, self.unexpected_inference)

    def unexpected_inference(self, images):
        self.fail("The model received an empty camera view")

    def test_blank_eye_rejected(self):
        blank = np.zeros((320, 320), dtype=np.uint8)
        texture = np.random.default_rng(20).integers(0, 256, blank.shape, dtype=np.uint8)
        for side in (-1, 1):
            for patches in ([blank, texture], [texture, blank]):
                self.assertIsNone(self.track._features(patches, [0, 0], side, 0.31))

    def test_stereo_geometry(self):
        points = np.array([[0.01 * i, 0.03, 0.45 + 0.01 * i] for i in range(6)])
        rays = points - self.track.origins[:, None]
        np.testing.assert_allclose(self.track._triangulate(rays), points, atol=1e-9)
        self.assertIsNone(self.track._triangulate(np.stack([rays[0], rays[0]])))
        self.assertIsNone(self.track._triangulate(-rays))

    def test_one_palm_needs_gravity_and_preserves_observed_yaw(self):
        led = np.array([0.0, 0.0, 0.45])
        angle = math.radians(35)
        c, s = math.cos(angle), math.sin(angle)
        rotation = np.array([[c, 0, s], [0, 1, 0], [-s, 0, c]])
        up = np.array([0.0, 1.0, 0.0])
        images = [np.zeros((64, 64), dtype=np.uint8)] * 2
        for side in (-1, 1):
            hand = led + rotation @ [side * 0.095, -0.015, 0.075]
            palms = [hand, None] if side < 0 else [None, hand]
            with patch.object(self.track, "_hand", side_effect=palms * 2):
                self.assertIsNone(self.track._pose(images, led, 1.0, None))
                pose = self.track._pose(images, led, 2.0, [up, up])
                np.testing.assert_allclose(pose.rotation, rotation, atol=1e-8)

    def test_stale_missing_and_repeated_camera_observations(self):
        from tools.frame_palm_bootstrap import StartingPose

        led = np.array([0, 0, 0.45])
        with patch.object(
            self.track, "_pose", side_effect=lambda ims, p, t, g: StartingPose(t, p, np.eye(3))
        ):
            for t, infrared, pos, expect in (
                (0.01, 0.01, led, False),
                (0.02, 0.01, led, False),
                (0.04, 0.04, led, False),
                (0.07, 0.07, led, True),
                (0.10, 0.07, led, False),
                (0.13, 0.13, led, False),
                (0.16, -math.inf, None, False),
                (0.19, 0.19, led, False),
                (0.22, 0.22, led, False),
                (0.25, 0.25, led, True),
            ):
                with self.subTest(t=t):
                    self.assertEqual(self.track.update(t, (), infrared, pos) is not None, expect)

    def test_slow_palm_inference_still_requires_three_consistent_poses(self):
        from tools.frame_palm_bootstrap import StartingPose

        led = np.array([0, 0, 0.45])
        moved = led + [0.12, 0, 0]
        with patch.object(
            self.track, "_pose", side_effect=lambda ims, p, t, g: StartingPose(t, p, np.eye(3))
        ):
            for t, point, expect in (
                (1.0, led, False),
                (1.15, led, False),
                (1.50, led, False),
                (1.65, moved, False),
                (1.80, moved, False),
                (1.95, moved, True),
            ):
                with self.subTest(t=t):
                    self.assertEqual(self.track.update(t, (), t, point) is not None, expect)
