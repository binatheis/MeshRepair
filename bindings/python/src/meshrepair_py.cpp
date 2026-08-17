// MeshRepair Python binding (nanobind, stable ABI).
//
// Exposes:
//   - version()
//   - repair_file(input, output, **options)          full CLI-equivalent flow
//   - repair_soup_bytes(data, nv, nf, **options)     in-memory repair via the
//                                                     engine binary mesh format
//   - detect_holes_bytes(data, nv, nf, **options)    in-memory hole detection
//   - RepairSession                                  stateful engine mirror
//
// All long operations release the GIL; the internal pipeline is already
// multi-threaded (std::thread) and cancellation uses an atomic flag.

#include "engine/mesh_binary.h"
#include "include/config.h"
#include "include/hole_ops.h"
#include "include/mesh_loader.h"
#include "include/mesh_preprocessor.h"
#include "include/pipeline_ops.h"
#include "include/worker_pool.h"

#include <CGAL/Polygon_mesh_processing/polygon_soup_to_polygon_mesh.h>

#include <nanobind/nanobind.h>
#include <nanobind/ndarray.h>
#include <nanobind/stl/string.h>
#include <nanobind/stl/vector.h>
#include <nanobind/stl/bind_vector.h>

#include <atomic>
#include <chrono>
#include <memory>
#include <stdexcept>
#include <string>

namespace nb = nanobind;
using namespace MeshRepair;

