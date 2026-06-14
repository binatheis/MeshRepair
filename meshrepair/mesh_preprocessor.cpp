#include "mesh_preprocessor.h"
#include "mesh_loader.h"  // For PolygonSoup
#include "polygon_soup_repair.h"
#include "polygon_soup_validation.h"
#include <CGAL/Polygon_mesh_processing/repair.h>
#include <CGAL/Polygon_mesh_processing/repair_polygon_soup.h>
#include <CGAL/Polygon_mesh_processing/orient_polygon_soup.h>
#include <CGAL/Polygon_mesh_processing/polygon_soup_to_polygon_mesh.h>
#include <CGAL/Polygon_mesh_processing/connected_components.h>
#include <CGAL/boost/graph/Euler_operations.h>
#include <CGAL/boost/graph/helpers.h>
#include <CGAL/IO/PLY.h>
#include <CGAL/bounding_box.h>
#include "debug_path.h"
#include "include/logger.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <iomanip>
#include <limits>
#include <map>
#include <set>
#include <string>
#include <thread>
#include <sstream>
#include <vector>

namespace PMP = CGAL::Polygon_mesh_processing;

namespace MeshRepair {

namespace {

    struct SoupEdgeUse {
        size_t a    = 0;
        size_t b    = 0;
        size_t v0   = 0;
        size_t v1   = 0;
        size_t face = 0;
    };

    struct SoupEdgeUseLess {
        bool operator()(const SoupEdgeUse& lhs, const SoupEdgeUse& rhs) const
        {
            if (lhs.a != rhs.a) {
                return lhs.a < rhs.a;
            }
            if (lhs.b != rhs.b) {
                return lhs.b < rhs.b;
            }
            return lhs.face < rhs.face;
        }
    };

    struct FacePair {
        size_t f0 = 0;
        size_t f1 = 0;
    };

    struct BoundaryEdgeInfo {
        size_t v0   = 0;
        size_t v1   = 0;
        size_t face = 0;
    };

    size_t invalid_index() { return std::numeric_limits<size_t>::max(); }

    double compute_soup_bbox_diagonal(const std::vector<Point_3>& points)
    {
        if (points.empty()) {
            return 0.0;
        }

        auto bbox         = CGAL::bounding_box(points.begin(), points.end());
        auto diag_squared = CGAL::squared_distance(bbox.min(), bbox.max());
        return std::sqrt(CGAL::to_double(diag_squared));
    }

    size_t thin_bridge_boundary_separation(size_t max_face_hops, size_t requested_separation)
    {
        if (requested_separation > 0) {
            return requested_separation;
        }
        if (max_face_hops == 0) {
            return 1;
        }
        return max_face_hops * 2 + 1;
    }

    void build_offsets_from_counts(std::vector<size_t>& offsets, const std::vector<size_t>& counts)
    {
        offsets.resize(counts.size() + 1);
        offsets[0] = 0;
        for (size_t i = 0; i < counts.size(); ++i) {
            offsets[i + 1] = offsets[i] + counts[i];
        }
    }

    void mark_near_boundary_edges(size_t source, size_t max_boundary_steps,
                                  const std::vector<BoundaryEdgeInfo>& boundary_edges,
                                  const std::vector<size_t>& vertex_edge_offsets,
                                  const std::vector<size_t>& vertex_edges, std::vector<size_t>& boundary_marks,
                                  std::vector<size_t>& boundary_depths, std::vector<size_t>& queue, size_t token)
    {
        queue.clear();
        boundary_marks[source]  = token;
        boundary_depths[source] = 0;
        queue.push_back(source);

        size_t head = 0;
        while (head < queue.size()) {
            size_t edge_id = queue[head++];
            size_t depth   = boundary_depths[edge_id];
            if (depth >= max_boundary_steps) {
                continue;
            }

            const BoundaryEdgeInfo& edge = boundary_edges[edge_id];
            const size_t endpoints[2]    = { edge.v0, edge.v1 };
            for (size_t endpoint_index = 0; endpoint_index < 2; ++endpoint_index) {
                size_t vertex = endpoints[endpoint_index];
                if (vertex + 1 >= vertex_edge_offsets.size()) {
                    continue;
                }

                for (size_t i = vertex_edge_offsets[vertex]; i < vertex_edge_offsets[vertex + 1]; ++i) {
                    size_t other_edge = vertex_edges[i];
                    if (boundary_marks[other_edge] == token) {
                        continue;
                    }
                    boundary_marks[other_edge]  = token;
                    boundary_depths[other_edge] = depth + 1;
                    queue.push_back(other_edge);
                }
            }
        }
    }

    void mark_face_path(size_t face, const std::vector<size_t>& face_parent, std::vector<uint8_t>& remove_flags)
    {
        size_t current = face;
        while (current != invalid_index()) {
            remove_flags[current] = 1;
            current               = face_parent[current];
        }
    }

