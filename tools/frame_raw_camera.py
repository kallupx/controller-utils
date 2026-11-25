"""Read GRAY8 exposures explicitly shared by a local camera provider.

The producer listens on $XDG_RUNTIME_DIR/controller-utils-camera.sock (Unix
SEQPACKET). The first JSON packet contains version=1, device, rig and metadata
(factory calibration and named camera serials). Subsequent packets contain seq,
exposure ("ir" or "scene"), time (CLOCK_MONOTONIC_RAW seconds), and views, an
array of [camera serial, row stride] in SCM_RIGHTS descriptor order. The Frame
hook sends storage="udmabuf" with [serial, stride, offset] views. Those handles
are read-only references to the memory behind the DMA buffer, with cache
synchronization performed by the hook. A provider must keep a group immutable
until it receives {"release": seq} or confirms the reader has exited.
"""

import array
import errno
import fcntl
import json
import math
import mmap
import os
import socket
import struct
from contextlib import ExitStack
from pathlib import Path

import numpy as np
from frame_led_tracking import calibrated_cameras
from frame_palm_bootstrap import PalmBootstrap

# Linux dma-buf UAPI: _IOW('b', 0, struct dma_buf_sync).
DMA_BUF_IOCTL_SYNC = 0x40086200


def address():
    return (
        Path(os.environ.get("XDG_RUNTIME_DIR", f"/run/user/{os.getuid()}"))
        / "controller-utils-camera.sock"
    )


def receive(sock, stack):
    item = array.array("i")
    try:
        data, control, flags, _ = sock.recvmsg(
            131072, socket.CMSG_SPACE(4 * item.itemsize), socket.MSG_CMSG_CLOEXEC
        )
    except (ConnectionError, TimeoutError) as exc:
        raise EOFError("Raw camera feed disconnected") from exc
    fds = []
    for level, kind, payload in control:
        if level == socket.SOL_SOCKET and kind == socket.SCM_RIGHTS:
            item.frombytes(payload)
    for fd in item:
        stack.callback(os.close, fd)
        fds.append(fd)
    if flags & (socket.MSG_TRUNC | socket.MSG_CTRUNC) or len(fds) > 4:
        raise ValueError("Raw camera packet too large")
    if not data:
        raise EOFError("Raw camera feed disconnected")
    msg = json.loads(data)
    if not isinstance(msg, dict):
        raise ValueError("Invalid raw camera header")
    return msg, fds


def parse_calibration(packet, headset=None):
    serial = packet["device"]["device_serial_number"]
    if (
        packet.get("version") != 1
        or not isinstance(serial, str)
        or not serial
        or (headset is not None and serial != headset)
    ):
        raise ValueError("Raw camera feed does not match this Frame")
    cameras, config = calibrated_cameras(packet["rig"], packet["metadata"], packet["device"])
    if [camera.shape for camera in cameras.values()] != [(1024, 1056)] * 2 + [(480, 640)] * 2:
        raise ValueError("Invalid Frame camera dimensions")
    PalmBootstrap(config, None)
    return cameras, config


def calibration(sock, headset):
    with ExitStack() as stack:
        packet, fds = receive(sock, stack)
        if fds:
            raise ValueError("Unexpected camera descriptors in calibration")
        return parse_calibration(packet, headset)


def pixels(fd, shape, stride, stack, *, offset=0, udmabuf=False):
    height, width = shape
    if type(stride) is not int or not (0 < height <= 4096 and 0 < width <= stride <= 8192):
        raise ValueError("Invalid raw camera dimensions")
    size = stride * height
    if type(offset) is not int or offset < 0 or offset % mmap.PAGESIZE:
        raise ValueError("Invalid raw camera offset")
    if os.fstat(fd).st_size < offset + size:
        raise ValueError("Raw camera buffer is too short")
    if udmabuf:
        if fcntl.fcntl(fd, fcntl.F_GETFL) & os.O_ACCMODE != os.O_RDONLY or not (
            fcntl.fcntl(fd, fcntl.F_GET_SEALS) & fcntl.F_SEAL_SHRINK
        ):
            raise ValueError("Invalid read-only camera backing")
        buf = stack.enter_context(mmap.mmap(fd, size, access=mmap.ACCESS_READ, offset=offset))
        return np.ndarray(shape, np.uint8, buf, strides=(stride, 1))
    try:
        fcntl.ioctl(fd, DMA_BUF_IOCTL_SYNC, struct.pack("=Q", 1))  # START | READ
    except OSError as e:
        if e.errno != errno.ENOTTY:
            raise
        seals = fcntl.fcntl(fd, fcntl.F_GET_SEALS)
        if seals & (fcntl.F_SEAL_WRITE | fcntl.F_SEAL_SHRINK) != (
            fcntl.F_SEAL_WRITE | fcntl.F_SEAL_SHRINK
        ):
            raise ValueError("Unsealed camera memory")
    else:
        stack.callback(fcntl.ioctl, fd, DMA_BUF_IOCTL_SYNC, struct.pack("=Q", 5))  # END | READ
    buf = stack.enter_context(mmap.mmap(fd, size, access=mmap.ACCESS_READ))
    return np.ndarray(shape, np.uint8, buf, strides=(stride, 1))


def frame(sock, cameras, previous, consume):
    with ExitStack() as stack:
        pkt, fds = receive(sock, stack)
        seq, t, views = pkt["seq"], pkt["time"], pkt["views"]
        storage = pkt.get("storage")
        kind = {"ir": 38, "scene": 32}[pkt["exposure"]]
        if (
            type(seq) is not int
            or seq <= previous
            or not math.isfinite(t)
            or t <= 0
            or not isinstance(views, list)
            or not 0 < len(views) == len(fds) <= 4
            or storage not in (None, "udmabuf")
        ):
            raise ValueError("Invalid raw camera group")
        images = {}
        for view, fd in zip(views, fds, strict=True):
            if storage == "udmabuf":
                serial, stride, offset = view
            else:
                serial, stride = view
                offset = 0
            if serial not in cameras or serial in images:
                raise ValueError("Unknown or repeated raw camera")
            images[serial] = pixels(
                fd,
                cameras[serial].shape,
                stride,
                stack,
                offset=offset,
                udmabuf=storage == "udmabuf",
            )
        events = consume(kind, t, images)
        images.clear()
    try:
        sock.sendall(json.dumps({"release": seq}).encode())
    except (ConnectionError, TimeoutError) as exc:
        raise EOFError("Raw camera feed disconnected") from exc
    return seq, events