namespace {

// ---------------------------------------------------------------------------
// Option plumbing: kwargs -> native structs
// ---------------------------------------------------------------------------

struct PyFillOptions {
    unsigned int continuity          = Config::DEFAULT_FAIRING_CONTINUITY;
    bool refine                      = Config::DEFAULT_REFINE;
    size_t max_boundary              = Config::DEFAULT_MAX_HOLE_BOUNDARY;
    double max_diameter_ratio        = Config::DEFAULT_MAX_HOLE_DIAMETER_RATIO;
    bool use_partitioned             = true;
    bool holes_only                  = false;
    bool use_2d_cdt                  = Config::DEFAULT_USE_2D_CDT;
    bool use_3d_delaunay             = Config::DEFAULT_USE_3D_DELAUNAY;
    bool skip_cubic                  = Config::DEFAULT_SKIP_CUBIC;
    bool preprocess                  = true;
    bool remove_duplicates           = true;
    bool remove_non_manifold         = true;
    bool remove_3_face_fans          = true;
    bool remove_isolated             = true;
    bool keep_largest_component      = false;
    bool remove_long_edges           = false;
    bool remove_thin_bridges         = false;
    size_t threads                   = 0;
};

FillingOptions
to_filling_options(const PyFillOptions& py)
{
    FillingOptions opts;
    opts.fairing_continuity   = py.continuity;
    opts.refine               = py.refine;
    opts.max_hole_boundary_vertices = py.max_boundary;
    opts.max_hole_diameter_ratio    = py.max_diameter_ratio;
    opts.use_2d_cdt           = py.use_2d_cdt;
    opts.use_3d_delaunay      = py.use_3d_delaunay;
    opts.skip_cubic_search    = py.skip_cubic;
    opts.holes_only           = py.holes_only;
    opts.verbose              = false;
    opts.show_progress        = false;
    return opts;
}

PreprocessingOptions
to_preprocess_options(const PyFillOptions& py)
{
    PreprocessingOptions opts;
    opts.remove_duplicates       = py.remove_duplicates;
    opts.remove_non_manifold     = py.remove_non_manifold;
    opts.remove_3_face_fans      = py.remove_3_face_fans;
    opts.remove_isolated         = py.remove_isolated;
    opts.keep_largest_component  = py.keep_largest_component;
    opts.remove_long_edges       = py.remove_long_edges;
    opts.remove_thin_bridges     = py.remove_thin_bridges;
    opts.verbose                 = false;
    opts.debug                   = false;
    return opts;
}

// ---------------------------------------------------------------------------
// stats -> dict
// ---------------------------------------------------------------------------

nb::dict
hole_stats_to_dict(const HoleStatistics& h)
{
    nb::dict d;
    d["boundary_vertices"] = h.num_boundary_vertices;
    d["faces_added"]       = h.num_faces_added;
    d["vertices_added"]    = h.num_vertices_added;
    d["area"]              = h.hole_area;
    d["diameter"]          = h.hole_diameter;
    d["filled"]            = h.filled_successfully;
    d["fairing_succeeded"] = h.fairing_succeeded;
    d["fill_time_ms"]      = h.fill_time_ms;
    if (!h.error_message.empty())
        d["error"] = h.error_message;
    return d;
}

nb::dict
mesh_stats_to_dict(const MeshStatistics& s)
{
    nb::dict d;
    d["original_vertices"] = s.original_vertices;
    d["original_faces"]    = s.original_faces;
    d["final_vertices"]    = s.final_vertices;
    d["final_faces"]       = s.final_faces;
    d["holes_detected"]    = s.num_holes_detected;
    d["holes_filled"]      = s.num_holes_filled;
    d["holes_failed"]      = s.num_holes_failed;
    d["holes_skipped"]     = s.num_holes_skipped;
    d["total_time_ms"]     = s.total_time_ms;
    d["fill_time_ms"]      = s.fill_time_ms;
    nb::list details;
    for (const auto& h : s.hole_details)
        details.append(hole_stats_to_dict(h));
    d["hole_details"] = details;
    return d;
}

nb::dict
preprocess_stats_to_dict(const PreprocessingStats& s)
{
    nb::dict d;
    d["duplicates_merged"]             = s.duplicates_merged;
    d["non_manifold_vertices_removed"] = s.non_manifold_vertices_removed;
    d["face_fans_collapsed"]           = s.face_fans_collapsed;
    d["long_edge_polygons_removed"]    = s.long_edge_polygons_removed;
    d["thin_bridge_polygons_removed"]  = s.thin_bridge_polygons_removed;
    d["isolated_vertices_removed"]     = s.isolated_vertices_removed;
    d["connected_components_found"]    = s.connected_components_found;
    d["small_components_removed"]      = s.small_components_removed;
    d["total_time_ms"]                 = s.total_time_ms;
    return d;
}

// ---------------------------------------------------------------------------
// Shared repair core (detection + fill only; preprocessing is handled by the
// callers in soup space, which is cheaper than mesh space)
// ---------------------------------------------------------------------------

MeshStatistics
run_fill_pipeline(Mesh& mesh, ThreadManager& mgr, const PyFillOptions& py, std::atomic<bool>* cancel)
{
    ParallelPipelineCtx ctx;
    ctx.mesh        = &mesh;
    ctx.thread_mgr  = &mgr;
    ctx.options     = to_filling_options(py);
    ctx.cancel_flag = cancel;

    if (py.use_partitioned) {
        return parallel_fill_partitioned(&ctx, false, false);
    }
    PipelineContext seq_ctx;
    seq_ctx.mesh        = &mesh;
    seq_ctx.thread_mgr  = &mgr;
    seq_ctx.options     = ctx.options;
    seq_ctx.cancel_flag = cancel;
    return pipeline_process_batch(&seq_ctx, false);
}

PyFillOptions
parse_options(const nb::kwargs& kwargs)
{
    PyFillOptions py;
    for (auto [key, value] : kwargs) {
        std::string k = nb::cast<std::string>(key);
        if (k == "continuity")             py.continuity = nb::cast<unsigned int>(value);
        else if (k == "refine")            py.refine = nb::cast<bool>(value);
        else if (k == "max_boundary")      py.max_boundary = nb::cast<size_t>(value);
        else if (k == "max_diameter")      py.max_diameter_ratio = nb::cast<double>(value);
        else if (k == "use_partitioned")   py.use_partitioned = nb::cast<bool>(value);
        else if (k == "holes_only")        py.holes_only = nb::cast<bool>(value);
        else if (k == "use_2d_cdt")        py.use_2d_cdt = nb::cast<bool>(value);
        else if (k == "use_3d_delaunay")   py.use_3d_delaunay = nb::cast<bool>(value);
        else if (k == "skip_cubic")        py.skip_cubic = nb::cast<bool>(value);
        else if (k == "preprocess")        py.preprocess = nb::cast<bool>(value);
        else if (k == "remove_duplicates") py.remove_duplicates = nb::cast<bool>(value);
        else if (k == "remove_non_manifold") py.remove_non_manifold = nb::cast<bool>(value);
        else if (k == "remove_3_face_fans")   py.remove_3_face_fans = nb::cast<bool>(value);
        else if (k == "remove_isolated")      py.remove_isolated = nb::cast<bool>(value);
        else if (k == "keep_largest_component") py.keep_largest_component = nb::cast<bool>(value);
        else if (k == "remove_long_edges")     py.remove_long_edges = nb::cast<bool>(value);
        else if (k == "remove_thin_bridges")   py.remove_thin_bridges = nb::cast<bool>(value);
        else if (k == "threads")            py.threads = nb::cast<size_t>(value);
        else throw std::invalid_argument("unknown option: " + k);
    }
    return py;
}

std::unique_ptr<ThreadManager>
make_thread_manager(size_t threads)
{
    auto mgr = std::make_unique<ThreadManager>();
    ThreadingConfig cfg;
    cfg.num_threads    = threads;
    cfg.filling_threads = threads;
    cfg.detection_threads = threads;
    thread_manager_init(*mgr, cfg);
    return mgr;
}

// ---------------------------------------------------------------------------
// Module-level functions
// ---------------------------------------------------------------------------

nb::dict
repair_file(const std::string& input_path, const std::string& output_path, const nb::kwargs& kwargs)
{
    PyFillOptions py = parse_options(kwargs);

    PolygonSoup soup;
    if (!MeshLoader::load_soup(input_path, MeshLoader::Format::AUTO, false, &soup))
        throw std::runtime_error("failed to load mesh: " + input_path + " (" + MeshLoader::last_error() + ")");

    Mesh mesh;
    if (py.preprocess) {
        MeshPreprocessor::preprocess_soup(soup, mesh, to_preprocess_options(py));
    } else {
        CGAL::Polygon_mesh_processing::polygon_soup_to_polygon_mesh(soup.points, soup.polygons, mesh);
    }

    auto mgr = make_thread_manager(py.threads);

    MeshStatistics stats;
    {
        nb::gil_scoped_release release;
        stats = run_fill_pipeline(mesh, *mgr, py, nullptr);
    }

    if (!MeshLoader::save_mesh(mesh, output_path, MeshLoader::Format::AUTO, true))
        throw std::runtime_error("failed to save mesh: " + output_path);

    return mesh_stats_to_dict(stats);
}

nb::object
repair_soup_bytes(nb::bytes data, size_t vertex_count, size_t face_count, const nb::kwargs& kwargs)
{
    PyFillOptions py = parse_options(kwargs);

    const auto* raw = reinterpret_cast<const uint8_t*>(data.data());
    const size_t size = data.size();
    std::vector<uint8_t> buffer(raw, raw + size);

    PolygonSoup soup = Engine::deserialize_mesh_binary_to_soup(
        buffer,
        static_cast<uint32_t>(vertex_count),
        static_cast<uint32_t>(face_count));

    Mesh mesh;
    if (py.preprocess) {
        MeshPreprocessor::preprocess_soup(soup, mesh, to_preprocess_options(py));
    } else {
        CGAL::Polygon_mesh_processing::polygon_soup_to_polygon_mesh(soup.points, soup.polygons, mesh);
    }

    auto mgr = make_thread_manager(py.threads);

    MeshStatistics stats;
    {
        nb::gil_scoped_release release;
        stats = run_fill_pipeline(mesh, *mgr, py, nullptr);
    }

    auto out = Engine::serialize_mesh_binary(mesh);
    return nb::cast(nb::bytes(reinterpret_cast<const char*>(out.data()), out.size()));
}

nb::list
detect_holes_bytes(nb::bytes data, size_t vertex_count, size_t face_count, const nb::kwargs& kwargs)
{
    PyFillOptions py = parse_options(kwargs);

    const auto* raw = reinterpret_cast<const uint8_t*>(data.data());
    std::vector<uint8_t> buffer(raw, raw + data.size());

    PolygonSoup soup = Engine::deserialize_mesh_binary_to_soup(
        buffer, static_cast<uint32_t>(vertex_count), static_cast<uint32_t>(face_count));

    Mesh mesh;
    CGAL::Polygon_mesh_processing::polygon_soup_to_polygon_mesh(soup.points, soup.polygons, mesh);

    std::vector<HoleInfo> holes;
    nb::list out;
    {
        nb::gil_scoped_release release;
        if (detect_all_holes_c(mesh, false, holes) != 0)
            throw std::runtime_error("hole detection failed");
    }
    for (const auto& h : holes) {
        nb::dict d;
        d["boundary_size"]      = h.boundary_size;
        d["estimated_diameter"] = h.estimated_diameter;
        d["estimated_area"]     = h.estimated_area;
        out.append(d);
    }
    return out;
}

// ---------------------------------------------------------------------------
// RepairSession: stateful mirror of the engine flow
// ---------------------------------------------------------------------------

class RepairSession
{
public:
    explicit RepairSession(size_t threads)
    {
        auto mgr = make_thread_manager(threads);
        mgr_ = std::move(mgr);
    }

