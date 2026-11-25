// Copyright (c) Valve Corporation. See LICENSE.
// https://github.com/ValveSoftware/Proton/blob/22f40122788d5f65389f59144705309ce9d6d403/vrclient_x64/openvr_v1.10.30/openvr.h#L5099-L5118
#pragma once
#include <cstdint>

namespace vr {
typedef uint64_t vrmb_typea;
enum vrmb_typeb { valuea = 0 };
class IVRMailbox {
  public:
    virtual vrmb_typeb undoc1(const char *name, vrmb_typea *handle) = 0;
    virtual vrmb_typeb undoc2(vrmb_typea handle) = 0;
    virtual vrmb_typeb undoc3(vrmb_typea handle, const char *destination, const char *message) = 0;
    virtual vrmb_typeb undoc4(vrmb_typea handle, char *buffer, uint32_t size, uint32_t *used) = 0;
};
} // namespace vr
