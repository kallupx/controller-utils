#pragma once

namespace ct::update {
inline constexpr char MaintenanceService[] =
    "[Unit]\n"
    "Description=Restore Controller Utils permissions\n"
    "After=systemd-udevd.service\n"
    "Before=user@1000.service\n"
    "RequiresMountsFor=/var/lib/controller-utils /var/lib/controller-utils-updates\n"
    "\n"
    "[Service]\n"
    "Type=oneshot\n"
    "ExecStart=/var/lib/controller-utils/controller-utils-updater --repair\n"
    "TimeoutStartSec=20\n"
    "UMask=0077\n"
    "NoNewPrivileges=yes\n"
    "ProtectHome=yes\n"
    "PrivateTmp=yes\n"
    "\n"
    "[Install]\n"
    "WantedBy=multi-user.target\n";
inline constexpr char MaintenanceKeep[] =
    "/etc/atomic-update.conf.d/controller-utils.conf\n"
    "/etc/sudoers.d/zz-controller-utils-updater\n"
    "/etc/udev/rules.d/70-controller-utils.rules\n"
    "/etc/systemd/system/controller-utils-maintenance.service\n"
    "/etc/systemd/system/multi-user.target.wants/controller-utils-maintenance.service\n";
} // namespace ct::update
