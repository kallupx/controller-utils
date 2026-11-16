import json
import os
import shutil
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
BASE = Path("/home/steamos/.local/share/controller-utils")
PRIVILEGED = Path("/var/lib/controller-utils")
SERVICE = Path("/home/steamos/.local/share/systemd/user")


def run(*command, **kwargs):
    result = subprocess.run(command, capture_output=True, text=True, **kwargs)
    if result.returncode:
        raise RuntimeError(
            f"{command!r} exited {result.returncode}\n{result.stdout}\n{result.stderr}"
        )
    return result


def write(path, contents, mode=0o644):
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(contents)
    path.chmod(mode)


def bind(source, destination):
    run("mount", "--bind", str(source), str(destination))
    run("mount", "-o", "remount,bind,ro", str(destination))


def sandbox(case, directory, parent_namespace):
    if (
        os.geteuid() != 0
        or os.getpid() != 1
        or os.readlink("/proc/self/ns/mnt") == parent_namespace
    ):
        raise RuntimeError("Installer tests require isolated mount and PID namespaces")
    os.umask(0o022)
    root = Path(directory)
    run("mount", "--make-rprivate", "/")
    run("mount", "-t", "tmpfs", "-o", "size=128m,mode=0755", "tmpfs", str(root))
    (root / "usr").mkdir()
    bind("/usr", root / "usr")
    for name in ("bin", "sbin", "lib", "lib64"):
        source = Path("/") / name
        if source.is_symlink():
            (root / name).symlink_to(os.readlink(source))
        elif source.is_dir():
            (root / name).mkdir()
            bind(source, root / name)
    (root / "proc").mkdir()
    run("mount", "-t", "proc", "-o", "ro,nosuid,nodev,noexec", "proc", str(root / "proc"))
    (root / "dev").mkdir()
    (root / "dev/null").touch()
    bind("/dev/null", root / "dev/null")
    write(
        root / "etc/passwd",
        "root:x:0:0:root:/root:/bin/sh\nsteamos:x:1000:1000::/home/steamos:/bin/sh\n",
    )
    write(root / "etc/group", "root:x:0:\nsteamos:x:1000:\n")
    write(root / "etc/nsswitch.conf", "passwd: files\ngroup: files\nshadow: files\n")
    write(
        root / "etc/pam.d/runuser",
        "auth required pam_permit.so\naccount required pam_permit.so\nsession required pam_permit.so\n",
    )
    write(
        root / "etc/pam.d/sudo",
        "auth required pam_permit.so\naccount required pam_permit.so\nsession required pam_permit.so\n",
    )
    write(
        root / "etc/sudoers",
        "root ALL=(ALL:ALL) ALL\nsteamos ALL=(ALL:ALL) NOPASSWD: ALL\n@includedir /etc/sudoers.d\n",
        0o440,
    )
    (root / "etc/sudoers.d").mkdir()
    source = root / "opt/controller-utils"
    if case == "native-cycle":
        sanitized = os.environ.get("CONTROLLER_UTILS_INSTALLER_SANITIZE") == "1"
        binaries = "build/sanitize" if sanitized else "build"
        names = ("update_setup_test", "update_installer_test", "resilience_maintenance_repair")
        for name in names:
            target = source / name
            target.parent.mkdir(parents=True, exist_ok=True)
            target.touch()
            bind(ROOT / binaries / name, target)
        shutil.copytree(
            ROOT / "tests/fixtures/update-metadata", source / "tests/fixtures/update-metadata"
        )
        (root / "var/lib").mkdir(parents=True)
        os.chroot(root)
        os.chdir("/opt/controller-utils")
        outputs = [
            run(
                "./" + name,
                env={**os.environ, "CT_MAINTENANCE_ISOLATED": "1"},
            ).stdout
            for name in names
        ]
        print(json.dumps({"results": outputs}))
        return
    for name in ("install.sh", "uninstall.sh"):
        write(source / name, (ROOT / "packaging" / name).read_text(), 0o755)
    write(
        source / "packaging/controller-utils-update.service",
        (ROOT / "packaging/controller-utils-update.service").read_text(),
    )
    write(source / "controller-utils-launcher", "launcher fixture", 0o755)
    write(
        source / "controller-utils",
        '#!/bin/sh\nprintf \'%s %s\\n\' "$(id -u)" "$*" >> /home/steamos/registration.log\n',
        0o755,
    )
    if case in ("registration-failure", "partial-registration-failure"):
        write(source / "controller-utils", "#!/bin/sh\nexit 1\n", 0o755)
    write(source / "native-driver/driver.vrdrivermanifest", "driver fixture")
    write(source / "app-data-marker", "preserved")
    write(
        source / "compatibility/controller-utils-updater",
        """#!/usr/bin/python3
import os, pathlib, shutil, sys
with open('/trace/native.log', 'a') as log:
    log.write(str(os.getuid()) + ' ' + ' '.join(sys.argv[1:]) + '\\n')
if sys.argv[1] == '--stage':
    assert os.getuid() == 1000
    shutil.copytree('/opt/controller-utils', sys.argv[3])
elif sys.argv[1] == '--install':
    assert os.getuid() == 0
    root = pathlib.Path('/var/lib/controller-utils')
    assert root.stat().st_uid == 0 and not root.stat().st_mode & 0o022
    shutil.copyfile('/opt/controller-utils/compatibility/controller-utils-updater', root / 'controller-utils-updater')
    (root / 'controller-utils-updater').chmod(0o755)
elif sys.argv[1] in ('--repair', '--repair-status', '--apply', '--confirm'):
    assert os.getuid() == 0
elif sys.argv[1] == '--uninstall':
    assert os.getuid() == 0
    if pathlib.Path('/home/steamos/fail-uninstall').exists():
        sys.exit(1)
    shutil.rmtree('/var/lib/controller-utils')
else:
    raise ValueError(sys.argv)
""",
        0o755,
    )
    write(root / "trace/install.log", "", 0o666)
    write(root / "trace/native.log", "", 0o666)
    write(root / "test-bin/uname", "#!/bin/sh\nprintf '%s\\n' aarch64\n", 0o755)
    write(
        root / "test-bin/systemctl",
        """#!/bin/sh
printf '%s %s\\n' "$(id -u)" "$*" >> /home/steamos/systemctl.log
case "$*" in
    '--user is-active --quiet steamvr.service') [ ! -e /home/steamos/steamvr.inactive ] ;;
    '--user show steamvr.service -p LoadState --value')
        if [ -e /home/steamos/steamvr.masked ]; then printf '%s\\n' masked; else printf '%s\\n' loaded; fi ;;
    '--user mask --runtime steamvr.service') touch /home/steamos/steamvr.masked ;;
    '--user stop steamvr.service') [ -e /home/steamos/steamvr.masked ] || exit 1 ;;
    '--user unmask --runtime steamvr.service') rm /home/steamos/steamvr.masked ;;
    '--user start steamvr.service') [ ! -e /home/steamos/steamvr.masked ] || exit 1 ;;
esac
""",
        0o755,
    )
    write(root / "test-bin/udevadm", "#!/bin/sh\nexit 0\n", 0o755)
    shutil.copyfile("/usr/bin/sudo", root / "test-bin/sudo")
    (root / "test-bin/sudo").chmod(0o4755)
    shutil.copyfile("/usr/bin/install", root / "test-bin/install-real")
    (root / "test-bin/install-real").chmod(0o755)
    write(
        root / "test-bin/install",
        """#!/usr/bin/python3
import json, os, sys
path = sys.argv[-1]
with open('/trace/install.log', 'a') as log:
    log.write(json.dumps({'uid': os.getuid(), 'euid': os.geteuid(), 'path': path}) + '\\n')
if path.endswith('/controller-utils-update.service') and os.path.exists('/home/steamos/race'):
    directory = os.path.dirname(path)
    os.rmdir(directory)
    os.symlink('/var/lib/controller-utils', directory)
os.execv('/test-bin/install-real', ['install', *sys.argv[1:]])
""",
        0o755,
    )
    for name in ("uname", "systemctl", "install", "sudo", "udevadm"):
        bind(root / "test-bin" / name, root / "usr/bin" / name)
    write(
        root / "opt/steamvr/bin/linuxarm64/vrpathreg",
        '#!/bin/sh\nprintf \'%s %s\\n\' "$(id -u)" "$*" >> /home/steamos/driver.log\n',
        0o755,
    )
    write(
        root / PRIVILEGED.relative_to("/") / "controller-utils-updater", "previous updater\n", 0o755
    )
    home = root / "home/steamos"
    write(root / BASE.relative_to("/") / "app-data/app.lock", "", 0o600)
    write(root / BASE.relative_to("/") / "app-data/settings.json", "preserved settings", 0o600)
    write(
        root / BASE.relative_to("/") / "app-data/calibrations/gamepad.json",
        "preserved calibration",
        0o600,
    )
    legacy = root / BASE.with_name("controllertracking").relative_to("/")
    if case == "legacy-installed":
        write(legacy / "app/uninstall.sh", "previous uninstaller", 0o755)
    elif case in ("legacy-data", "legacy-data-conflict", "legacy-data-link"):
        legacy.mkdir(parents=True)
        if case == "legacy-data":
            (root / BASE.relative_to("/") / "app-data").rename(legacy / "app-data")
        elif case == "legacy-data-conflict":
            write(legacy / "app-data/settings.json", "previous settings", 0o600)
        else:
            (legacy / "app-data").symlink_to(BASE / "app-data", target_is_directory=True)
    (root / SERVICE.relative_to("/")).mkdir(parents=True)
    for path in (home, *home.rglob("*")):
        os.chown(path, 1000, 1000, follow_symlinks=False)
    write(
        root / "trace/production.sudoers",
        (ROOT / "packaging/controller-utils-updater.sudoers").read_text(),
        0o440,
    )
    os.chroot(root)
    os.chdir("/")
    if case == "registration-failure":
        (PRIVILEGED / "controller-utils-updater").unlink()
    if case == "service-link":
        SERVICE.rmdir()
        SERVICE.symlink_to(PRIVILEGED, target_is_directory=True)
    elif case == "ancestor-link":
        SERVICE.rmdir()
        SERVICE.parent.rmdir()
        SERVICE.parent.symlink_to(PRIVILEGED, target_is_directory=True)
    elif case == "service-file-link":
        (SERVICE / "controller-utils-update.service").symlink_to(
            PRIVILEGED / "controller-utils-updater"
        )
    elif case == "lock-link":
        (BASE / "app-data/app.lock").unlink()
        (BASE / "app-data/app.lock").symlink_to(PRIVILEGED / "controller-utils-updater")
    elif case == "race":
        write(Path("/home/steamos/race"), "swap before use\n")
    elif case == "unsafe-root-owner":
        os.chown(PRIVILEGED, 1000, 1000)
    elif case == "unsafe-root-mode":
        PRIVILEGED.chmod(0o777)
    elif case == "masked-runtime":
        write(Path("/home/steamos/steamvr.masked"), "preexisting mask")
    elif case == "inactive-runtime":
        write(Path("/home/steamos/steamvr.inactive"), "inactive")
    user_command = [
        shutil.which("runuser"),
        "-u",
        "steamos",
        "--",
        "/usr/bin/env",
        "HOME=/home/steamos",
    ]
    command = [
        *user_command,
        "/bin/sh",
        "/opt/controller-utils/install.sh",
        "/opt/controller-utils",
    ]
    result = subprocess.run(command, capture_output=True, text=True)
    repeated = None
    removed = None
    authorization = []
    cleanup_status = None
    updater = PRIVILEGED / "controller-utils-updater"
    installed = updater.read_text() if updater.exists() else ""
    root_owner = PRIVILEGED.stat().st_uid if PRIVILEGED.exists() else None
    root_mode = PRIVILEGED.stat().st_mode & 0o777 if PRIVILEGED.exists() else None
    attempted = subprocess.run(
        [
            *user_command,
            "/usr/bin/python3",
            "-c",
            "from pathlib import Path; p=Path('/var/lib/controller-utils/controller-utils-updater'); p.unlink(); p.write_text('replacement')",
        ],
        capture_output=True,
        text=True,
    )
    if case in ("normal", "remove-failure", "legacy-data") and result.returncode == 0:
        repeated = subprocess.run(command, capture_output=True, text=True).returncode
        write(BASE / "app-data/startup-recovery", "disabled recovery")
        if case == "remove-failure":
            write(Path("/home/steamos/fail-uninstall"), "fail")
        removed = subprocess.run(
            [*user_command, "/bin/sh", str(BASE / "app/uninstall.sh")],
            capture_output=True,
            text=True,
        )
    if case == "partial-registration-failure" and result.returncode != 0:
        cleanup_status = subprocess.run(
            [
                *user_command,
                "/bin/sh",
                "/opt/controller-utils/install.sh",
                "--cleanup",
                "/opt/controller-utils",
            ],
            capture_output=True,
            text=True,
        ).returncode
    if case == "authorization" and result.returncode == 0:
        write(
            Path("/etc/sudoers"),
            "root ALL=(ALL:ALL) ALL\n@includedir /etc/sudoers.d\n",
            0o440,
        )
        write(
            Path("/etc/sudoers.d/zz-controller-utils-updater"),
            Path("/trace/production.sudoers").read_text(),
            0o440,
        )
        run("visudo", "-c")
        for arguments in (
            [str(updater), "--repair"],
            [str(updater), "--repair-status"],
            [str(updater), "--apply"],
            [str(updater), "--confirm"],
            [str(updater), "--repair", "untrusted"],
            [str(updater), "--install", "/untrusted"],
            [str(updater), "--uninstall"],
            ["/bin/sh", "-c", "true"],
        ):
            operation = subprocess.run(
                [*user_command, "/usr/bin/sudo", "-n", *arguments],
                capture_output=True,
                text=True,
            )
            authorization.append(operation.returncode)
    print(
        json.dumps(
            {
                "status": result.returncode,
                "cleanup_status": cleanup_status,
                "authorization": authorization,
                "stderr": result.stderr,
                "root_owner": root_owner,
                "root_mode": root_mode,
                "installed": installed,
                "replacement_denied": attempted.returncode != 0,
                "operations": [
                    json.loads(line) for line in Path("/trace/install.log").read_text().splitlines()
                ],
                "native": Path("/trace/native.log").read_text().splitlines(),
                "repeat_status": repeated,
                "remove_status": removed.returncode if removed else None,
                "remove_stderr": removed.stderr if removed else None,
                "settings": (BASE / "app-data/settings.json").read_text(),
                "calibration": (BASE / "app-data/calibrations/gamepad.json").read_text(),
                "recovery_exists": (BASE / "app-data/startup-recovery").exists(),
                "app_exists": (BASE / "app").exists(),
                "root_exists": PRIVILEGED.exists(),
                "service_exists": (SERVICE / "controller-utils-update.service").exists(),
                "systemctl": Path("/home/steamos/systemctl.log").read_text().splitlines()
                if Path("/home/steamos/systemctl.log").exists()
                else [],
                "steamvr_masked": Path("/home/steamos/steamvr.masked").exists(),
                "legacy_settings": (
                    BASE.with_name("controllertracking") / "app-data/settings.json"
                ).read_text()
                if (BASE.with_name("controllertracking") / "app-data/settings.json").is_file()
                else None,
            }
        )
    )


