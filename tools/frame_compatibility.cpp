#include "steamvr/ui_gate.h"
#include "platform/linux/files.h"
#include "steamvr/compatibility.h"
#include <iostream>

int main(int argc, char **argv) {
    using ct::steamvr::GateKind;
    using namespace ct::steamvr::compatibility;
    if (argc != 1 && argc != 3)
        return 2;
    for (const auto &[kind, path] :
         {std::pair{GateKind::Beam, argc == 3 ? argv[1] : Compositor.path},
          std::pair{GateKind::Input, argc == 3 ? argv[2] : SteamWebHelper.path}}) {
        try {
            ct::steamvr::resolve_gate_layout(kind, ct::read_file(path, 32 * 1024 * 1024));
        } catch (const std::exception &error) {
            std::cerr << path << ": " << error.what() << '\n';
            return kind == GateKind::Input ? 3 : 1;
        }
    }
    return 0;
}
