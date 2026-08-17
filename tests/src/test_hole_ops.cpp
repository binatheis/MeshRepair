// Hole detection, analysis, guards and filling on synthetic meshes.
#include "include/mesh_loader.h"
#include "include/hole_ops.h"

#include <CGAL/Polygon_mesh_processing/polygon_soup_to_polygon_mesh.h>
#include <CGAL/boost/graph/helpers.h>
#include <gtest/gtest.h>

#include <cmath>
#include <set>
#include <vector>


namespace {

using namespace MeshRepair;

Mesh
make_open_box(double c = 1.0)
{
    PolygonSoup soup;
    soup.points   = {
        {0, 0, 0}, {c, 0, 0}, {c, c, 0}, {0, c, 0},
        {0, 0, c}, {c, 0, c}, {c, c, c}, {0, c, c},
    };
    soup.polygons = {
        {0, 2, 1}, {0, 3, 2},
        {0, 1, 5}, {0, 5, 4},
        {1, 2, 6}, {1, 6, 5},
        {2, 3, 7}, {2, 7, 6},
        {3, 0, 4}, {3, 4, 7},
    };
    Mesh mesh;
    CGAL::Polygon_mesh_processing::polygon_soup_to_polygon_mesh(soup.points, soup.polygons, mesh);
    return mesh;
}

}  // namespace

TEST(HoleOps, DetectSingleHoleInOpenBox)
{
    Mesh mesh = make_open_box();
    std::vector<HoleInfo> holes;
    ASSERT_EQ(detect_all_holes_c(mesh, false, holes), 0);
    ASSERT_EQ(holes.size(), 1u);
    EXPECT_EQ(holes[0].boundary_size, 4u);
    EXPECT_EQ(holes[0].boundary_vertices.size(), 4u);
}

TEST(HoleOps, CountBorderEdgesOpenBox)
{
    Mesh mesh = make_open_box();
    EXPECT_EQ(count_border_edges(mesh), 4u);
}

TEST(HoleOps, AnalyzeHoleDiameter)
{
    Mesh mesh = make_open_box(2.0);
    std::vector<HoleInfo> holes;
    ASSERT_EQ(detect_all_holes_c(mesh, false, holes), 0);
    ASSERT_EQ(holes.size(), 1u);

    // Top face is a 2x2 square: bbox diagonal of the boundary is 2*sqrt(2).
    EXPECT_NEAR(holes[0].estimated_diameter, 2.0 * std::sqrt(2.0), 1e-9);
    EXPECT_GT(holes[0].estimated_area, 0.0);
}

TEST(HoleOps, ClosedMeshHasNoHoles)
{
    PolygonSoup soup;
    soup.points   = {
        {0, 0, 0}, {1, 0, 0}, {1, 1, 0}, {0, 1, 0},
        {0, 0, 1}, {1, 0, 1}, {1, 1, 1}, {0, 1, 1},
    };
    soup.polygons = {
        {0, 2, 1}, {0, 3, 2},
        {4, 5, 6}, {4, 6, 7},
        {0, 1, 5}, {0, 5, 4},
        {1, 2, 6}, {1, 6, 5},
        {2, 3, 7}, {2, 7, 6},
        {3, 0, 4}, {3, 4, 7},
    };
    Mesh mesh;
    CGAL::Polygon_mesh_processing::polygon_soup_to_polygon_mesh(soup.points, soup.polygons, mesh);

    std::vector<HoleInfo> holes;
    ASSERT_EQ(detect_all_holes_c(mesh, false, holes), 0);
    EXPECT_TRUE(holes.empty());
    EXPECT_TRUE(CGAL::is_closed(mesh));
}

TEST(HoleOps, GuardMaxBoundarySkipsLargeHole)
{
    Mesh mesh = make_open_box();
    std::vector<HoleInfo> holes;
    ASSERT_EQ(detect_all_holes_c(mesh, false, holes), 0);
    ASSERT_EQ(holes.size(), 1u);

    // Boundary has 4 vertices: a limit of 3 must cause the skip.
    FillingOptions opts;
    opts.max_hole_boundary_vertices = 3;

    HoleFillerCtx ctx;
    ctx.mesh        = &mesh;
    ctx.options     = opts;
    ctx.cancel_flag = nullptr;

    MeshStatistics stats = fill_all_holes_ctx(&ctx, holes);
    EXPECT_EQ(stats.num_holes_skipped, 1u);
    EXPECT_EQ(stats.num_holes_filled, 0u);
}

TEST(HoleOps, GuardMaxDiameterRatioSkipsBigHole)
{
    Mesh mesh = make_open_box();
    std::vector<HoleInfo> holes;
    ASSERT_EQ(detect_all_holes_c(mesh, false, holes), 0);

    // Hole diameter is sqrt(2) while the mesh bbox diagonal is sqrt(3):
    // ratio ~0.816 — a limit of 0.1 must skip it.
    FillingOptions opts;
    opts.max_hole_diameter_ratio = 0.1;

    HoleFillerCtx ctx;
    ctx.mesh    = &mesh;
    ctx.options = opts;

    MeshStatistics stats = fill_all_holes_ctx(&ctx, holes);
    EXPECT_EQ(stats.num_holes_skipped, 1u);
}

TEST(HoleOps, GuardSelectionBoundaryProtectsUserSelection)
{
    Mesh mesh = make_open_box();
    std::vector<HoleInfo> holes;
    ASSERT_EQ(detect_all_holes_c(mesh, false, holes), 0);

    // Mark all 4 boundary vertices as the user selection: hole must be skipped.
    FillingOptions opts;
    opts.guard_selection_boundary = true;
    for (auto v : holes[0].boundary_vertices)
        opts.selection_boundary_vertices.insert(static_cast<uint32_t>(v));

    HoleFillerCtx ctx;
    ctx.mesh    = &mesh;
    ctx.options = opts;

    MeshStatistics stats = fill_all_holes_ctx(&ctx, holes);
    EXPECT_EQ(stats.num_holes_skipped, 1u);
}

TEST(HoleOps, FillHoleClosesOpenBox)
{
    Mesh mesh = make_open_box();
    std::vector<HoleInfo> holes;
    ASSERT_EQ(detect_all_holes_c(mesh, false, holes), 0);
    ASSERT_EQ(holes.size(), 1u);

    HoleFillerCtx ctx;
    ctx.mesh    = &mesh;
    // Hole spans the whole top face: diameter/bbox ratio ~0.816, so the
    // default 0.10 guard would skip it. Allow big holes explicitly.
    FillingOptions opts;
    opts.max_hole_diameter_ratio = 1.0;
    ctx.options = opts;

    HoleStatistics hole_stats = fill_hole_ctx(&ctx, holes[0]);
    EXPECT_TRUE(hole_stats.filled_successfully);
    EXPECT_GT(hole_stats.num_faces_added, 0u);
    EXPECT_TRUE(CGAL::is_closed(mesh));
    EXPECT_TRUE(CGAL::is_valid_polygon_mesh(mesh));
}
