#include "ui_gate.h"
#include "steamvr/compatibility.h"
#include "steamvr/code.h"
#include "steamvr/image.h"
#include <algorithm>
#include <array>
#include <cstring>
#include <unistd.h>
#include <limits>
#include <stdexcept>

namespace ct::steamvr {
namespace {
Bytes hex(const std::string &text) {
    if (text.size() % 2)
        throw std::logic_error("invalid compiled patch bytes");
    Bytes out;
    auto digit = [](char c) -> unsigned {
        if (c >= '0' && c <= '9')
            return unsigned(c - '0');
        if (c >= 'a' && c <= 'f')
            return unsigned(c - 'a' + 10);
        throw std::logic_error("invalid compiled patch bytes");
    };
    for (size_t i = 0; i < text.size(); i += 2)
        out.push_back(uint8_t(digit(text[i]) * 16 + digit(text[i + 1])));
    return out;
}
Bytes word(uint32_t value) {
    Bytes bytes(4);
    std::memcpy(bytes.data(), &value, 4);
    return bytes;
}
} // namespace
uint32_t aarch64_branch(uintptr_t source, uintptr_t destination) {
    if (source % 4 || destination % 4 ||
        (destination >= source ? destination - source >= (1u << 27)
                               : source - destination > (1u << 27)))
        throw std::invalid_argument("AArch64 branch target out of range");
    const int64_t delta =
        destination >= source ? int64_t(destination - source) : -int64_t(source - destination);
    return 0x14000000u | (uint32_t(delta / 4) & 0x3ffffffu);
}
GateLayout place_gate(GateKind kind, uintptr_t site, uintptr_t cave, unsigned beam_register) {
    if (site > UINTPTR_MAX - 12 || cave > UINTPTR_MAX - 60)
        throw std::invalid_argument("native gate address overflow");
    GateLayout layout;
    if (kind == GateKind::Beam)
        layout = {
            kind,
            site,
            cave,
            hex("8b1244bd"),
            hex("9609211e250a211e"),
            {},
            hex("f047bea910423bd5f00b00f98b1244bdf05f41b931010010310240b91f02116b41000054eb0327"
                "1ef00b40f910421bd5f047c2a84c3aec17ffffffff"),
            56,
            64};
    else if (kind == GateKind::Input)
        layout = {kind,
                  site,
                  cave,
                  hex("219040f9"),
                  hex("20003fd6772e40f9"),
                  {},
                  hex("219040f9b00640b9b10000181f02116b400000541bbf05141bbf0514ffffffff"),
                  28,
                  16};
    else
        throw std::invalid_argument("unknown native gate kind");
    if (beam_register != 11 && (kind != GateKind::Beam || beam_register != 19))
        throw std::invalid_argument("unsupported native beam register");
    if (beam_register == 19) {
        layout.original = hex("931244bd");
        layout.following = hex("8b09211e2a0a211e");
        std::copy(layout.original.begin(), layout.original.end(), layout.payload.begin() + 12);
        const auto clear = word(0x1e2703f3);
        std::copy(clear.begin(), clear.end(), layout.payload.begin() + 36);
    }
    layout.redirect = word(aarch64_branch(site, cave));
    auto relocate = [&](size_t offset, uintptr_t target) {
        const auto branch = aarch64_branch(cave + offset, site + target);
        std::memcpy(layout.payload.data() + offset, &branch, 4);
    };
    if (kind == GateKind::Input) {
        relocate(20, 4);
        relocate(24, 8);
    } else
        relocate(52, 4);
    return layout;
}
GateLayout resolve_gate_reviewed(GateKind kind, const std::string &bytes, const char *expected,
                                 unsigned beam_register = 11, const char *alternate = nullptr) {
    const auto prototype = place_gate(kind, 0, 0, beam_register);
    auto pattern = prototype.original;
    pattern.insert(pattern.end(), prototype.following.begin(), prototype.following.end());
    steamvr::Image image(bytes);
    const auto site = image.unique(pattern);
    const uintptr_t before = kind == GateKind::Input ? 44 : 64;
    if (site < before)
        throw std::runtime_error("native gate context is outside the image");
    if (!expected)
        throw std::invalid_argument("native gate signature is unavailable");
    const auto signature = steamvr::code_signature(image.at(site - before, 128, true), 128);
    if (signature != expected && (!alternate || signature != alternate))
        throw std::runtime_error("native gate context signature mismatch");
    const auto cave = image.padding(kind == GateKind::Input, prototype.payload.size());
    const auto page_size = sysconf(_SC_PAGESIZE);
    if (page_size <= 0)
        throw std::runtime_error("native page size is unavailable");
    const auto data = kind == GateKind::Input
                          ? image.at(cave, prototype.payload.size(), true)
                          : image.mapped_tail(cave, prototype.payload.size(), size_t(page_size));
    if (!std::all_of(data, data + prototype.payload.size(), [](char byte) { return !byte; }))
        throw std::runtime_error("native gate padding is occupied");
    return place_gate(kind, site, cave, beam_register);
}
GateLayout resolve_beam_reviewed(const std::string &bytes,
                                 const std::vector<BeamContract> &contracts) {
    steamvr::Image image(bytes);
    std::optional<GateLayout> result;
    for (const auto &contract : contracts) {
        const auto prototype = place_gate(GateKind::Beam, 0, 0, contract.beam_register);
        auto pattern = prototype.original;
        pattern.insert(pattern.end(), prototype.following.begin(), prototype.following.end());
        const auto found = image.find_unique(pattern);
        if (!found)
            continue;
        const auto site = *found;
        if (site < contract.before || !contract.signature || contract.size < 0x70 ||
            contract.size > 16384 || contract.size % 4 || contract.before > contract.size - 12)
            continue;
        const auto body = image.at(site - contract.before, contract.size, true);
        if (steamvr::code_signature(body, contract.size) != contract.signature)
            continue;
        if (result)
            throw std::runtime_error("ambiguous native beam contract");
        uint32_t frame, initialize, store;
        std::memcpy(&frame, body, 4);
        std::memcpy(&initialize, body + 0x68, 4);
        std::memcpy(&store, body + 0x6c, 4);
        const auto frame_size = (frame >> 10) & 0xfff;
        const auto slot = ((store >> 10) & 0xfff) * 4;
        if ((frame & 0xffc003ff) != 0xd10003ff || initialize != 0x12800000 ||
            (store & 0xffc003ff) != 0xb90003e0 || slot + 4 > frame_size || slot + 32 > 0x3ffc)
            throw std::runtime_error("native beam stack layout changed");
        result =
            resolve_gate_reviewed(GateKind::Beam, bytes, contract.context, contract.beam_register);
        const uint32_t load = 0xb94003f0 | (((slot + 32) / 4) << 10);
        std::memcpy(result->payload.data() + 16, &load, 4);
    }
    if (!result)
        throw UnsupportedBeamLayout();
    return *result;
}
GateLayout resolve_gate_layout(GateKind kind, const std::string &bytes) {
    if (kind == GateKind::Input)
        return resolve_gate_reviewed(
            kind, bytes, "790dac44ec743f59cb5f9d8498de276e4fb8969228ee2e205c7f416591e30db8", 11,
            "2249f7e44410741a5a5f3b7efe9fbdfa4e5b0f4201c8f7e0ecb517f0c4a000bc");
    if (kind != GateKind::Beam)
        throw std::invalid_argument("unknown native gate kind");
    return resolve_beam_reviewed(
        bytes, {{0xc30, 8248, "5b41ef23f2f75fcb08bf07951aab21198c1b2e930b6650d56195cc7390f72b98",
                 "0ac3c32eda0053d2026c612513a70f53f5fb152baf761e8bc8a38e34e4ddd3e9"},
                {0xc38, 8256, "31ba833270ee0fdba8acfdaadce9bd33ecbefc51e80035d822fd1c4a02ec2552",
                 "274ec840321f1525d5747ee8c3292852111e7d5059a13534f854542e50858de8"},
                {0xc38, 8256, "b78a9ee5aa4e18e1237ac6f8d292e024dae968e045dc4c15ceb51b139cbe428a",
                 "274ec840321f1525d5747ee8c3292852111e7d5059a13534f854542e50858de8"},
                {0xbcc, 8116, "5b095a997f7fc6d06c29da69988d62a17f7c94fd0882440a79c1d742e85422fb",
                 "d9018d435580f9b9ac0daba202718e8a9222866cb16e3c0144be1f8a4cb21e71", 19}});
}
void validate_gate_mappings(const GateLayout &layout, uintptr_t base,
                            const std::vector<MemoryMapping> &mappings) {
    const auto kind = layout.kind;
    const auto &binary = kind == GateKind::Input ? steamvr::compatibility::SteamWebHelper
                                                 : steamvr::compatibility::Compositor;
    const auto count = std::count_if(mappings.begin(), mappings.end(), [&](const auto &mapping) {
        return mapping.path == binary.path && !mapping.offset && mapping.readable &&
               mapping.executable && !mapping.writable && mapping.private_mapping &&
               mapping.first == base;
    });
    if (count != 1)
        throw std::runtime_error("expected one executable zero-offset mapping");
    if (base > UINTPTR_MAX - std::max(layout.site + 12, layout.cave + layout.payload.size()))
        throw std::invalid_argument("native gate address overflow");
    for (const auto &range : std::array<std::pair<uintptr_t, size_t>, 2>{
             {{layout.site, 12}, {layout.cave, layout.payload.size()}}})
        if (!std::any_of(mappings.begin(), mappings.end(), [&](const auto &mapping) {
                return mapping.path == binary.path && mapping.readable && mapping.executable &&
                       !mapping.writable && mapping.private_mapping &&
                       mapping.first >= mapping.offset && mapping.first - mapping.offset == base &&
                       mapping.contains(base + range.first, range.second);
            }))
            throw std::runtime_error("patch range is outside the pinned RX mapping");
}
} // namespace ct::steamvr