    size_t assign_boundary_loop_ids(const std::vector<BoundaryEdgeInfo>& boundary_edges,
                                    const std::vector<size_t>& vertex_edge_offsets,
                                    const std::vector<size_t>& vertex_edges, std::vector<size_t>& boundary_loop_ids,
                                    std::vector<size_t>& queue)
    {
        boundary_loop_ids.assign(boundary_edges.size(), invalid_index());
        size_t loop_count = 0;

        for (size_t source = 0; source < boundary_edges.size(); ++source) {
            if (boundary_loop_ids[source] != invalid_index()) {
                continue;
            }

            queue.clear();
            boundary_loop_ids[source] = loop_count;
            queue.push_back(source);

            size_t head = 0;
            while (head < queue.size()) {
                size_t edge_id               = queue[head++];
                const BoundaryEdgeInfo& edge = boundary_edges[edge_id];
                const size_t endpoints[2]    = { edge.v0, edge.v1 };

                for (size_t endpoint_index = 0; endpoint_index < 2; ++endpoint_index) {
                    size_t vertex = endpoints[endpoint_index];
                    if (vertex + 1 >= vertex_edge_offsets.size()) {
                        continue;
                    }

                    for (size_t i = vertex_edge_offsets[vertex]; i < vertex_edge_offsets[vertex + 1]; ++i) {
                        size_t next_edge = vertex_edges[i];
                        if (boundary_loop_ids[next_edge] != invalid_index()) {
                            continue;
                        }
                        boundary_loop_ids[next_edge] = loop_count;
                        queue.push_back(next_edge);
                    }
                }
            }

            ++loop_count;
        }

        return loop_count;
    }

