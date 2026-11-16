#include "compatibility_profile.h"
#include "native_queue_startup.h"
#include "update/profiles.h"
#include "update_public_key.h"
#include <chrono>
#include <iostream>
#include <pwd.h>
#include <unistd.h>

namespace ct::queue_startup {
bool initialize_compatible(ct::steamvr::BlockQueue *queue) {
    if (!queue)
        return false;
    try {
        if (getuid() != 1000 || geteuid() != 1000)
            return initialize(queue);
        const auto user = getpwuid(1000);
        if (!user || !user->pw_dir || !user->pw_name || std::strcmp(user->pw_name, "steamos"))
            return initialize(queue);
        const auto data =
            std::filesystem::path(user->pw_dir) / ".local/share/controller-utils/app-data/updates";
        const ProtectedDirectory cache(data, 1000);
        const auto now =
            std::chrono::duration<double>(std::chrono::system_clock::now().time_since_epoch())
                .count();
        const auto profile = update::load_profile(cache, UpdatePublicKey, now);
        const auto backend = installed_backend("/var/lib/controller-utils");
        if (!profile || profile->backend != backend)
            return initialize(queue);
        return initialize_with(
            queue,
            [&](ct::steamvr::BlockQueue *, const uintptr_t *table) {
                return validate_profile_queue(table, ct::steamvr::Module(table), *profile, backend);
            },
            retain_hook_module);
    } catch (const std::exception &failure) {
        std::cerr << "Controller Utils compatibility profile: " << failure.what() << '\n';
        return initialize(queue);
    }
}
} // namespace ct::queue_startup
