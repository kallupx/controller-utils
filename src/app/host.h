#pragma once
#include "application.h"
#include "instance.h"
#include "registration.h"
#include "controller/reader.h"
#include "steamvr/shortcuts.h"
#include "tracking/backend.h"
#include "update/client.h"
#include "ui/presentation.h"

namespace ct {
class NativeApplicationHost final : public ApplicationHost {
    Runtime &runtime_;
    ControllerReader &reader_;
    const ApplicationLock &lock_;
    ui::Presentation &presentation_;
    std::filesystem::path root_, data_;
    ChildFactory children_;
    SteamUiFactory steam_ui_;
    std::function<void()> repair_verifier_;
    std::optional<TrackingBackend> backend_;
    std::optional<std::string> backend_problem_;
    ApplicationRegistration registration_;
    std::unique_ptr<SteamUiSession> keyboard_;
    std::unique_ptr<SteamUiSession> system_input_;
    std::unique_ptr<ChildChannel> camera_switch_;
    std::string system_serial_;
    uint32_t system_tap_ = 0;
    std::optional<double> system_confirmed_;
    std::unique_ptr<OpenVrShortcutBackend> shortcut_backend_;
    std::unique_ptr<NativeShortcuts> shortcuts_;
    update::UpdateClient updates_;
    std::unique_ptr<ChildChannel> update_confirmation_;
    std::unique_ptr<ChildChannel> repair_;
    double repair_started_ = 0;
    std::string repair_status_ = "Checking helper permissions.";
    bool repair_inspection_ = false;
    std::optional<std::string> inspection_result_;
    bool closed_ = false, recovery_mode_ = false;
    NativeShortcuts &shortcuts();
    void start_permission_action(bool inspection);
    void verify_permission_updater();

  public:
    NativeApplicationHost(Runtime &runtime, ControllerReader &reader, const ApplicationLock &lock,
                          ui::Presentation &presentation, std::filesystem::path root,
                          std::filesystem::path data, ChildFactory children = {},
                          SteamUiFactory steam_ui = {}, std::function<void()> repair_verifier = {});
    ~NativeApplicationHost() override;
    const std::optional<std::string> &tracking_problem() const {
        return backend_problem_;
    }
    bool running();
    void send(json::Value message) override;
    std::vector<json::Value> events() override;
    void save(const Settings &options) override;
    void autostart(bool enabled) override;
    std::unique_ptr<TrackingSessionView> track(const Settings &options) override;
    void shortcut(const std::string &action) override;
    std::optional<std::string> camera_switch_error(bool enabled) override;
    bool system_input(bool enabled) override;
    void shortcut_progress(std::optional<double> amount) override;
    void request_keyboard() override;
    std::optional<std::string> keyboard_error() override;
    void check_updates() override;
    bool recovery_active() const override;
    std::string recovery_message() const override;
    void retry_recovery() override;
    void open_uninstaller() override;
    json::Value updates(bool automatic) override;
    void confirm_update() override;
    void request_permission_repair() override;
    void request_permission_inspection() override;
    json::Value permission_status() override;
    std::string permission_repair_status();
    void log(const std::string &message) override;
    void close_keyboard();
    void close();
};
} // namespace ct