    void load_mesh_bytes(nb::bytes data, size_t vertex_count, size_t face_count)
    {
        const auto* raw = reinterpret_cast<const uint8_t*>(data.data());
        std::vector<uint8_t> buffer(raw, raw + data.size());
        soup_ = Engine::deserialize_mesh_binary_to_soup(
            buffer, static_cast<uint32_t>(vertex_count), static_cast<uint32_t>(face_count));
        mesh_.clear();
        has_mesh_ = false;
    }

    void load_file(const std::string& path)
    {
        if (!MeshLoader::load_soup(path, MeshLoader::Format::AUTO, false, &soup_))
            throw std::runtime_error("failed to load mesh: " + path + " (" + MeshLoader::last_error() + ")");
        mesh_.clear();
        has_mesh_ = false;
    }

    nb::dict preprocess(const nb::kwargs& kwargs)
    {
        require_soup();
        PyFillOptions py = parse_options(kwargs);
        PreprocessingOptions opts = to_preprocess_options(py);
        PreprocessingStats stats;
        {
            nb::gil_scoped_release release;
            stats = MeshPreprocessor::preprocess_soup(soup_, mesh_, opts);
        }
        has_mesh_ = true;
        return preprocess_stats_to_dict(stats);
    }

    nb::list detect_holes(const nb::kwargs& kwargs)
    {
        ensure_mesh();
        PyFillOptions py = parse_options(kwargs);

        std::vector<HoleInfo> holes;
        {
            nb::gil_scoped_release release;
            if (detect_all_holes_c(mesh_, false, holes) != 0)
                throw std::runtime_error("hole detection failed");
        }
        nb::list out;
        for (const auto& h : holes) {
            nb::dict d;
            d["boundary_size"]      = h.boundary_size;
            d["estimated_diameter"] = h.estimated_diameter;
            d["estimated_area"]     = h.estimated_area;
            out.append(d);
        }
        return out;
    }

