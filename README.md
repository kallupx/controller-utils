# Controller Utils

Adds support for tracking the 2026 Steam Controller on Steam Frame over Bluetooth.
It uses Frame's native tracking services to track position through the controller's
IR LED, and the controller's IMU for orientation.

The app displays a 3D controller model in VR. You can navigate Steam's UI with a
laser pointer or touchpad cursor, and type with both touchpads. Steam Controller
tracking also works while Frame's own controllers are being tracked.

![Controller Utils running on Steam Frame](docs/controller-utils.png)

Tracking relies on a single IR LED. It can become unstable or drift when the LED
is hidden from the headset's cameras. Position tracking recovers when you hold
the controller with the LED visible to the headset again.

The app supports the latest stable and beta SteamOS releases.
Since the driver interfaces directly with Frame's native tracking services,
SteamOS and SteamVR updates can break tracking and require an app update. I'll try
to maintain this at least until Valve releases official tracking support for the
Steam Controller.

The app is experimental.

## Install

Open a terminal on Frame and run:

```sh
curl -fsSL https://raw.githubusercontent.com/kallupx/controller-utils/main/packaging/bootstrap.py | python3 -
```

The script detects your SteamOS channel, downloads the latest release, verifies
it and installs the app.
Installation asks for confirmation and your password, then briefly restarts
SteamVR. Set a password with `passwd` first if you haven't already.

Open Controller Utils from Frame's app drawer (+). Pair your gamepad with Frame,
leave Enable tracking on and follow the headset prompts.

The app starts with SteamVR after its first launch. Change this in App settings.
For an existing installation, use updates or uninstall before installing again.

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

Enable Automatic updates in App settings to download signed GitHub releases.
For a manual check, select Check now. Updates install when SteamVR next starts. Later updates
and permission repairs run without another password prompt.

To uninstall, run:

```sh
~/.local/share/controller-utils/app/uninstall.sh
```

If an older uninstall stopped partway through, run the latest signed uninstaller:

```sh
curl -fsSL https://raw.githubusercontent.com/kallupx/controller-utils/main/packaging/bootstrap.py | python3 - --uninstall
```

Removal asks for administrator access. Updates and removal keep settings and
per-controller calibration in `~/.local/share/controller-utils/app-data`.

For installations using the old `controllertracking` directory, uninstall with
`~/.local/share/controllertracking/app/uninstall.sh`, then run the install command
above. The installer moves the saved settings and calibration to the new directory.

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
