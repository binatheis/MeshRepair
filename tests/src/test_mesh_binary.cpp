// Roundtrip and error-path tests for the IPC binary mesh format.
#include "engine/mesh_binary.h"

#include <CGAL/Polygon_mesh_processing/polygon_soup_to_polygon_mesh.h>
#include <gtest/gtest.h>

#include <cstdint>
#include <cstring>
#include <stdexcept>
#include <vector>

namespace {

using namespace MeshRepair;
using namespace MeshRepair::Engine;

Mesh
make_open_box_soup_mesh()
{
    // Open box (missing top face): 8 vertices, 10 triangles.
    PolygonSoup soup;
    const double c = 1.0;
    soup.points   = {
        {0, 0, 0}, {c, 0, 0}, {c, c, 0}, {0, c, 0},  // bottom z=0
        {0, 0, c}, {c, 0, c}, {c, c, c}, {0, c, c},  // top z=c
    };
    soup.polygons = {
        {0, 2, 1}, {0, 3, 2},                   // bottom
        {0, 1, 5}, {0, 5, 4},                   // back y=0
        {1, 2, 6}, {1, 6, 5},                   // right x=c
        {2, 3, 7}, {2, 7, 6},                   // front y=c
        {3, 0, 4}, {3, 4, 7},                   // left x=0
    };                                          // top (z=c) intentionally missing
    Mesh mesh;
    CGAL::Polygon_mesh_processing::polygon_soup_to_polygon_mesh(soup.points, soup.polygons, mesh);
    return mesh;
}

std::vector<uint8_t>
build_raw(const std::vector<float>& verts, const std::vector<uint32_t>& faces)
{
    std::vector<uint8_t> out;
    auto append_u32 = [&out](uint32_t v) {
        for (int b = 0; b < 4; ++b)
            out.push_back(static_cast<uint8_t>((v >> (8 * b)) & 0xFF));
    };
    auto append_f32 = [&out](float f) {
        uint32_t v;
        std::memcpy(&v, &f, 4);
        for (int b = 0; b < 4; ++b)
            out.push_back(static_cast<uint8_t>((v >> (8 * b)) & 0xFF));
    };
    append_u32(static_cast<uint32_t>(verts.size() / 3));
    for (float f : verts)
        append_f32(f);
    append_u32(static_cast<uint32_t>(faces.size() / 3));
    for (uint32_t i : faces)
        append_u32(i);
    return out;
}

}  // namespace

TEST(MeshBinary, SerializeDeserializeRoundtrip)
{
    Mesh mesh = make_open_box_soup_mesh();
    ASSERT_TRUE(mesh.is_valid());

    auto data = serialize_mesh_binary(mesh);
    ASSERT_EQ(data.size(), 4u + 8 * 12 + 4u + 10 * 12);

    Mesh back = deserialize_mesh_binary(data, 8, 10);
    EXPECT_TRUE(back.is_valid());
    EXPECT_EQ(back.number_of_vertices(), 8u);
    EXPECT_EQ(back.number_of_faces(), 10u);
}

TEST(MeshBinary, DeserializeToSoupRoundtrip)
{
    Mesh mesh = make_open_box_soup_mesh();
    auto data = serialize_mesh_binary(mesh);

    PolygonSoup soup = deserialize_mesh_binary_to_soup(data, 8, 10);
    EXPECT_EQ(soup.points.size(), 8u);
    ASSERT_EQ(soup.polygons.size(), 10u);
    for (const auto& poly : soup.polygons)
        ASSERT_EQ(poly.size(), 3u);
}

TEST(MeshBinary, CountMismatchRejected)
{
    Mesh mesh = make_open_box_soup_mesh();
    auto data = serialize_mesh_binary(mesh);
    EXPECT_THROW(deserialize_mesh_binary(data, 7, 10), std::runtime_error);
    EXPECT_THROW(deserialize_mesh_binary(data, 8, 11), std::runtime_error);
    // 0 skips the check
    EXPECT_NO_THROW(deserialize_mesh_binary(data, 0, 0));
}

TEST(MeshBinary, TruncatedBufferRejected)
{
    Mesh mesh = make_open_box_soup_mesh();
    auto data = serialize_mesh_binary(mesh);
    ASSERT_GT(data.size(), 10u);
    EXPECT_THROW(deserialize_mesh_binary(std::vector<uint8_t>(data.begin(), data.begin() + 10),
                                         0, 0),
                 std::runtime_error);
}

TEST(MeshBinary, FaceIndexOutOfRangeRejected)
{
    std::vector<float> verts = {0, 0, 0, 1, 0, 0, 0, 1, 0};
    std::vector<uint32_t> faces = {0, 1, 99};  // 99 out of range
    auto data = build_raw(verts, faces);
    EXPECT_THROW(deserialize_mesh_binary_to_soup(data, 0, 0), std::runtime_error);
}

TEST(MeshBinary, Base64Roundtrip)
{
    std::vector<uint8_t> data(257);
    for (size_t i = 0; i < data.size(); ++i)
        data[i] = static_cast<uint8_t>(i * 7 + 3);

    std::string encoded = base64_encode(data);
    std::vector<uint8_t> decoded = base64_decode(encoded);
    EXPECT_EQ(decoded, data);
}

TEST(MeshBinary, Base64InvalidCharRejected)
{
    EXPECT_THROW(base64_decode("abc!defg"), std::runtime_error);
    EXPECT_THROW(base64_decode("!!!!"), std::runtime_error);
}

TEST(MeshBinary, EmptyMeshRoundtrip)
{
    Mesh empty;
    auto data = serialize_mesh_binary(empty);
    Mesh back = deserialize_mesh_binary(data, 0, 0);
    EXPECT_EQ(back.number_of_vertices(), 0u);
    EXPECT_EQ(back.number_of_faces(), 0u);
}
