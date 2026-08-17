# meshrepair (Python binding)

CGAL-based mesh hole filling and repair, exposed as a native Python module.

Built on the MeshRepair core: Liepa triangulation + Botsch bi-Laplacian fairing,
soup-space preprocessing, and a partitioned parallel pipeline.

## Install

Wheels are published as build artifacts on GitHub Actions (`cp311-abi3`,
Python 3.11–3.14, Windows x64 / Linux x86_64 / macOS arm64).

## Usage

```python
import meshrepair

# One-shot file repair
stats = meshrepair.repair_file("damaged.obj", "repaired.obj",
                               continuity=1, refine=True)
print(stats["holes_filled"])

# In-memory (engine binary format: float32 LE verts + uint32 LE triangles)
out = meshrepair.repair_soup_bytes(data, vertex_count, face_count)

# Stateful session
s = meshrepair.RepairSession(threads=8)
s.load_mesh_bytes(data, nv, nf)
holes = s.detect_holes()
stats = s.fill_holes(continuity=2)
result = s.mesh_bytes()
```

## License

GPL-3.0-or-later (inherited from CGAL).