    nb::dict fill_holes(const nb::kwargs& kwargs)
    {
        ensure_mesh();
        PyFillOptions py = parse_options(kwargs);

        ParallelPipelineCtx ctx;
        ctx.mesh        = &mesh_;
        ctx.thread_mgr  = mgr_.get();
        ctx.options     = to_filling_options(py);
        ctx.cancel_flag = &cancel_;

        MeshStatistics stats;
        {
            nb::gil_scoped_release release;
            if (py.use_partitioned) {
                stats = parallel_fill_partitioned(&ctx, false, false);
            } else {
                PipelineContext seq_ctx;
                seq_ctx.mesh        = &mesh_;
                seq_ctx.thread_mgr  = mgr_.get();
                seq_ctx.options     = ctx.options;
                seq_ctx.cancel_flag = &cancel_;
                stats = pipeline_process_batch(&seq_ctx, false);
            }
        }
        return mesh_stats_to_dict(stats);
    }

    nb::object mesh_bytes() const
    {
        const_cast<RepairSession*>(this)->ensure_mesh();
        auto out = Engine::serialize_mesh_binary(mesh_);
        return nb::cast(nb::bytes(reinterpret_cast<const char*>(out.data()), out.size()));
    }

    void save_mesh_file(const std::string& path, bool binary_ply) const
    {
        const_cast<RepairSession*>(this)->ensure_mesh();
        if (!MeshLoader::save_mesh(mesh_, path, MeshLoader::Format::AUTO, binary_ply))
            throw std::runtime_error("failed to save mesh: " + path);
    }

