# Controller Utils

Adds support for tracking the 2026 Steam Controller on Steam Frame over Bluetooth.
It uses the Frame's cameras to track the controller's IR LED and the
controller's motion sensors to follow its movement.

The app displays a 3D controller model in VR. You can navigate Steam's UI with a
laser pointer or touchpad cursor, and type with both touchpads. Steam Controller
tracking also works while Frame's own controllers are being tracked.

![Controller Utils running on Steam Frame](docs/controller-utils.png)

Tracking relies on a single IR LED. It can become unstable or drift when the LED
is hidden from the headset's cameras. Position tracking recovers when you hold
the controller with the LED visible to the headset again.

The app supports SteamOS stable and beta. SteamOS and SteamVR updates can break
tracking and require an app update. I'll try to maintain this at least until
Valve releases official tracking support for the Steam Controller.

The app is experimental.

## Install

Open a terminal on Frame and run:

```sh
curl -fsSL https://raw.githubusercontent.com/kallupx/controller-utils/main/packaging/bootstrap.py | python3 -
```

The script detects your SteamOS channel, downloads the latest release, verifies
it and installs the app.
Installation asks for confirmation and may ask for your password, then briefly
restarts Steam and SteamVR. Set a password with `passwd` first if you haven't already.

Open Controller Utils from Frame's app drawer (+). Pair your gamepad with Frame,
leave Enable tracking on and follow the headset prompts.

The app starts with SteamVR after its first launch. Change this in App settings.

## Troubleshooting tracking

To send a debug log, open **Controller Utils → App**, turn on **Debug logging**,
then reproduce the problem. Turn logging off to save the report, or select
**Save log** while the issue is still happening. Attach the
`controller-utils-debug-*.txt` file from **Downloads** to your bug report.
You can open it first to see exactly what you're sharing.

If the app won't reopen after you enabled logging, you can still save the
report from Konsole:

```sh
python3 -IB ~/.local/share/controller-utils/app/optical/frame_debug.py save
```

Debug logging is off by default. Once enabled it stays on across app and headset
restarts. A report keeps the latest 1,500 app events. It includes app and
SteamOS versions, controller connection and tracking changes, camera frame
counts, SteamVR camera errors, and setup or update failures. Camera images,
tracking coordinates and typed text are excluded.
For a first installation that fails before the app can open, use the **Setup log**
path printed in Konsole.

If v0.3.0 stays at **Palm + LED** with the LED visible in passthrough, try
**SteamVR Settings → Camera → Anti-Flicker → 50Hz**. The released v0.3.0 misses
the images it needs to see your hands when Frame uses 60Hz camera timing.
After installing v0.3.1, you can restore your usual Anti-Flicker setting.

If tracking is waiting for camera images, put Frame on, leave the gamepad
connected and try Frame's own black-and-white passthrough. If passthrough or
head tracking also fails, restart the headset. If both work and Controller
Utils is still waiting, restart SteamVR. On v0.3.1, disconnecting the Arcturus
Vision Camera can leave Controller Utils without images from the two front
tracking cameras even while Frame's own tracking works. In a bug report,
include whether Arcturus was connected, whether passthrough worked, the SteamOS
channel, Anti-Flicker setting and time of the error. Frame's camera service
writes its current log to `~/.local/share/Steam/logs/xrservice.txt`. You can
read the app's SteamVR journal for the current boot in Konsole with
`journalctl --user -b -u steamvr.service --since '10 minutes ago'`.

If setup reports missing SteamOS camera devices, restart Frame before using
tracking. During v0.3.1 setup, **GPU device discovery failed** can appear even when the
CPU palm model loads successfully. Look for **Controller Utils installed** at
the end of setup. If setup failed, check the `Setup log`
path printed by the install command. If another setup is running, let it finish
before starting one again.

## Controls

| Input | Action |
| --- | --- |
| R4 | Toggle UI control, configurable in Controls |
| Right pad | Move the cursor, release to return to the laser |
| Right trigger or right pad click | Click |
| Left trigger | Right-click |
| Right stick or left pad | Scroll |
| View + Menu, hold 1 second then release | Realign heading |
| View + Menu, hold 5 seconds | Restart tracking |
| Steam + X | Open the keyboard |
| Steam + R1 | Take a screenshot |
| Steam + L1 | Switch between the Frame and Arcturus passthrough cameras |
| Steam, hold 1 second then release | Recenter the view |
| Steam, double press | Toggle passthrough |

Steam shortcuts are enabled by default. Change this in Controls.
Camera switching requires an attached Arcturus Vision Camera and Camera Control
running from the Arcturus Vision Camera app. Choose a different UI control
binding if you use L1 for it.

With UI control on, look at the keyboard and touch either pad to type. Both pads
control the typing cursors. Typing stays active while you look away. Release both
pads to aim again.

## Updates and removal

Enable **Automatic updates** in App settings to download signed updates and
install them while the Frame is idle. To update manually, select
**Check for updates**, then **Install update** when it appears. If an older
version can't update in the app, run the install command above.

To uninstall, choose **Exit app**, then run:

```sh
curl -fsSL https://raw.githubusercontent.com/kallupx/controller-utils/main/packaging/bootstrap.py | python3 - --uninstall
```

Removal asks for administrator access. Updates and removal keep settings and
per-controller calibration in `~/.local/share/controller-utils/app-data`.

For installations using the old `controllertracking` directory, uninstall with
`~/.local/share/controllertracking/app/uninstall.sh`.

## Build

The [CI workflow](.github/workflows/checks.yml) lists the required compilers,
libraries and tools. With those installed:

```sh
python3 -m venv .venv
.venv/bin/pip install -r requirements-dev.txt
export PATH="$PWD/.venv/bin:$PATH"
make -j4 all
make check-format
make check
make -j4 check-sanitize
```

Desktop builds support automated tests. Headset binaries require ARM64 and
Frame-compatible system libraries.

## License

Code is [MIT licensed](LICENSE). The bundled Valve controller model is
CC BY-NC-SA 4.0, so the complete bundle has noncommercial artwork restrictions.
Textures are CC BY 4.0 and the Kenney icon is CC0. See [artwork credits](assets/README.md)
and the [OpenVR license](third_party/openvr/LICENSE).

This project is not affiliated with Valve.
