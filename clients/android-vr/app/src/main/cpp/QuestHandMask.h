// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <array>
#include <cstdint>
#include <openxr/openxr.h>
#include <vector>

namespace oxr
{
class QuestHandMask
{
  public:
    bool Load(PFN_xrGetHandMeshFB getMesh, XrHandTrackerEXT tracker);
    bool Skin(const XrHandJointLocationsEXT &locations, float scale);
    const std::vector<XrVector3f> &Vertices() const
    {
        return vertices_;
    }
    const std::vector<uint16_t> &Indices() const
    {
        return indices_;
    }

  private:
    struct Influence
    {
        XrVector3f offset{};
        float weight = 0;
        int joint = 0;
    };
    std::vector<std::array<Influence, 4>> bindings_;
    std::vector<XrVector3f> vertices_;
    std::vector<uint16_t> indices_;
    std::array<bool, XR_HAND_JOINT_COUNT_EXT> used_{};
};
} // namespace oxr
