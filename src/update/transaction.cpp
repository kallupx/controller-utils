#include "transaction.h"
#include "directories.h"
#include "archive.h"
#include "app/settings.h"
#include "platform/linux/files.h"
#include <algorithm>
#include <cerrno>
#include <system_error>
#include <fcntl.h>
#include <linux/fs.h>

namespace ct::update {
namespace {
void check(int value, const char *operation) {
    if (value < 0)
        throw std::system_error(errno, std::generic_category(), operation);
}
bool contains(const std::filesystem::path &parent, const std::filesystem::path &path) {
    auto a = parent.begin(), b = path.begin();
    while (a != parent.end() && b != path.end() && *a == *b) {
        ++a;
        ++b;
    }
    return a == parent.end();
}
struct Journal {
    std::string phase;
    Release previous, next;
    DirectoryIdentity old_directory, new_directory;
    bool settings_present;
    json::Value to_json() const {
        json::Value value(json_object_new_object());
        json_object_object_add(value.get(), "schema", json_object_new_int(1));
        json_object_object_add(value.get(), "phase", json_object_new_string(phase.c_str()));
        json_object_object_add(value.get(), "previous", previous.to_json().release());
        json_object_object_add(value.get(), "next", next.to_json().release());
        json_object_object_add(value.get(), "old_directory", old_directory.to_json().release());
        json_object_object_add(value.get(), "new_directory", new_directory.to_json().release());
        json_object_object_add(value.get(), "settings_present",
                               json_object_new_boolean(settings_present));
        return value;
    }
    static Journal from_json(json_object *value) {
        if (!value || json_object_get_type(value) != json_type_object ||
            json_object_object_length(value) != 7 ||
            json::integer(json::field(value, "schema")) != 1)
            throw std::runtime_error("Preserve the changed update recovery record for review");
        Journal result{json::string(json::field(value, "phase")),
                       Release::from_json(json::field(value, "previous")),
                       Release::from_json(json::field(value, "next")),
                       DirectoryIdentity::from_json(json::field(value, "old_directory")),
                       DirectoryIdentity::from_json(json::field(value, "new_directory")),
                       json::boolean(json::field(value, "settings_present"))};
        if ((result.phase != "prepared" && result.phase != "swapped" &&
             result.phase != "healthy") ||
            result.next.sequence <= result.previous.sequence ||
            result.old_directory == result.new_directory ||
            result.old_directory.device != result.new_directory.device)
            throw std::runtime_error("Preserve the changed update recovery record for review");
        return result;
    }
};
std::optional<Journal> journal(const ProtectedDirectory &cache) {
    const auto bytes = read_record(cache, "transaction.json");
    if (!bytes)
        return std::nullopt;
    auto value = json::parse(*bytes, MaxMetadata);
    return Journal::from_json(value.get());
}
void save_journal(const ProtectedDirectory &cache, const Journal &value) {
    auto encoded = value.to_json();
    write_record(cache, "transaction.json", json::encode(encoded.get(), MaxMetadata));
}
void application_lease(const ProtectedDirectory &data, const ApplicationLock &lock) {
    if (!lock.primary())
        throw std::logic_error("Application updates require the exclusive application lock");
    data.verify_file("app.lock", lock.descriptor());
}
void copy_record(const ProtectedDirectory &source, const std::string &name,
                 const ProtectedDirectory &destination, size_t limit, bool required) {
    const auto contents = read_record(source, name, limit);
    if (!contents && required)
        throw std::runtime_error("Required private controller calibration is missing");
    if (contents)
        write_record(destination, name, *contents, 0644);
}
} // namespace
Transaction::Transaction(std::filesystem::path application, const std::filesystem::path &data,
                         const ApplicationLock &application_lock,
                         const MaintenanceLock &maintenance,
                         std::function<InstalledRequirements()> installed)
    : application_(std::move(application)), parent_(application_.parent_path(), getuid()),
      data_(data, getuid()), application_lock_(application_lock), maintenance_(maintenance),
      installed_(std::move(installed)) {
    if (!application_.is_absolute() || application_.lexically_normal() != application_ ||
        application_.filename().empty() || application_.string().find('\0') != std::string::npos ||
        contains(application_, data) || contains(data, application_) ||
        maintenance_.cache().path() != data / "updates" || !installed_)
        throw std::invalid_argument("Invalid application update layout");
    verify();
}
void Transaction::verify() const {
    parent_.verify();
    application_lease(data_, application_lock_);
    maintenance_.verify();
}
void Transaction::recover(bool force) {
    verify();
    const auto &cache = maintenance_.cache();
    const auto record = journal(cache);
    if (!record)
        return;
    auto current = directory_identity(parent_, application_.filename());
    auto backup = directory_identity(cache, "previous-app");
    if (!force && record->phase == "healthy" && current && *current == record->new_directory)
        return;
    if (current && !(*current == record->old_directory) && !(*current == record->new_directory))
        throw std::runtime_error("Preserve the changed application for update recovery review");
    if (backup && !(*backup == record->old_directory) && !(*backup == record->new_directory))
        throw std::runtime_error("Preserve the changed update backup for review");
    if (!current || !(*current == record->old_directory)) {
        if (!backup || !(*backup == record->old_directory) ||
            !(release(cache.path() / "previous-app") == record->previous))
            throw std::runtime_error("Verified previous application is unavailable for recovery");
        move_directory(cache, "previous-app", parent_, application_.filename(),
                       current ? RENAME_EXCHANGE : RENAME_NOREPLACE);
    }
    if (directory_identity(parent_, application_.filename()) != record->old_directory ||
        !(release(application_) == record->previous))
        throw std::runtime_error("Previous application could not be verified after recovery");
    if (record->phase != "prepared" && record->settings_present) {
        const auto snapshot = read_record(cache, "settings.before", 131072);
        if (!snapshot)
            throw std::runtime_error("Saved settings are unavailable for update recovery");
        const auto value = json::parse(*snapshot);
        Settings::from_json(value.get());
        write_record(data_, "settings.json", *snapshot);
    } else if (record->phase != "prepared")
        erase_record(data_, "settings.json");
    auto history = read_history(cache);
    history.highest_sequence = std::max(history.highest_sequence, record->next.sequence);
    history.failed_sequence = record->next.sequence;
    history.manual_sequence = 0;
    history.message = "An update could not start. The previous version was restored.";
    save_history(cache, history);
    remove_directory(cache, "previous-app", record->new_directory);
    remove_directory(cache, "staging", record->new_directory);
    erase_record(cache, "transaction.json");
    erase_record(cache, "settings.before");
}
bool Transaction::activate() {
    verify();
    recover();
    const auto &cache = maintenance_.cache();
    if (!directory_identity(cache, "pending"))
        return false;
    ProtectedDirectory current(application_, getuid()), pending(cache.path() / "pending", getuid());
    const auto version = release(application_);
    auto history = read_history(cache);
    const auto metadata =
        verify_metadata(pending.path() / "latest.json", pending.path() / "latest.sig",
                        application_ / "assets/update-public.pem",
                        std::max(version.sequence, history.highest_sequence));
    if (metadata.release.sequence <= version.sequence)
        return false;
    const auto saved = read_record(data_, "settings.json", 131072);
    auto settings = Settings{};
    if (saved) {
        const auto value = json::parse(*saved);
        settings = Settings::from_json(value.get());
    }
    if ((!settings.automatic_updates || history.failed_sequence == metadata.release.sequence) &&
        history.manual_sequence != metadata.release.sequence)
        return false;
    check_requirements(metadata.requirements.get(), installed_());
    struct stat live{}, cached{};
    check(fstat(current.descriptor(), &live), "inspect application update filesystem");
    check(fstat(cache.descriptor(), &cached), "inspect application update cache filesystem");
    if (live.st_dev != cached.st_dev)
        throw std::runtime_error("App updates require a regular directory on the same filesystem");
    const auto previous_record = journal(cache);
    const auto backup = directory_identity(cache, "previous-app");
    if (backup && (!previous_record || previous_record->phase != "healthy" ||
                   !(*backup == previous_record->old_directory) ||
                   !(DirectoryIdentity{uint64_t(live.st_dev), uint64_t(live.st_ino)} ==
                     previous_record->new_directory)))
        throw std::runtime_error("Preserve the existing update backup for review");
    remove_directory(cache, "staging");
    unpack(pending.path() / metadata.artifact, cache.path() / "staging", metadata);
    ProtectedDirectory stage(cache.path() / "staging", getuid());
    if (read_file(application_ / "assets/update-public.pem", 16384) !=
            read_file(stage.path() / "assets/update-public.pem", 16384) ||
        channel(application_) != channel(stage.path()))
        throw std::runtime_error("Update cannot change its signing key or release channel");
    copy_record(current, "controller-utils.vrmanifest", stage, MaxMetadata, false);
    check_requirements(metadata.requirements.get(), installed_());
    current.verify();
    stage.verify();
    verify();
    if (backup)
        remove_directory(cache, "previous-app", previous_record->old_directory);
    if (saved)
        write_record(cache, "settings.before", *saved);
    else
        erase_record(cache, "settings.before");
    Journal record{"prepared",
                   version,
                   metadata.release,
                   {uint64_t(live.st_dev), uint64_t(live.st_ino)},
                   *directory_identity(cache, "staging"),
                   bool(saved)};
    try {
        save_journal(cache, record);
        move_directory(cache, "staging", cache, "previous-app", RENAME_NOREPLACE);
        move_directory(parent_, application_.filename(), cache, "previous-app", RENAME_EXCHANGE);
        record.phase = "swapped";
        save_journal(cache, record);
        history.highest_sequence = std::max(history.highest_sequence, metadata.release.sequence);
        history.downloaded_sequence = metadata.release.sequence;
        history.message = "Version " + metadata.release.version + " installed.";
        save_history(cache, history);
    } catch (...) {
        const auto failure = std::current_exception();
        recover();
        std::rethrow_exception(failure);
    }
    return true;
}
void confirm(const std::filesystem::path &application, const std::filesystem::path &data,
             const ApplicationLock &application_lock) {
    ProtectedDirectory directory(data, getuid());
    application_lease(directory, application_lock);
    if (!directory_identity(directory, "updates"))
        return;
    ProtectedDirectory cache(data / "updates", getuid());
    auto record = journal(cache);
    if (!record || record->phase != "swapped")
        return;
    ProtectedDirectory parent(application.parent_path(), getuid());
    if (directory_identity(parent, application.filename()) != record->new_directory ||
        !(release(application) == record->next))
        throw std::runtime_error("Installed release does not match the startup confirmation");
    record->phase = "healthy";
    save_journal(cache, *record);
}
} // namespace ct::update
