// SPDX-License-Identifier: MPL-2.0
#include "QuestHandMask.h"
#include <cmath>

namespace oxr
{
namespace
{
XrVector3f Rotate(const XrQuaternionf &q, const XrVector3f &v)
{
    const XrVector3f t{2 * (q.y * v.z - q.z * v.y), 2 * (q.z * v.x - q.x * v.z), 2 * (q.x * v.y - q.y * v.x)};
    return {v.x + q.w * t.x + q.y * t.z - q.z * t.y, v.y + q.w * t.y + q.z * t.x - q.x * t.z,
            v.z + q.w * t.z + q.x * t.y - q.y * t.x};
}
} // namespace

bool QuestHandMask::Load(PFN_xrGetHandMeshFB getMesh, XrHandTrackerEXT tracker)
{
    *this = {};
    XrHandTrackingMeshFB mesh{XR_TYPE_HAND_TRACKING_MESH_FB};
    if (!getMesh || XR_FAILED(getMesh(tracker, &mesh)) || mesh.jointCountOutput != XR_HAND_JOINT_COUNT_EXT ||
        !mesh.vertexCountOutput || mesh.vertexCountOutput > 32767 || !mesh.indexCountOutput ||
        mesh.indexCountOutput > 196608 || mesh.indexCountOutput % 3)
        return false;
    const auto count = mesh.vertexCountOutput;
    std::array<XrPosef, XR_HAND_JOINT_COUNT_EXT> bind{};
    std::array<XrHandJointEXT, XR_HAND_JOINT_COUNT_EXT> parents{};
    std::array<float, XR_HAND_JOINT_COUNT_EXT> radii{};
    std::vector<XrVector3f> positions(count), normals(count);
    std::vector<XrVector2f> uvs(count);
    std::vector<XrVector4sFB> joints(count);
    std::vector<XrVector4f> weights(count);
    std::vector<int16_t> indices(mesh.indexCountOutput);
    mesh.jointCapacityInput = bind.size();
    mesh.jointBindPoses = bind.data();
    mesh.jointParents = parents.data();
    mesh.jointRadii = radii.data();
    mesh.vertexCapacityInput = count;
    mesh.vertexPositions = positions.data();
    mesh.vertexNormals = normals.data();
    mesh.vertexUVs = uvs.data();
    mesh.vertexBlendIndices = joints.data();
    mesh.vertexBlendWeights = weights.data();
    mesh.indexCapacityInput = indices.size();
    mesh.indices = indices.data();
    if (XR_FAILED(getMesh(tracker, &mesh)) || mesh.jointCountOutput != bind.size() || mesh.vertexCountOutput != count ||
        mesh.indexCountOutput != indices.size())
        return false;
    for (auto index : indices)
        if (index < 0 || static_cast<uint32_t>(index) >= count)
            return false;
    QuestHandMask validated;
    validated.bindings_.resize(count);
    for (size_t i = 0; i < count; ++i)
    {
        const int ids[] = {joints[i].x, joints[i].y, joints[i].z, joints[i].w};
        const float w[] = {weights[i].x, weights[i].y, weights[i].z, weights[i].w};
        float sum = 0;
        for (int j = 0; j < 4; ++j)
        {
            if (!std::isfinite(w[j]) || w[j] < 0)
                return false;
            if (w[j] == 0)
                continue;
            if (ids[j] < 0 || ids[j] >= static_cast<int>(bind.size()))
                return false;
            const auto &pose = bind[ids[j]];
            const auto &q = pose.orientation;
            const auto &p = positions[i];
            auto local =
                Rotate({-q.x, -q.y, -q.z, q.w}, {p.x - pose.position.x, p.y - pose.position.y, p.z - pose.position.z});
            if (!std::isfinite(local.x + local.y + local.z))
                return false;
            validated.bindings_[i][j] = {local, w[j], ids[j]};
            validated.used_[ids[j]] = true;
            sum += w[j];
        }
        if (sum < .98f || sum > 1.02f)
            return false;
        for (auto &influence : validated.bindings_[i])
            influence.weight /= sum;
    }
    validated.vertices_.resize(count);
    validated.indices_.assign(indices.begin(), indices.end());
    *this = std::move(validated);
    return true;
}

bool QuestHandMask::Skin(const XrHandJointLocationsEXT &loc, float scale)
{
    constexpr auto required = XR_SPACE_LOCATION_POSITION_VALID_BIT | XR_SPACE_LOCATION_ORIENTATION_VALID_BIT;
    if (bindings_.empty() || !loc.isActive || !loc.jointLocations || loc.jointCount != XR_HAND_JOINT_COUNT_EXT ||
        !std::isfinite(scale) || scale <= 0 ||
        !(loc.jointLocations[XR_HAND_JOINT_WRIST_EXT].locationFlags & XR_SPACE_LOCATION_POSITION_TRACKED_BIT))
        return false;
    for (size_t i = 0; i < used_.size(); ++i)
        if (used_[i] && (loc.jointLocations[i].locationFlags & required) != required)
            return false;
    for (size_t i = 0; i < vertices_.size(); ++i)
    {
        XrVector3f result{};
        for (const auto &inf : bindings_[i])
        {
            if (inf.weight == 0)
                continue;
            const auto &pose = loc.jointLocations[inf.joint].pose;
            const auto p = Rotate(pose.orientation, inf.offset);
            result.x += inf.weight * (pose.position.x + scale * p.x);
            result.y += inf.weight * (pose.position.y + scale * p.y);
            result.z += inf.weight * (pose.position.z + scale * p.z);
        }
        if (!std::isfinite(result.x + result.y + result.z))
            return false;
        vertices_[i] = result;
    }
    return true;
}
} // namespace oxr
