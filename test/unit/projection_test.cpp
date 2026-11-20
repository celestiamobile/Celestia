#include <doctest.h>

#include <vector>

#include <celengine/fisheyeprojectionmode.h>
#include <celengine/glsupport.h>
#include <celengine/perspectiveprojectionmode.h>
#include <celengine/pointstarrenderer.h>
#include <celengine/star.h>
#include <celengine/stellarclass.h>
#include <celmath/geomutil.h>

namespace
{

struct ReverseZScope
{
    bool previous = celestia::gl::reverseZ;
    ReverseZScope() { celestia::gl::reverseZ = true; }
    ~ReverseZScope() { celestia::gl::reverseZ = previous; }
};

}

TEST_CASE("Reverse-Z perspective retains depth precision through annotation projection")
{
    ReverseZScope scope;
    celestia::engine::PerspectiveProjectionMode projection(1200, 800, 400, 96);
    constexpr float nearZ = 0.001f;
    const auto matrix = projection.getProjectionMatrix(nearZ, 1.0e12f, 1.0f);
    const auto annotations = celestia::math::Ortho2D(0.0f, 1200.0f, 0.0f, 800.0f);
    for (float distance : {nearZ, 1.0f, 1.0e5f, 1.0e12f})
    {
        Eigen::Vector4f clip = matrix * Eigen::Vector4f(0, 0, -distance, 1);
        float depth = clip.z() / clip.w();
        CHECK(depth == doctest::Approx(nearZ / distance).epsilon(1.0e-6));
        float z = projection.getNormalizedDeviceZ(nearZ, 1.0e12f, distance);
        Eigen::Vector4f annotation = annotations * Eigen::Vector4f(0, 0, z, 1);
        CHECK(annotation.z() == doctest::Approx(depth).epsilon(1.0e-6));
        CHECK(annotation.z() > 0.0f);
    }
    CHECK(matrix == projection.getProjectionMatrix(nearZ, 1.0e6f, 1.0f));
}

TEST_CASE("Reverse-Z fisheye annotations match linear scene depth")
{
    ReverseZScope scope;
    celestia::engine::FisheyeProjectionMode projection(1200, 800, 96);
    constexpr float nearZ = 1.0f;
    constexpr float farZ = 1000.0f;
    const auto matrix = projection.getProjectionMatrix(nearZ, farZ, 1.0f);
    const auto annotations = celestia::math::Ortho2D(0.0f, 1200.0f, 0.0f, 800.0f);
    for (float distance : {nearZ, 10.0f, 500.0f, farZ})
    {
        Eigen::Vector4f clip = matrix * Eigen::Vector4f(0, 0, -distance, 1);
        float z = projection.getNormalizedDeviceZ(nearZ, farZ, distance);
        Eigen::Vector4f annotation = annotations * Eigen::Vector4f(0, 0, z, 1);
        CHECK(annotation.z() == doctest::Approx(clip.z()).epsilon(1.0e-6));
    }
}

TEST_CASE("Fisheye background stars behind the viewer are rejected before rendering")
{
    Star star(1, StarDetails::GetStarDetails(StellarClass::parse("G2V")));
    PointStarRenderer renderer;
    renderer.obsPos = Eigen::Vector3d::Zero();
    renderer.viewNormal = Eigen::Vector3f(0, 0, -1);
    renderer.clipBackgroundStarsBehindViewer = true;
    std::vector<RenderListEntry> entries;
    renderer.renderList = &entries;
    for (const auto& position : {Eigen::Vector3f(0, 0, 4), Eigen::Vector3f(0, 4, 0)})
    {
        star.setPosition(position);
        renderer.process(star, position.norm(), 0.0f);
        CHECK(entries.empty());
    }
}
