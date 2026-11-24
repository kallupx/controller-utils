"""Start the camera worker with NumPy for the running SteamOS Python."""

import fcntl
import json
import os
import subprocess
import sys
import tempfile
from pathlib import Path

NUMPY = "2.5.5"
NUMPY_WHEELS = (
    "a0750f2156d53446caca85663d0b65d67214e1664b9050454a731ea1418d176a",
    "ae2b8285d0b4eed24739c3ca9ec0fa424a0247489c01e1d5fe2815a599e11d33",
    "cc6eb7502facce90349b4097a26821af224aaa0ab36e78304711cdb544352882",
    "e99bf8a3d54693f2063fa6c7d13955d460a9f543d9dbc80e69a92da288259b28",
)


def run(*args):
    subprocess.run(args, check=True, stdout=sys.stderr, stdin=subprocess.DEVNULL, timeout=180)


def camera_device_warning():
    devices = Path("/sys/class/video4linux")
    if devices.is_dir() and not any(devices.glob("v4l-subdev*")):
        return "SteamOS camera devices are missing. Restart Frame and check its own passthrough."


def numpy_python(data):
    target = data / f"numpy-{NUMPY}-{sys.implementation.cache_tag}"
    python = target / "bin/python"
    if python.is_file():
        return python
    with (data / "numpy.lock").open("a") as lock:
        fcntl.flock(lock, fcntl.LOCK_EX)
        if python.is_file():
            return python
        if target.exists():
            raise RuntimeError(f"Move the incomplete Python environment aside at {target}")
        with tempfile.TemporaryDirectory(prefix="numpy-", dir=data) as directory:
            directory = Path(directory)
            trial = directory / "venv"
            requirement = directory / "requirements.txt"
            hashes = " ".join(f"--hash=sha256:{digest}" for digest in NUMPY_WHEELS)
            requirement.write_text(f"numpy=={NUMPY} {hashes}\n")
            run(sys.executable, "-m", "venv", trial)
            run(
                trial / "bin/python",
                "-m",
                "pip",
                "--isolated",
                "install",
                "--index-url",
                "https://pypi.org/simple",
                "--only-binary=:all:",
                "--no-deps",
                "--require-hashes",
                "-r",
                requirement,
            )
            run(
                trial / "bin/python",
                "-I",
                "-c",
                f"import numpy; assert numpy.__version__ == '{NUMPY}'",
            )
            trial.rename(target)
    return python


def main():
    optical = Path(__file__).resolve().parent
    if len(sys.argv) == 3 and sys.argv[1] == "--validate-calibration":
        python = numpy_python(Path(sys.argv[2]))
        os.execv(
            python,
            [
                python,
                "-IB",
                "-c",
                "import json,sys; sys.path.insert(0,sys.argv[1]); "
                "from frame_raw_camera import parse_calibration; "
                "parse_calibration(json.load(sys.stdin))",
                optical,
            ],
        )
    try:
        python = numpy_python(optical.parents[1] / "app-data")
        os.execv(python, [python, "-Bu", optical / "frame_camera_worker.py", sys.argv[1]])
    except (OSError, RuntimeError, subprocess.SubprocessError) as error:
        sys.path.insert(0, str(optical))
        from frame_debug import failure

        failure("Camera Python startup", error)
        print(
            json.dumps(dict(op="error", message=f"Camera Python setup failed: {error}")), flush=True
        )
        sys.exit(1)


if __name__ == "__main__":
    main()