    size_t remove_thin_bridge_polygons(PolygonSoup& soup, size_t max_face_hops, size_t requested_boundary_separation,
                                       bool verbose)
    {
        const size_t face_count = soup.polygons.size();
        if (face_count == 0) {
            return 0;
        }

        size_t total_edges = 0;
        for (size_t face = 0; face < face_count; ++face) {
            total_edges += soup.polygons[face].size();
        }
        if (total_edges == 0) {
            return 0;
        }

        std::vector<SoupEdgeUse> edge_uses;
        edge_uses.reserve(total_edges);
        for (size_t face = 0; face < face_count; ++face) {
            const std::vector<std::size_t>& polygon = soup.polygons[face];
            const size_t n                          = polygon.size();
            if (n < 2) {
                continue;
            }

            for (size_t i = 0; i < n; ++i) {
                size_t v0 = polygon[i];
                size_t v1 = polygon[(i + 1) % n];
                if (v0 == v1 || v0 >= soup.points.size() || v1 >= soup.points.size()) {
                    continue;
                }

                SoupEdgeUse edge;
                edge.a    = std::min(v0, v1);
                edge.b    = std::max(v0, v1);
                edge.v0   = v0;
                edge.v1   = v1;
                edge.face = face;
                edge_uses.push_back(edge);
            }
        }

        if (edge_uses.empty()) {
            return 0;
        }

        std::sort(edge_uses.begin(), edge_uses.end(), SoupEdgeUseLess());

        std::vector<FacePair> face_pairs;
        std::vector<BoundaryEdgeInfo> boundary_edges;
        face_pairs.reserve(edge_uses.size() / 2);
        boundary_edges.reserve(edge_uses.size() / 4);

        size_t run_begin = 0;
        while (run_begin < edge_uses.size()) {
            size_t run_end = run_begin + 1;
            while (run_end < edge_uses.size() && edge_uses[run_end].a == edge_uses[run_begin].a
                   && edge_uses[run_end].b == edge_uses[run_begin].b) {
                ++run_end;
            }

            const size_t run_size = run_end - run_begin;
            if (run_size == 1) {
                BoundaryEdgeInfo boundary_edge;
                boundary_edge.v0   = edge_uses[run_begin].v0;
                boundary_edge.v1   = edge_uses[run_begin].v1;
                boundary_edge.face = edge_uses[run_begin].face;
                boundary_edges.push_back(boundary_edge);
            } else if (run_size == 2) {
                size_t f0 = edge_uses[run_begin].face;
                size_t f1 = edge_uses[run_begin + 1].face;
                if (f0 != f1) {
                    FacePair pair;
                    pair.f0 = f0;
                    pair.f1 = f1;
                    face_pairs.push_back(pair);
                }
            }

            run_begin = run_end;
        }

        if (boundary_edges.size() < 2) {
            return 0;
        }

        std::vector<size_t> face_neighbor_counts(face_count, 0);
        for (size_t i = 0; i < face_pairs.size(); ++i) {
            const FacePair& pair = face_pairs[i];
            if (pair.f0 < face_count && pair.f1 < face_count) {
                ++face_neighbor_counts[pair.f0];
                ++face_neighbor_counts[pair.f1];
            }
        }

        std::vector<size_t> face_neighbor_offsets;
        build_offsets_from_counts(face_neighbor_offsets, face_neighbor_counts);

        std::vector<size_t> face_neighbors(face_neighbor_offsets[face_count], invalid_index());
        std::vector<size_t> face_neighbor_cursor = face_neighbor_offsets;
        for (size_t i = 0; i < face_pairs.size(); ++i) {
            const FacePair& pair = face_pairs[i];
            if (pair.f0 >= face_count || pair.f1 >= face_count) {
                continue;
            }
            face_neighbors[face_neighbor_cursor[pair.f0]++] = pair.f1;
            face_neighbors[face_neighbor_cursor[pair.f1]++] = pair.f0;
        }

        std::vector<size_t> face_source_counts(face_count, 0);
        for (size_t source = 0; source < boundary_edges.size(); ++source) {
            const size_t face = boundary_edges[source].face;
            if (face < face_count) {
                ++face_source_counts[face];
            }
        }

        std::vector<size_t> face_source_offsets;
        build_offsets_from_counts(face_source_offsets, face_source_counts);

        std::vector<size_t> face_sources(face_source_offsets[face_count], invalid_index());
        std::vector<size_t> face_source_cursor = face_source_offsets;
        for (size_t source = 0; source < boundary_edges.size(); ++source) {
            const size_t face = boundary_edges[source].face;
            if (face < face_count) {
                face_sources[face_source_cursor[face]++] = source;
            }
        }

        std::vector<size_t> vertex_boundary_counts(soup.points.size(), 0);
        for (size_t source = 0; source < boundary_edges.size(); ++source) {
            const BoundaryEdgeInfo& edge = boundary_edges[source];
            if (edge.v0 < soup.points.size()) {
                ++vertex_boundary_counts[edge.v0];
            }
            if (edge.v1 < soup.points.size()) {
                ++vertex_boundary_counts[edge.v1];
            }
        }

        std::vector<size_t> vertex_edge_offsets;
        build_offsets_from_counts(vertex_edge_offsets, vertex_boundary_counts);

        std::vector<size_t> vertex_edges(vertex_edge_offsets[soup.points.size()], invalid_index());
        std::vector<size_t> vertex_edge_cursor = vertex_edge_offsets;
        for (size_t source = 0; source < boundary_edges.size(); ++source) {
            const BoundaryEdgeInfo& edge = boundary_edges[source];
            if (edge.v0 < soup.points.size()) {
                vertex_edges[vertex_edge_cursor[edge.v0]++] = source;
            }
            if (edge.v1 < soup.points.size()) {
                vertex_edges[vertex_edge_cursor[edge.v1]++] = source;
            }
        }

        const size_t min_boundary_separation = thin_bridge_boundary_separation(max_face_hops,
                                                                               requested_boundary_separation);

        std::vector<uint8_t> remove_flags(face_count, 0);
        std::vector<size_t> boundary_marks(boundary_edges.size(), 0);
        std::vector<size_t> boundary_depths(boundary_edges.size(), 0);
        std::vector<size_t> boundary_queue;
        boundary_queue.reserve(boundary_edges.size());

        std::vector<size_t> boundary_loop_ids;
        size_t boundary_loop_count = assign_boundary_loop_ids(boundary_edges, vertex_edge_offsets, vertex_edges,
                                                              boundary_loop_ids, boundary_queue);

        std::vector<size_t> face_marks(face_count, 0);
        std::vector<size_t> face_depths(face_count, 0);
        std::vector<size_t> face_parent(face_count, invalid_index());
        std::vector<size_t> face_queue;
        face_queue.reserve(face_count);

        size_t boundary_token = 1;
        size_t face_token     = 1;
        for (size_t source = 0; source < boundary_edges.size(); ++source) {
            ++boundary_token;
            if (boundary_token == 0) {
                std::fill(boundary_marks.begin(), boundary_marks.end(), 0);
                boundary_token = 1;
            }
            mark_near_boundary_edges(source, min_boundary_separation, boundary_edges, vertex_edge_offsets, vertex_edges,
                                     boundary_marks, boundary_depths, boundary_queue, boundary_token);

            const size_t start_face = boundary_edges[source].face;
            if (start_face >= face_count) {
                continue;
            }

            ++face_token;
            if (face_token == 0) {
                std::fill(face_marks.begin(), face_marks.end(), 0);
                face_token = 1;
            }

            face_queue.clear();
            face_marks[start_face]  = face_token;
            face_depths[start_face] = 0;
            face_parent[start_face] = invalid_index();
            face_queue.push_back(start_face);

            size_t head = 0;
            while (head < face_queue.size()) {
                size_t face  = face_queue[head++];
                size_t depth = face_depths[face];

                bool remove_path = false;
                for (size_t i = face_source_offsets[face]; i < face_source_offsets[face + 1]; ++i) {
                    size_t target_source = face_sources[i];
                    if (target_source != source && boundary_loop_ids[target_source] == boundary_loop_ids[source]
                        && boundary_marks[target_source] != boundary_token) {
                        remove_path = true;
                        break;
                    }
                }
                if (remove_path) {
                    mark_face_path(face, face_parent, remove_flags);
                }

                if (depth >= max_face_hops) {
                    continue;
                }

                for (size_t i = face_neighbor_offsets[face]; i < face_neighbor_offsets[face + 1]; ++i) {
                    size_t next_face = face_neighbors[i];
                    if (next_face >= face_count || face_marks[next_face] == face_token) {
                        continue;
                    }
                    face_marks[next_face]  = face_token;
                    face_depths[next_face] = depth + 1;
                    face_parent[next_face] = face;
                    face_queue.push_back(next_face);
                }
            }
        }

        size_t removed = 0;
        for (size_t i = 0; i < remove_flags.size(); ++i) {
            if (remove_flags[i]) {
                ++removed;
            }
        }
        if (removed == 0) {
            return 0;
        }

        std::vector<std::vector<std::size_t>> filtered_polygons;
        filtered_polygons.reserve(face_count - removed);
        for (size_t i = 0; i < face_count; ++i) {
            if (!remove_flags[i]) {
                filtered_polygons.push_back(std::move(soup.polygons[i]));
            }
        }
        soup.polygons.swap(filtered_polygons);

        if (verbose) {
            std::ostringstream msg;
            msg << "Removed: " << removed << " thin-bridge polygon(s), max hops=" << max_face_hops
                << ", boundary separation=" << min_boundary_separation << ", boundary loops=" << boundary_loop_count;
            logDetail(LogCategory::Preprocess, msg.str());
        }

        return removed;
    }

}  // namespace

MeshPreprocessor::MeshPreprocessor(Mesh& mesh, const PreprocessingOptions& options)
    : mesh_(mesh)
    , options_(options)
    , stats_()
{
}


// ============================================================================
// Mesh-level helper functions
// ============================================================================

size_t
MeshPreprocessor::remove_isolated_vertices()
{
    return PMP::remove_isolated_vertices(mesh_);
}

size_t
MeshPreprocessor::keep_only_largest_connected_component()
{
    // Create a face property map to store component IDs
    auto fccmap = mesh_.add_property_map<face_descriptor, std::size_t>("f:CC").first;

    // Compute connected components
    std::size_t num_components = PMP::connected_components(mesh_, fccmap);

    stats_.connected_components_found = num_components;

    if (num_components <= 1) {
        // Only one component, nothing to remove
        mesh_.remove_property_map(fccmap);
        return 0;
    }

    // Count faces in each component
    std::map<std::size_t, std::size_t> component_sizes;
    for (auto f : mesh_.faces()) {
        component_sizes[fccmap[f]]++;
    }

    // Find the largest component
    std::size_t largest_component_id   = 0;
    std::size_t largest_component_size = 0;
    for (const auto& pair : component_sizes) {
        if (pair.second > largest_component_size) {
            largest_component_id   = pair.first;
            largest_component_size = pair.second;
        }
    }

    if (options_.verbose) {
        logDetail(LogCategory::Preprocess, "Largest component ID: " + std::to_string(largest_component_id) + " with "
                                               + std::to_string(largest_component_size) + " faces");
    }

    // Remove all faces NOT in the largest component
    std::vector<face_descriptor> faces_to_remove;
    for (auto f : mesh_.faces()) {
        if (fccmap[f] != largest_component_id) {
            faces_to_remove.push_back(f);
        }
    }

    // Remove the faces
    for (auto f : faces_to_remove) {
        if (!mesh_.is_removed(f)) {
            CGAL::Euler::remove_face(CGAL::halfedge(f, mesh_), mesh_);
        }
    }

    // Clean up property map
    mesh_.remove_property_map(fccmap);

    // Remove isolated vertices created by face removal
    PMP::remove_isolated_vertices(mesh_);

    return num_components - 1;  // Number of small components removed
}

void
MeshPreprocessor::print_report() const
{
    logDetail(LogCategory::Preprocess, "=== Preprocessing Report ===");
    logDetail(LogCategory::Preprocess, "Duplicate vertices merged: " + std::to_string(stats_.duplicates_merged));
    logDetail(LogCategory::Preprocess,
              "Non-manifold polygons removed: " + std::to_string(stats_.non_manifold_vertices_removed));
    logDetail(LogCategory::Preprocess,
              "Long-edge polygons removed: " + std::to_string(stats_.long_edge_polygons_removed));
    logDetail(LogCategory::Preprocess,
              "Thin-bridge polygons removed: " + std::to_string(stats_.thin_bridge_polygons_removed));
    logDetail(LogCategory::Preprocess, "3-face fans collapsed: " + std::to_string(stats_.face_fans_collapsed));
    logDetail(LogCategory::Preprocess,
              "Isolated vertices removed: " + std::to_string(stats_.isolated_vertices_removed));
    logDetail(LogCategory::Preprocess,
              "Connected components found: " + std::to_string(stats_.connected_components_found));
    logDetail(LogCategory::Preprocess, "Small components removed: " + std::to_string(stats_.small_components_removed));
    {
        std::ostringstream oss;
        oss << "Total time: " << std::fixed << std::setprecision(2) << stats_.total_time_ms << " ms";
        logDetail(LogCategory::Preprocess, oss.str());
    }
    logDetail(LogCategory::Preprocess, "============================\n");
}

// Static function: preprocess polygon soup directly (avoids mesh->soup extraction!)
PreprocessingStats
MeshPreprocessor::preprocess_soup(PolygonSoup& soup, Mesh& output_mesh, const PreprocessingOptions& options)
{
    auto total_start = std::chrono::high_resolution_clock::now();
    auto soup_start  = total_start;

    PreprocessingStats stats;

    size_t initial_points   = soup.points.size();
    size_t initial_polygons = soup.polygons.size();

    if (options.verbose) {
        logDetail(LogCategory::Preprocess, "=== Mesh Preprocessing (Soup-Based) ===");
        logDetail(LogCategory::Preprocess, "Initial soup state: points=" + std::to_string(initial_points)
                                               + ", polygons=" + std::to_string(initial_polygons));
    }

    // =========================================================================
    // PHASE 1: POLYGON SOUP CLEANUP (already in soup space - no extraction!)
    // =========================================================================

    if (options.verbose) {
        logDetail(LogCategory::Preprocess, "[Phase 1] Polygon Soup Cleanup");
    }

    auto duplicates_start = std::chrono::high_resolution_clock::now();
    //
    // Step 1.1: Remove duplicate points
    if (options.remove_duplicates) {
        if (options.verbose) {
            logDetail(LogCategory::Preprocess, "[1/9] Removing duplicate points...");
        }
        size_t points_before = soup.points.size();
        PMP::merge_duplicate_points_in_polygon_soup(soup.points, soup.polygons);
        stats.duplicates_merged = points_before - soup.points.size();
        if (options.verbose) {
            logDetail(LogCategory::Preprocess,
                      "Merged: " + std::to_string(stats.duplicates_merged) + " duplicate points");
        }
    }
    auto duplicates_end      = std::chrono::high_resolution_clock::now();
    stats.duplicates_time_ms = std::chrono::duration<double, std::milli>(duplicates_end - duplicates_start).count();

    auto degenerate_start = std::chrono::high_resolution_clock::now();
    //
    // Step 1.2: Remove duplicate polygons
    if (options.remove_duplicates) {
        size_t polygons_before = soup.polygons.size();
        PMP::merge_duplicate_polygons_in_polygon_soup(soup.points, soup.polygons);
        if (options.debug && options.verbose && polygons_before > soup.polygons.size()) {
            logDebug(LogCategory::Preprocess, "[DEBUG] Removed "
                                                  + std::to_string(polygons_before - soup.polygons.size())
                                                  + " duplicate polygons");
        }
    }

    //
    // Step 1.3: Remove degenerate polygons
    if (options.verbose) {
        logDetail(LogCategory::Preprocess, "[2/9] Removing degenerate polygons...");
    }
    size_t before_degenerate = soup.polygons.size();
    auto it = std::remove_if(soup.polygons.begin(), soup.polygons.end(), [](const std::vector<std::size_t>& poly) {
        if (poly.size() < 3)
            return true;
        std::set<std::size_t> unique_verts(poly.begin(), poly.end());
        return unique_verts.size() < 3;
    });
    soup.polygons.erase(it, soup.polygons.end());
    if (options.verbose) {
        logDetail(LogCategory::Preprocess,
                  "Removed: " + std::to_string(before_degenerate - soup.polygons.size()) + " degenerate polygons");
    }
    auto degenerate_end      = std::chrono::high_resolution_clock::now();
    stats.degenerate_time_ms = std::chrono::duration<double, std::milli>(degenerate_end - degenerate_start).count();

    if (options.debug) {
        MeshPreprocessor::plyDump(soup, DebugPath::step_file("after_removal"), "Saved soup (after degenerate removal)",
                                  options.verbose);
    }

    auto long_edge_start = std::chrono::high_resolution_clock::now();
    //
    // Step 1.4: Remove polygons with overly long edges (optional, disabled by default)
    if (options.remove_long_edges && options.long_edge_max_ratio > 0.0) {
        double bbox_diagonal = compute_soup_bbox_diagonal(soup.points);
        if (bbox_diagonal > 0.0) {
            double threshold        = options.long_edge_max_ratio * bbox_diagonal;
            double threshold_sq     = threshold * threshold;
            const size_t poly_count = soup.polygons.size();

            if (options.verbose) {
                std::ostringstream msg;
                msg << "[3/9] Removing long-edge polygons (threshold=" << threshold
                    << " units, ratio=" << options.long_edge_max_ratio << " of bbox diagonal)";
                logDetail(LogCategory::Preprocess, msg.str());
            }

            std::vector<uint8_t> remove_flags(poly_count, 0);
            if (poly_count > 0) {
                size_t hw_threads = std::thread::hardware_concurrency();
                if (hw_threads == 0) {
                    hw_threads = 4;
                }
                size_t thread_count = std::min(hw_threads, poly_count);
                size_t chunk        = (poly_count + thread_count - 1) / thread_count;

                auto worker = [&](size_t start, size_t end) {
                    const auto& points   = soup.points;
                    const auto& polygons = soup.polygons;

                    for (size_t i = start; i < end; ++i) {
                        const auto& poly = polygons[i];
                        if (poly.size() < 2) {
                            continue;
                        }

                        bool remove    = false;
                        const size_t n = poly.size();
                        for (size_t j = 0; j < n; ++j) {
                            std::size_t i0 = poly[j];
                            std::size_t i1 = poly[(j + 1) % n];
                            if (i0 >= points.size() || i1 >= points.size()) {
                                continue;
                            }
                            double sq = CGAL::to_double(CGAL::squared_distance(points[i0], points[i1]));
                            if (sq > threshold_sq) {
                                remove = true;
                                break;
                            }
                        }

                        if (remove) {
                            remove_flags[i] = 1;
                        }
                    }
                };

                std::vector<std::thread> threads;
                threads.reserve(thread_count);
                for (size_t t = 0; t < thread_count; ++t) {
                    size_t start = t * chunk;
                    if (start >= poly_count) {
                        break;
                    }
                    size_t end = std::min(start + chunk, poly_count);
                    threads.emplace_back(worker, start, end);
                }

                for (auto& th : threads) {
                    if (th.joinable()) {
                        th.join();
                    }
                }

                std::vector<std::vector<std::size_t>> filtered;
                filtered.reserve(poly_count);
                size_t removed = 0;
                for (size_t i = 0; i < poly_count; ++i) {
                    if (remove_flags[i]) {
                        ++removed;
                    } else {
                        filtered.push_back(std::move(soup.polygons[i]));
                    }
                }
                soup.polygons.swap(filtered);
                stats.long_edge_polygons_removed = removed;

                if (options.verbose) {
                    logDetail(LogCategory::Preprocess, "Removed: " + std::to_string(removed) + " long-edge polygon(s)");
                }
            }
        }
        if (options.debug) {
            MeshPreprocessor::plyDump(soup, DebugPath::step_file("after_long_edges"),
                                      "Saved soup (after long-edge removal)", options.verbose);
        }
    }
    auto long_edge_end      = std::chrono::high_resolution_clock::now();
    stats.long_edge_time_ms = std::chrono::duration<double, std::milli>(long_edge_end - long_edge_start).count();

    auto face_fans_start = std::chrono::high_resolution_clock::now();
    //
    // Step 1.5: Remove 3-face fans
    if (options.remove_3_face_fans) {
        if (options.verbose) {
            logDetail(LogCategory::Preprocess, "[4/9] Collapsing 3-face fans...");
        }
        size_t fans_collapsed     = PolygonSoupRepair::remove_3_face_fans(soup.points, soup.polygons);
        stats.face_fans_collapsed = fans_collapsed;
        if (options.verbose) {
            logDetail(LogCategory::Preprocess, "Collapsed: " + std::to_string(fans_collapsed) + " 3-face fan(s)");
        }
    }
    auto face_fans_end      = std::chrono::high_resolution_clock::now();
    stats.face_fans_time_ms = std::chrono::duration<double, std::milli>(face_fans_end - face_fans_start).count();

    if (options.debug) {
        MeshPreprocessor::plyDump(soup, DebugPath::step_file("after_3_face_fans"), "Saved soup (after 3-face fans)",
                                  options.verbose);
    }

    auto non_manifold_start = std::chrono::high_resolution_clock::now();
    //
    // Step 1.6: Remove non-manifold vertices/edges
    if (options.remove_non_manifold) {
        if (options.verbose) {
            logDetail(LogCategory::Preprocess,
                      "[5/9] Removing non-manifold vertices/edges (recursive local search)...");
        }
        size_t max_depth = (options.non_manifold_passes > 0) ? options.non_manifold_passes : 10;
        auto nm_result   = PolygonSoupRepair::remove_non_manifold_polygons_detailed(soup.polygons, max_depth,
                                                                                    options.debug);
        stats.non_manifold_vertices_removed = nm_result.total_polygons_removed;
        if (options.verbose) {
            std::ostringstream oss;
            oss << "Removed: " << nm_result.total_polygons_removed << " polygon(s) in " << nm_result.iterations_executed
                << " iteration(s)";
            if (nm_result.hit_max_iterations) {
                oss << " (hit max limit of " << max_depth << ")";
            }
            logDetail(LogCategory::Preprocess, oss.str());
        }
    }
    auto non_manifold_end = std::chrono::high_resolution_clock::now();
    stats.non_manifold_time_ms
        = std::chrono::duration<double, std::milli>(non_manifold_end - non_manifold_start).count();

    if (options.debug) {
        MeshPreprocessor::plyDump(soup, DebugPath::step_file("after_non_manifold_removal"),
                                  "Saved soup (after non-manifold removal)", options.verbose);
    }

    auto thin_bridge_start = std::chrono::high_resolution_clock::now();
    //
    // Step 1.7: Remove narrow boundary-to-boundary polygon bridges (optional, disabled by default)
    if (options.remove_thin_bridges) {
        if (options.verbose) {
            std::ostringstream msg;
            msg << "[6/9] Removing thin polygon bridges (max face hops=" << options.thin_bridge_max_hops << ")";
            logDetail(LogCategory::Preprocess, msg.str());
        }
        stats.thin_bridge_polygons_removed = remove_thin_bridge_polygons(soup, options.thin_bridge_max_hops,
                                                                         options.thin_bridge_min_boundary_separation,
                                                                         options.verbose);
        if (options.debug) {
            MeshPreprocessor::plyDump(soup, DebugPath::step_file("after_thin_bridges"),
                                      "Saved soup (after thin-bridge removal)", options.verbose);
        }
    }
    auto thin_bridge_end      = std::chrono::high_resolution_clock::now();
    stats.thin_bridge_time_ms = std::chrono::duration<double, std::milli>(thin_bridge_end - thin_bridge_start).count();

    auto orient_start = thin_bridge_end;
    //
    // Step 1.8: Orient polygon soup (disabled for now)
#if false
    if (options.verbose) {
        logDetail(LogCategory::Preprocess, "[6/7] Orienting polygon soup...");
    }
    bool oriented = PMP::orient_polygon_soup(soup.points, soup.polygons);
    if (options.verbose) {
        logDetail(LogCategory::Preprocess,
                  oriented ? "Oriented successfully" : "Warning: Some points duplicated during orientation");
    }
    auto orient_end        = std::chrono::high_resolution_clock::now();
#else
    auto orient_end = orient_start;
#endif
    stats.orient_time_ms = std::chrono::duration<double, std::milli>(orient_end - orient_start).count();

#if false
    auto validation_result = validate_polygon_soup_basic(soup.points, soup.polygons);
    if (options.verbose) {
        if (validation_result.polygons_removed_total == 0) {
            logDetail(LogCategory::Preprocess, "Soup validation: no additional issues found");
        } else {
            std::ostringstream oss;
            oss << "Soup validation removed " << validation_result.polygons_removed_total << " polygon(s)"
                << " [out_of_bounds=" << validation_result.polygons_removed_out_of_bounds
                << ", invalid_cycle=" << validation_result.polygons_removed_invalid_cycle
                << ", edge_orientation=" << validation_result.polygons_removed_edge_orientation
                << ", non_manifold=" << validation_result.polygons_removed_non_manifold << "]";
            logWarn(LogCategory::Preprocess, oss.str());
        }
    }
#endif

    auto soup_end              = std::chrono::high_resolution_clock::now();
    stats.soup_cleanup_time_ms = std::chrono::duration<double, std::milli>(soup_end - soup_start).count();

    // =========================================================================
    // PHASE 2: CONVERT TO MESH (ONCE!)
    // =========================================================================

    if (options.verbose) {
        logDetail(LogCategory::Preprocess, "[7/9] Converting soup to mesh (one-time conversion)...");
    }

    auto mesh_convert_start = std::chrono::high_resolution_clock::now();

    output_mesh.clear();
    PMP::polygon_soup_to_polygon_mesh(soup.points, soup.polygons, output_mesh);

    auto mesh_convert_end = std::chrono::high_resolution_clock::now();
    stats.soup_to_mesh_time_ms
        = std::chrono::duration<double, std::milli>(mesh_convert_end - mesh_convert_start).count();

    if (options.verbose) {
        logDetail(LogCategory::Preprocess, "Mesh: " + std::to_string(output_mesh.number_of_vertices()) + " vertices, "
                                               + std::to_string(output_mesh.number_of_faces()) + " faces");
    }

    // =========================================================================
    // PHASE 3: MESH-LEVEL CLEANUP (Quick operations requiring halfedge structure)
    // =========================================================================

    auto mesh_cleanup_start = std::chrono::high_resolution_clock::now();

    if (options.verbose) {
        logDetail(LogCategory::Preprocess, "[Phase 2] Mesh-Level Cleanup");
    }

    // Remove isolated vertices
    if (options.remove_isolated) {
        if (options.verbose) {
            logDetail(LogCategory::Preprocess, "[8/9] Removing isolated vertices...");
        }
        stats.isolated_vertices_removed = PMP::remove_isolated_vertices(output_mesh);
        if (options.verbose) {
            logDetail(LogCategory::Preprocess,
                      "Removed: " + std::to_string(stats.isolated_vertices_removed) + " isolated vertices");
        }
    }

    // Keep largest component
    if (options.keep_largest_component) {
        if (options.verbose) {
            logDetail(LogCategory::Preprocess, "[9/9] Keeping only largest connected component...");
        }

        auto fccmap                      = output_mesh.add_property_map<face_descriptor, std::size_t>("f:CC").first;
        std::size_t num_components       = PMP::connected_components(output_mesh, fccmap);
        stats.connected_components_found = num_components;

        if (num_components > 1) {
            std::map<std::size_t, std::size_t> component_sizes;
            for (auto f : output_mesh.faces()) {
                component_sizes[fccmap[f]]++;
            }

            std::size_t largest_id   = 0;
            std::size_t largest_size = 0;
            for (const auto& pair : component_sizes) {
                if (pair.second > largest_size) {
                    largest_id   = pair.first;
                    largest_size = pair.second;
                }
            }

            std::vector<face_descriptor> faces_to_remove;
            for (auto f : output_mesh.faces()) {
                if (fccmap[f] != largest_id) {
                    faces_to_remove.push_back(f);
                }
            }

            for (auto f : faces_to_remove) {
                if (!output_mesh.is_removed(f)) {
                    CGAL::Euler::remove_face(CGAL::halfedge(f, output_mesh), output_mesh);
                }
            }

            output_mesh.remove_property_map(fccmap);
            PMP::remove_isolated_vertices(output_mesh);
            stats.small_components_removed = num_components - 1;
        }

        if (options.verbose) {
            logDetail(LogCategory::Preprocess,
                      "Found: " + std::to_string(stats.connected_components_found) + " component(s)");
            logDetail(LogCategory::Preprocess,
                      "Removed: " + std::to_string(stats.small_components_removed) + " small component(s)");
        }
    }

    auto mesh_cleanup_end = std::chrono::high_resolution_clock::now();
    stats.mesh_cleanup_time_ms
        = std::chrono::duration<double, std::milli>(mesh_cleanup_end - mesh_cleanup_start).count();

    // Collect garbage
    if (output_mesh.has_garbage()) {
        if (options.verbose) {
            logDetail(LogCategory::Preprocess, "Collecting garbage (compacting mesh)...");
        }
        output_mesh.collect_garbage();
    }

    // Validate
    if (options.verbose) {
        logDetail(LogCategory::Preprocess, "Validating mesh topology...");
    }
    bool is_valid = CGAL::is_valid_polygon_mesh(output_mesh, options.verbose);
    if (!is_valid && options.verbose) {
        logWarn(LogCategory::Preprocess, "WARNING: Mesh is not valid after preprocessing!");
    }

    auto total_end      = std::chrono::high_resolution_clock::now();
    stats.total_time_ms = std::chrono::duration<double, std::milli>(total_end - total_start).count();

    if (options.verbose) {
        logDetail(LogCategory::Preprocess,
                  "Final mesh state: vertices=" + std::to_string(output_mesh.number_of_vertices())
                      + ", faces=" + std::to_string(output_mesh.number_of_faces())
                      + ", valid=" + std::string(is_valid ? "YES" : "NO"));
        {
            std::ostringstream oss;
            oss << "Timing breakdown: soup cleanup=" << std::fixed << std::setprecision(2) << stats.soup_cleanup_time_ms
                << " ms";
            logDetail(LogCategory::Preprocess, oss.str());
        }
        logDetail(LogCategory::Preprocess, "  Duplicates: " + std::to_string(stats.duplicates_time_ms) + " ms");
        logDetail(LogCategory::Preprocess, "  Degenerate: " + std::to_string(stats.degenerate_time_ms) + " ms");
        logDetail(LogCategory::Preprocess, "  Long-edge: " + std::to_string(stats.long_edge_time_ms) + " ms");
        logDetail(LogCategory::Preprocess, "  Thin-bridge: " + std::to_string(stats.thin_bridge_time_ms) + " ms");
        logDetail(LogCategory::Preprocess, "  Non-manifold: " + std::to_string(stats.non_manifold_time_ms) + " ms");
        logDetail(LogCategory::Preprocess, "  3-face fans: " + std::to_string(stats.face_fans_time_ms) + " ms");
        logDetail(LogCategory::Preprocess, "  Orient: " + std::to_string(stats.orient_time_ms) + " ms");
        logDetail(LogCategory::Preprocess,
                  "Soup->Mesh conversion: " + std::to_string(stats.soup_to_mesh_time_ms) + " ms");
        logDetail(LogCategory::Preprocess, "Mesh cleanup: " + std::to_string(stats.mesh_cleanup_time_ms) + " ms");
        logDetail(LogCategory::Preprocess, "Total: " + std::to_string(stats.total_time_ms) + " ms");
        logDetail(LogCategory::Preprocess, "========================================\n");
    }

    return stats;
}

void
MeshPreprocessor::plyDump(MeshRepair::PolygonSoup& soup, const std::string debug_file, const std::string message,
                          bool verbose)
{
    auto debug_points     = soup.points;
    auto debug_polygons   = soup.polygons;
    auto debug_validation = validate_polygon_soup_basic(debug_points, debug_polygons);

    Mesh debug_mesh;
    PMP::polygon_soup_to_polygon_mesh(debug_points, debug_polygons, debug_mesh);
    auto resolved = MeshRepair::DebugPath::resolve(debug_file);
    if (CGAL::IO::write_PLY(resolved, debug_mesh, CGAL ::parameters::use_binary_mode(true))) {
        if (verbose) {
            logDebug(LogCategory::Preprocess, "[DEBUG] " + message + " : " + resolved);
            logDebug(LogCategory::Preprocess, "[DEBUG]   Mesh: " + std::to_string(debug_mesh.number_of_vertices())
                                                  + " vertices, " + std::to_string(debug_mesh.number_of_faces())
                                                  + " faces");
            if (debug_validation.polygons_removed_total > 0) {
                std::ostringstream oss;
                oss << "[DEBUG]   Validation removed " << debug_validation.polygons_removed_total
                    << " polygon(s) before dump";
                logDebug(LogCategory::Preprocess, oss.str());
            }
        }
    }
}


int
preprocess_soup_c(PolygonSoup* soup, Mesh* out_mesh, const PreprocessingOptions* options, PreprocessingStats* out_stats)
{
    if (!soup || !out_mesh) {
        return -1;
    }
    PreprocessingOptions opts = options ? *options : PreprocessingOptions();
    PreprocessingStats stats  = MeshPreprocessor::preprocess_soup(*soup, *out_mesh, opts);
    if (out_stats) {
        *out_stats = stats;
    }
    return 0;
}

}  // namespace MeshRepair
