#pragma once
#include "runtime.h"
#include <ivrmailbox_001.h>
#include <array>
#include <functional>
#include <optional>
#include <string>

namespace ct {
namespace steamvr {
class Module;
}
bool validate_progress_table(const steamvr::Module &module, const uintptr_t *table,
                             size_t target_size, const char *target_signature);
enum class ShortcutAction { Recenter, Passthrough, Screenshot };
struct ShortcutProgress {
    ShortcutAction action;
    double amount;
};

class ShortcutBackend {
  public:
    virtual ~ShortcutBackend() = default;
    virtual void recenter() = 0;
    virtual void toggle_room_view() = 0;
    virtual void prepare_progress() = 0;
    virtual void post_progress(const ShortcutProgress &event) = 0;
    virtual void post_event(const vr::VREvent_t &event) = 0;
};

class OpenVrShortcutBackend final : public ShortcutBackend {
    Runtime &runtime_;
    vr::IVRMailbox *mailbox_ = nullptr;
    uint64_t handle_ = 0;
    void *client_ = nullptr;
    void (*post_)(void *, const vr::VREvent_t *, uint32_t) = nullptr;
    std::array<uint64_t, 4> progress_paths_{};
    void prepare_events();
    void send_room_view(const char *message);

  public:
    explicit OpenVrShortcutBackend(Runtime &runtime) : runtime_(runtime) {}
    OpenVrShortcutBackend(const OpenVrShortcutBackend &) = delete;
    OpenVrShortcutBackend &operator=(const OpenVrShortcutBackend &) = delete;
    ~OpenVrShortcutBackend() override;
    void recenter() override;
    void toggle_room_view() override;
    void prepare_progress() override;
    void post_progress(const ShortcutProgress &event) override;
    void post_event(const vr::VREvent_t &event) override;
};

class NativeShortcuts {
    ShortcutBackend &backend_;
    std::function<double()> monotonic_;
    bool closed_ = false;
    bool progress_ready_ = false;
    std::optional<ShortcutAction> progress_action_;
    double progress_last_ = 0;

  public:
    explicit NativeShortcuts(ShortcutBackend &backend, std::function<double()> monotonic = {});
    NativeShortcuts(const NativeShortcuts &) = delete;
    NativeShortcuts &operator=(const NativeShortcuts &) = delete;
    ~NativeShortcuts();
    void send_action(ShortcutAction action);
    void progress(std::optional<ShortcutAction> action, double amount = 0);
    void close();
};
} // namespace ct