    void cancel() { cancel_.store(true); }
    void clear_cancel() { cancel_.store(false); }

private:
    void require_soup() const
    {
        if (soup_.points.empty())
            throw std::runtime_error("no mesh loaded");
    }

    void ensure_mesh()
    {
        require_soup();
        if (!has_mesh_) {
            CGAL::Polygon_mesh_processing::polygon_soup_to_polygon_mesh(soup_.points, soup_.polygons, mesh_);
            has_mesh_ = true;
        }
    }

    PolygonSoup soup_;
    Mesh mesh_;
    bool has_mesh_ = false;
    std::unique_ptr<ThreadManager> mgr_;
    std::atomic<bool> cancel_{false};
};

}  // namespace

NB_MODULE(meshrepair_core_py, m)
{
    m.doc() = "MeshRepair: CGAL-based hole filling with a partitioned parallel pipeline";

    m.attr("__version__") = Config::VERSION;

    m.def("version", [] { return std::string(Config::VERSION); },
          "Return the MeshRepair core version string.");

    m.def(
        "repair_file",
        &repair_file,
        "Repair a mesh file (OBJ/PLY/OFF): load soup -> preprocess -> fill -> save. Returns stats dict.",
        nb::sig("def repair_file(input: str, output: str, **options) -> dict"));

    m.def(
        "repair_soup_bytes",
        &repair_soup_bytes,
        "Repair an in-memory mesh in the engine binary format "
        "(float32 LE verts + uint32 LE triangles, same as the IPC protocol). "
        "Returns the repaired mesh as bytes in the same format.",
        nb::sig("def repair_soup_bytes(data: bytes, vertex_count: int, face_count: int, **options) -> bytes"));

    m.def(
        "detect_holes_bytes",
        &detect_holes_bytes,
        "Detect holes in an in-memory binary mesh. Returns a list of hole dicts.",
        nb::sig("def detect_holes_bytes(data: bytes, vertex_count: int, face_count: int, **options) -> list[dict]"));

    nb::class_<RepairSession>(m, "RepairSession",
                              "Stateful repair session mirroring the engine workflow.")
        .def(nb::init<size_t>(), nb::arg("threads") = 0,
             nb::sig("def __init__(self, threads: int = 0) -> None"))
        .def("load_mesh_bytes", &RepairSession::load_mesh_bytes,
             nb::arg("data"), nb::arg("vertex_count"), nb::arg("face_count"))
        .def("load_file", &RepairSession::load_file, nb::arg("path"))
        .def("preprocess", &RepairSession::preprocess,
             "Preprocess the loaded soup. Returns preprocessing stats dict.",
             nb::sig("def preprocess(self, **options) -> dict"))
        .def("detect_holes", &RepairSession::detect_holes,
             "Detect holes in the loaded mesh. Returns a list of hole dicts.",
             nb::sig("def detect_holes(self, **options) -> list[dict]"))
        .def("fill_holes", &RepairSession::fill_holes,
             "Fill holes in the loaded mesh. Returns repair stats dict.",
             nb::sig("def fill_holes(self, **options) -> dict"))
        .def("mesh_bytes", &RepairSession::mesh_bytes,
             "Return the current mesh as bytes in the engine binary format.")
        .def("save_mesh", &RepairSession::save_mesh_file,
             nb::arg("path"), nb::arg("binary_ply") = true)
        .def("cancel", &RepairSession::cancel,
             "Request cancellation of the running pipeline stage.")
        .def("clear_cancel", &RepairSession::clear_cancel);
}
