#pragma once
#include "platform/linux/child.h"

namespace ct {
class DebugLog {
    std::filesystem::path data_;
    std::string id_, notice_, last_state_;
    bool recording_ = false;
    std::unique_ptr<ChildChannel> job_;

  public:
    explicit DebugLog(const std::filesystem::path &data);
    bool recording() const {
        return recording_;
    }
    void event(const std::string &message) const;
    void state(const std::string &message);
    void action(bool save_only, const ChildFactory &children);
    json::Value info();
};
} // namespace ct
