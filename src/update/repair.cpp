#include "repair.h"
#include "platform/linux/files.h"
#include <cerrno>
#include <fcntl.h>
#include <linux/fs.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <system_error>
#include <unistd.h>

namespace ct::update {
namespace {
constexpr char Evidence[] = ".repair-evidence";
constexpr char Journal[] = "repair-journal";
void check(int result, const char *message) {
    if (result < 0)
        throw std::system_error(errno, std::generic_category(), message);
}
Metadata evidence(const std::string &record, const std::string &key) {
    if (record.size() <= 64 || record.size() > MaxMetadata + 64)
        throw std::invalid_argument("Invalid repair evidence");
    const auto bytes = record.substr(64);
    verify_signed_bytes(bytes, record.substr(0, 64), key);
    auto parsed = json::parse(bytes, MaxMetadata);
    return Metadata::from_json(parsed.get(), 0, 0);
}
void match_updater(const ProtectedDirectory &native, const Metadata &metadata) {
    const auto updater = native.file("controller-utils-updater", MaxArchive);
    struct stat info{};
    check(fstat(updater.get(), &info), "inspect installed updater");
    if ((info.st_mode & 07777) != 0755 ||
        sha256(updater.get()) !=
            json::string(json::field(json::field(metadata.requirements.get(), "helper"),
                                     "controller-utils-updater")))
        throw std::runtime_error("Installed updater does not match signed evidence");
    native.verify_file("controller-utils-updater", updater.get());
}
struct Policies {
    std::string sudoers, udev;
};
Policies records(const ProtectedDirectory &cache, const std::string &key) {
    const auto stored_key = read_record(cache, "public.pem", 16384);
    const auto receipt = read_record(cache, "setup.json", 16384);
    const auto sudoers = read_record(cache, "sudoers", 16384);
    const auto udev = read_record(cache, "udev.rules", 16384);
    if (!stored_key || *stored_key != key || !receipt || !sudoers || !udev || sudoers->empty() ||
        udev->empty() || sudoers->find('\0') != std::string::npos ||
        udev->find('\0') != std::string::npos)
        throw std::runtime_error("Installed policy records are unavailable");
    auto parsed = json::parse(*receipt, 16384);
    auto release_value = json::Value(json_object_new_object());
    for (const auto field : {"version", "sequence", "platform"})
        json_object_object_add(release_value.get(), field,
                               json_object_get(json::field(parsed.get(), field)));
    (void)Release::from_json(release_value.get());
    const auto digest = json::string(json::field(parsed.get(), "sha256"));
    if (digest.size() != 64 || digest.find_first_not_of("0123456789abcdef") != std::string::npos)
        throw std::runtime_error("Installed policy receipt is invalid");
    return {*sudoers, *udev};
}
int policy_state(const std::filesystem::path &path, const std::string &expected, mode_t mode) {
    ProtectedDirectory parent(path.parent_path());
    struct stat info{};
    if (fstatat(parent.descriptor(), path.filename().c_str(), &info, AT_SYMLINK_NOFOLLOW)) {
        if (errno == ENOENT)
            return 0;
        check(-1, "inspect policy");
    }
    if (!S_ISREG(info.st_mode) || info.st_uid != 0 || (info.st_mode & 07777) != mode ||
        info.st_nlink != 1)
        return -1;
    const auto bytes = read_record(parent, path.filename(), 16384);
    return bytes && *bytes == expected ? 1 : -1;
}
std::string pending_name(const std::filesystem::path &path) {
    return "." + path.filename().string() + ".controller-utils-repair.pending";
}
int pending_state(const std::filesystem::path &path, const std::string &expected, mode_t mode) {
    ProtectedDirectory parent(path.parent_path());
    const auto name = pending_name(path);
    struct stat info{};
    if (fstatat(parent.descriptor(), name.c_str(), &info, AT_SYMLINK_NOFOLLOW)) {
        if (errno == ENOENT)
            return 0;
        check(-1, "inspect repair staging file");
    }
    if (!S_ISREG(info.st_mode) || info.st_uid != 0 || (info.st_mode & 07777) != mode ||
        info.st_nlink != 1)
        return -1;
    const auto bytes = read_record(parent, name, 16384);
    return bytes && bytes->size() <= expected.size() &&
                   expected.compare(0, bytes->size(), *bytes) == 0
               ? 1
               : -1;
}
void create_policy(const std::filesystem::path &path, const std::string &bytes, mode_t mode) {
    ProtectedDirectory parent(path.parent_path());
    const auto pending = pending_name(path);
    FileDescriptor file(openat(parent.descriptor(), pending.c_str(),
                               O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, mode));
    check(file.get(), "create repair staging file");
    try {
        check(fchmod(file.get(), mode), "set repair policy mode");
        size_t offset = 0;
        while (offset < bytes.size()) {
            const auto n = write(file.get(), bytes.data() + offset, bytes.size() - offset);
            if (n < 0 && errno == EINTR)
                continue;
            if (n <= 0)
                throw std::system_error(n < 0 ? errno : EIO, std::generic_category(),
                                        "write repair policy");
            offset += size_t(n);
        }
        check(fsync(file.get()), "flush repair policy");
        parent.verify_file(pending, file.get());
        check(renameat2(parent.descriptor(), pending.c_str(), parent.descriptor(),
                        path.filename().c_str(), RENAME_NOREPLACE),
              "activate repair policy");
        check(fsync(parent.descriptor()), "flush repair directory");
    } catch (...) {
        struct stat opened{}, current{};
        if (!fstat(file.get(), &opened) &&
            !fstatat(parent.descriptor(), pending.c_str(), &current, AT_SYMLINK_NOFOLLOW) &&
            opened.st_dev == current.st_dev && opened.st_ino == current.st_ino)
            unlinkat(parent.descriptor(), pending.c_str(), 0);
        throw;
    }
}
void undo(const SetupPolicies &paths, const Policies &expected, const std::string &journal,
          const std::function<void()> &reload) {
    if (journal.size() != 2 || journal.find_first_not_of("01") != std::string::npos)
        throw std::runtime_error("Invalid repair journal");
    const std::filesystem::path destinations[] = {paths.sudoers, paths.udev};
    const std::string contents[] = {expected.sudoers, expected.udev};
    const mode_t modes[] = {0440, 0644};
    int status[2]{}, pending[2]{};
    for (int i = 0; i < 2; ++i) {
        if (journal[size_t(i)] != '1')
            continue;
        status[i] = policy_state(destinations[i], contents[i], modes[i]);
        pending[i] = pending_state(destinations[i], contents[i], modes[i]);
        if (status[i] < 0 || pending[i] < 0)
            throw std::runtime_error("Changed repair destination requires review");
    }
    for (int i = 0; i < 2; ++i) {
        if (journal[size_t(i)] != '1')
            continue;
        ProtectedDirectory parent(destinations[i].parent_path());
        if (pending[i])
            erase_record(parent, pending_name(destinations[i]));
        if (status[i])
            erase_record(parent, destinations[i].filename());
    }
    if (journal[1] == '1')
        reload();
}
} // namespace
void record_repair_evidence(const ProtectedDirectory &native, const std::string &metadata,
                            const std::string &signature, const std::string &trusted_key,
                            const Release &expected) {
    const auto record = signature + metadata;
    const auto parsed = evidence(record, trusted_key);
    if (!(parsed.release == expected))
        throw std::runtime_error("Repair evidence release mismatch");
    match_updater(native, parsed);
    write_record(native, Evidence, record, 0644);
}
Metadata verify_native_evidence(const ProtectedDirectory &native, const std::string &trusted_key) {
    const auto record = read_record(native, Evidence, MaxMetadata + 64);
    if (!record)
        throw std::runtime_error("Installed updater has no signed evidence");
    auto parsed = evidence(*record, trusted_key);
    match_updater(native, parsed);
    return parsed;
}
Release verify_repair_updater(const ProtectedDirectory &native, const std::string &trusted_key) {
    return verify_native_evidence(native, trusted_key).release;
}
RepairStatus inspect_repair(const UpdateInstallation &installation, const SetupPolicies &policies,
                            const std::string &trusted_key) {
    if (geteuid() != 0)
        return RepairStatus::NeedsAuthorization;
    try {
        ProtectedDirectory native(installation.components.helpers);
        (void)verify_repair_updater(native, trusted_key);
        ProtectedDirectory cache(installation.cache);
        FileDescriptor lock(
            openat(cache.descriptor(), "update.lock", O_RDONLY | O_NOFOLLOW | O_CLOEXEC));
        check(lock.get(), "open inspection lock");
        cache.verify_file("update.lock", lock.get());
        if (flock(lock.get(), LOCK_SH | LOCK_NB)) {
            if (errno == EWOULDBLOCK)
                return RepairStatus::Busy;
            check(-1, "lock permission inspection");
        }
        const auto expected = records(cache, trusted_key);
        if (read_record(cache, Journal, 2))
            return RepairStatus::Interrupted;
        try {
            const auto a = policy_state(policies.sudoers, expected.sudoers, 0440);
            const auto b = policy_state(policies.udev, expected.udev, 0644);
            if (a < 0 || b < 0)
                return RepairStatus::Conflict;
            return a && b ? RepairStatus::Intact : RepairStatus::Missing;
        } catch (const std::exception &) {
            return RepairStatus::Conflict;
        }
    } catch (const std::exception &) {
        return RepairStatus::ReinstallRequired;
    }
}
void repair_permissions(const UpdateInstallation &installation, const SetupPolicies &policies,
                        const std::string &trusted_key, const std::function<void()> &reload) {
    if (getuid() != 0 || geteuid() != 0 || installation.user != 1000 || !reload)
        throw std::runtime_error("Repair requires the authorized root updater");
    ProtectedDirectory cache(installation.cache);
    FileDescriptor lock(openat(cache.descriptor(), "update.lock", O_RDWR | O_NOFOLLOW | O_CLOEXEC));
    check(lock.get(), "open repair lock");
    cache.verify_file("update.lock", lock.get());
    check(flock(lock.get(), LOCK_EX | LOCK_NB), "lock repair");
    ProtectedDirectory native(installation.components.helpers);
    (void)verify_repair_updater(native, trusted_key);
    const auto expected = records(cache, trusted_key);
    if (const auto journal = read_record(cache, Journal, 2)) {
        undo(policies, expected, *journal, reload);
        erase_record(cache, Journal);
    }
    const auto a = policy_state(policies.sudoers, expected.sudoers, 0440);
    const auto b = policy_state(policies.udev, expected.udev, 0644);
    if (a < 0 || b < 0)
        throw std::runtime_error("Existing system policy requires review");
    if (a && b)
        return;
    if ((!a && pending_state(policies.sudoers, expected.sudoers, 0440) != 0) ||
        (!b && pending_state(policies.udev, expected.udev, 0644) != 0))
        throw std::runtime_error("Existing repair staging file requires review");
    const std::string journal{a ? '0' : '1', b ? '0' : '1'};
    write_record(cache, Journal, journal);
    try {
        if (!a)
            create_policy(policies.sudoers, expected.sudoers, 0440);
        if (!b)
            create_policy(policies.udev, expected.udev, 0644);
        if (!b)
            reload();
        if (policy_state(policies.sudoers, expected.sudoers, 0440) != 1 ||
            policy_state(policies.udev, expected.udev, 0644) != 1)
            throw std::runtime_error("Repaired policy could not be verified");
        erase_record(cache, Journal);
    } catch (...) {
        undo(policies, expected, journal, reload);
        erase_record(cache, Journal);
        throw;
    }
}
std::unique_ptr<ChildChannel> start_repair_action(const ChildFactory &children,
                                                  const std::function<void()> &verify) {
    if (!children || !verify)
        throw std::invalid_argument("Permission repair is unavailable");
    verify();
    auto child = children(
        {"/usr/bin/sudo", "-n", "/var/lib/controller-utils/controller-utils-updater", "--repair"});
    if (!child)
        throw std::runtime_error("Could not start permission repair");
    return child;
}
std::unique_ptr<ChildChannel> start_inspection_action(const ChildFactory &children,
                                                      const std::function<void()> &verify) {
    if (!children || !verify)
        throw std::invalid_argument("Permission inspection is unavailable");
    verify();
    auto child =
        children({"/usr/bin/sudo", "-n", "/var/lib/controller-utils/controller-utils-updater",
                  "--repair-status"});
    if (!child)
        throw std::runtime_error("Could not start permission inspection");
    return child;
}
} // namespace ct::update
