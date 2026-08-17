// Partitioned pipeline tests: load balancing, full repair, holes_only mode.
#include "include/mesh_loader.h"
#include "include/pipeline_ops.h"
#include "include/worker_pool.h"

#include <CGAL/Polygon_mesh_processing/polygon_soup_to_polygon_mesh.h>
#include <CGAL/boost/graph/helpers.h>
#include <gtest/gtest.h>

#include <algorithm>
#include <vector>

namespace {

using namespace MeshRepair;

}  // namespace

TEST(Pipeline, PartitionBalancesLoad)
{
    // Fabricated HoleInfo list (boundary_size only matters for balancing).
    std::vector<HoleInfo> holes(7);
    std::vector<size_t> weights = {100, 1, 1, 50, 50, 30, 20};
    for (size_t i = 0; i < holes.size(); ++i)
        holes[i].boundary_size = weights[i];

    auto parts = partition_holes_by_count(holes, 3);
    ASSERT_EQ(parts.size(), 3u);

    std::vector<size_t> loads(3, 0);
    size_t assigned = 0;
    for (size_t p = 0; p < parts.size(); ++p) {
        ASSERT_FALSE(parts[p].empty());  // greedy never emits empty partitions here
        for (size_t idx : parts[p]) {
            ASSERT_LT(idx, holes.size());
            loads[p] += holes[idx].boundary_size;
            ++assigned;
        }
    }
    EXPECT_EQ(assigned, holes.size());  // every hole assigned exactly once

    // Greedy LPT bound: max load <= (total + heaviest) / partitions is loose
    // but catches gross imbalance.
    size_t total = 0;
    for (size_t w : weights)
        total += w;
    size_t heaviest = *std::max_element(weights.begin(), weights.end());
    EXPECT_LE(*std::max_element(loads.begin(), loads.end()), (total + heaviest - 1) / 3 + 1);
}

TEST(Pipeline, PartitionMorePartitionsThanHoles)
{
    std::vector<HoleInfo> holes(2);
    holes[0].boundary_size = 10;
    holes[1].boundary_size = 5;
    auto parts = partition_holes_by_count(holes, 8);
    // Cannot produce more partitions than holes.
    EXPECT_LE(parts.size(), 2u);
}

TEST(Pipeline, PartitionEmptyHoles)
{
    std::vector<HoleInfo> holes;
    auto parts = partition_holes_by_count(holes, 3);
    // No holes to assign: at most one (empty) partition, nothing in it.
    size_t total = 0;
    for (const auto& p : parts)
        total += p.size();
    EXPECT_EQ(total, 0u);
    EXPECT_LE(parts.size(), 1u);
}

TEST(Pipeline, ParallelFillPartitionedClosesMesh)
{
    // Simple, robust input: open box (1 hole). The interesting multi-hole
    // topology is covered by the Blender integration tests; here we assert
    // the partitioned pipeline contract: valid, closed result + stats.
    PolygonSoup soup;
    soup.points = {
        {0, 0, 0}, {1, 0, 0}, {1, 1, 0}, {0, 1, 0},
        {0, 0, 1}, {1, 0, 1}, {1, 1, 1}, {0, 1, 1},
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

    ThreadManager mgr;
    thread_manager_init(mgr, ThreadingConfig{});

    FillingOptions fill_opts;
    // The top hole spans the whole bbox: raise the diameter guard so it fills.
    fill_opts.max_hole_diameter_ratio = 1.0;

    ParallelPipelineCtx ctx;
    ctx.mesh        = &mesh;
    ctx.thread_mgr  = &mgr;
    ctx.options     = fill_opts;
    ctx.cancel_flag = nullptr;
    ctx.timeout_ms  = 0.0;

    MeshStatistics stats = parallel_fill_partitioned(&ctx, false, false);
    EXPECT_EQ(stats.num_holes_detected, 1u);
    EXPECT_EQ(stats.num_holes_filled, 1u);
    EXPECT_EQ(stats.num_holes_failed, 0u);
    EXPECT_TRUE(CGAL::is_closed(mesh));
    EXPECT_TRUE(CGAL::is_valid_polygon_mesh(mesh));
    EXPECT_GT(stats.final_faces, stats.original_faces);
    EXPECT_GT(stats.total_time_ms, 0.0);
}

TEST(Pipeline, HolesOnlyModeReturnsPatchFaces)
{
    // holes_only is driven by options; verify it is plumbed through and the
    // mesh remains valid (the merge filters base faces).
    PolygonSoup soup;
    soup.points = {
        {0, 0, 0}, {1, 0, 0}, {1, 1, 0}, {0, 1, 0},
        {0, 0, 1}, {1, 0, 1}, {1, 1, 1}, {0, 1, 1},
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
    const size_t faces_before = mesh.number_of_faces();

    ThreadManager mgr;
    thread_manager_init(mgr, ThreadingConfig{});

    FillingOptions opts;
    opts.holes_only             = true;
    opts.max_hole_diameter_ratio = 1.0;

    ParallelPipelineCtx ctx;
    ctx.mesh        = &mesh;
    ctx.thread_mgr  = &mgr;
    ctx.options     = opts;
    ctx.cancel_flag = nullptr;

    MeshStatistics stats = parallel_fill_partitioned(&ctx, false, false);
    EXPECT_EQ(stats.num_holes_filled, 1u);
    // In holes_only mode the returned mesh contains only the patch faces.
    EXPECT_LT(mesh.number_of_faces(), faces_before);
    EXPECT_TRUE(CGAL::is_valid_polygon_mesh(mesh));
}