@unittest.skipUnless(sys.platform == "linux", "installer tests need Linux namespaces")
class InstallUpdatesTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        missing = [
            name for name in ("unshare", "mount", "runuser", "visudo") if not shutil.which(name)
        ]
        if missing:
            message = "Installer test tools are unavailable: " + ", ".join(missing)
            if os.geteuid() == 0:
                raise RuntimeError(message)
            raise unittest.SkipTest(message)
        cls.namespace = ["unshare", "--mount", "--pid", "--fork"]
        if os.geteuid() != 0:
            cls.namespace += ["--user", "--map-root-user", "--map-auto"]
        probe = subprocess.run([*cls.namespace, "true"], capture_output=True, text=True)
        if probe.returncode:
            if os.geteuid() == 0:
                raise RuntimeError(probe.stderr)
            raise unittest.SkipTest("unprivileged namespaces unavailable: " + probe.stderr.strip())

    def check_case(self, case):
        with tempfile.TemporaryDirectory(prefix="controller-utils-install-") as directory:
            result = run(
                *self.namespace,
                sys.executable,
                str(Path(__file__).resolve()),
                "--sandbox",
                case,
                directory,
                os.readlink("/proc/self/ns/mnt"),
            )
        return json.loads(result.stdout)

    def test_native_signed_install_and_update_rollback(self):
        result = self.check_case("native-cycle")
        self.assertEqual(len(result["results"]), 3)
        self.assertIn("removal passed", result["results"][0])
        self.assertIn("signature rejection passed", result["results"][1])
        self.assertIn("recovery and preservation passed", result["results"][2])

    def test_repair_and_signed_update_commands_need_no_further_authentication(self):
        result = self.check_case("authorization")
        self.assertEqual(result["status"], 0, result["stderr"])
        self.assertEqual(result["authorization"][:4], [0, 0, 0, 0])
        self.assertTrue(all(status != 0 for status in result["authorization"][4:]))
        self.assertTrue(result["replacement_denied"])
        for operation in ("--repair", "--repair-status", "--apply", "--confirm"):
            self.assertIn("0 " + operation, result["native"])

    def test_normal_setup_and_removal_keep_privileges_separate(self):
        result = self.check_case("normal")
        self.assertEqual(result["status"], 0, result["stderr"])
        self.assertEqual(result["root_owner"], 0)
        self.assertTrue(result["replacement_denied"])
        self.assertNotEqual(result["repeat_status"], 0)
        self.assertEqual(result["remove_status"], 0, result["remove_stderr"])
        self.assertEqual(result["settings"], "preserved settings")
        self.assertFalse(result["recovery_exists"])
        self.assertFalse(result["app_exists"] or result["root_exists"] or result["service_exists"])
        self.assertEqual(result["native"][0].split()[0], "1000")
        self.assertTrue(any(line.startswith("0 --install ") for line in result["native"]))
        self.assertIn("0 --uninstall", result["native"])
        for operation in result["operations"]:
            self.assertEqual(operation["uid"], 1000, operation)
            self.assertEqual(operation["euid"], 1000, operation)
        self.assertTrue(all(line.startswith("1000 ") for line in result["systemctl"]))
        self.assertFalse(result["steamvr_masked"])
        self.assertEqual(result["systemctl"].count("1000 --user mask --runtime steamvr.service"), 2)
        self.assertEqual(
            result["systemctl"].count("1000 --user unmask --runtime steamvr.service"), 2
        )

    def test_partial_native_setup_can_be_removed_without_installed_app(self):
        result = self.check_case("partial-registration-failure")
        self.assertNotEqual(result["status"], 0)
        self.assertEqual(result["cleanup_status"], 0, result["stderr"])
        self.assertFalse(result["root_exists"] or result["app_exists"] or result["service_exists"])
        self.assertEqual(result["settings"], "preserved settings")

    def test_previous_installation_requires_its_uninstaller(self):
        result = self.check_case("legacy-installed")
        self.assertNotEqual(result["status"], 0)
        self.assertIn("controllertracking/app/uninstall.sh", result["stderr"])
        self.assertEqual(result["native"], [])
        self.assertEqual(result["settings"], "preserved settings")

    def test_previous_settings_are_moved_and_survive_removal(self):
        result = self.check_case("legacy-data")
        self.assertEqual(result["status"], 0, result["stderr"])
        self.assertEqual(result["remove_status"], 0, result["remove_stderr"])
        self.assertEqual(result["settings"], "preserved settings")
        self.assertEqual(result["calibration"], "preserved calibration")
        self.assertIsNone(result["legacy_settings"])

    def test_previous_settings_conflicts_and_symlinks_are_preserved(self):
        for case in ("legacy-data-conflict", "legacy-data-link"):
            with self.subTest(case=case):
                result = self.check_case(case)
                self.assertNotEqual(result["status"], 0)
                self.assertEqual(result["native"], [])
                self.assertEqual(result["settings"], "preserved settings")
                self.assertEqual(
                    result["legacy_settings"],
                    "previous settings" if case == "legacy-data-conflict" else "preserved settings",
                )

    def test_failed_removal_restores_steamvr_startup(self):
        result = self.check_case("remove-failure")
        self.assertEqual(result["status"], 0, result["stderr"])
        self.assertNotEqual(result["remove_status"], 0)
        self.assertTrue(result["app_exists"] and result["root_exists"])
        self.assertFalse(result["steamvr_masked"])
        self.assertEqual(result["systemctl"][-1], "1000 --user start steamvr.service")

    def test_existing_steamvr_mask_is_preserved(self):
        result = self.check_case("masked-runtime")
        self.assertNotEqual(result["status"], 0)
        self.assertTrue(result["steamvr_masked"])
        self.assertNotIn("1000 --user unmask --runtime steamvr.service", result["systemctl"])
        self.assertFalse(any(line.startswith("0 --install ") for line in result["native"]))

    def test_inactive_steamvr_is_not_started_after_installation(self):
        result = self.check_case("inactive-runtime")
        self.assertEqual(result["status"], 0, result["stderr"])
        self.assertFalse(result["steamvr_masked"])
        self.assertNotIn("1000 --user start steamvr.service", result["systemctl"])

    def test_failed_registration_cleans_user_installation_and_keeps_settings(self):
        result = self.check_case("registration-failure")
        self.assertNotEqual(result["status"], 0)
        self.assertFalse(result["app_exists"] or result["service_exists"] or result["root_exists"])
        self.assertEqual(result["settings"], "preserved settings")
        self.assertIsNone(result["root_owner"])
        self.assertTrue(result["replacement_denied"])
        self.assertIn("1000 --user start steamvr.service", result["systemctl"])
        self.assertFalse(result["steamvr_masked"])

    def test_home_symlinks_cannot_transfer_privileged_directory(self):
        for case in ("service-link", "ancestor-link", "service-file-link", "lock-link", "race"):
            with self.subTest(case=case):
                result = self.check_case(case)
                self.assertEqual(result["root_owner"], 0, result)
                self.assertEqual(result["root_mode"], 0o755, result)
                self.assertTrue(result["replacement_denied"], result)
                self.assertNotEqual(result["installed"], "replacement", result)

    def test_unprotected_privileged_directory_is_rejected(self):
        for case in ("unsafe-root-owner", "unsafe-root-mode"):
            with self.subTest(case=case):
                result = self.check_case(case)
                self.assertNotEqual(result["status"], 0, result)
                self.assertEqual(result["installed"], "previous updater\n", result)


if __name__ == "__main__":
    if len(sys.argv) == 5 and sys.argv[1] == "--sandbox":
        sandbox(sys.argv[2], sys.argv[3], sys.argv[4])
    else:
        unittest.main()
