// SPDX-License-Identifier: MPL-2.0
#include "QuestHandMask.h"
#include <catch2/catch_test_macros.hpp>
#include <cmath>

namespace
{
XrResult XRAPI_PTR FakeMesh(XrHandTrackerEXT, XrHandTrackingMeshFB *m)
{
    m->jointCountOutput = XR_HAND_JOINT_COUNT_EXT;
    m->vertexCountOutput = 3;
    m->indexCountOutput = 3;
    if (!m->jointCapacityInput)
        return XR_SUCCESS;
    for (size_t j = 0; j < m->jointCountOutput; ++j)
        m->jointBindPoses[j] = {{0, 0, 0, 1}, {0, 0, 0}};
    m->jointBindPoses[1].position = {1, 0, 0};
    for (int i = 0; i < 3; ++i)
    {
        m->vertexPositions[i] = {2.f, float(i), 0};
        m->indices[i] = i;
        m->vertexBlendIndices[i] = {1, 0, -1, -1};
        m->vertexBlendWeights[i] = {.75f, .25f, 0, 0};
    }
    return XR_SUCCESS;
}
} // namespace
TEST_CASE("Meta hand mask skins the bind mesh, scales offsets, and drops lost hands", "[hand-mask]")
{
    oxr::QuestHandMask mesh;
    REQUIRE(mesh.Load(FakeMesh, XR_NULL_HANDLE));
    XrHandJointLocationEXT joints[XR_HAND_JOINT_COUNT_EXT]{};
    for (auto &j : joints)
    {
        j.pose = {{0, 0, 0, 1}, {0, 0, 0}};
        j.locationFlags = XR_SPACE_LOCATION_POSITION_TRACKED_BIT | XR_SPACE_LOCATION_POSITION_VALID_BIT |
                          XR_SPACE_LOCATION_ORIENTATION_VALID_BIT;
    }
    joints[1].pose = {{0, 0, 1, 0}, {0, 3, 0}};
    XrHandJointLocationsEXT locations{XR_TYPE_HAND_JOINT_LOCATIONS_EXT, nullptr, XR_TRUE, XR_HAND_JOINT_COUNT_EXT,
                                      joints};
    REQUIRE(mesh.Skin(locations, 2.f));
    CHECK(std::abs(mesh.Vertices()[0].x + .5f) < 1e-5f);
    CHECK(std::abs(mesh.Vertices()[0].y - 2.25f) < 1e-5f);
    joints[1].locationFlags = 0;
    CHECK_FALSE(mesh.Skin(locations, 1.f));
    locations.isActive = XR_FALSE;
    CHECK_FALSE(mesh.Skin(locations, 1.f));
}
