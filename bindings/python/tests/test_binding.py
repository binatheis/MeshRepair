# SPDX-License-Identifier: GPL-3.0-or-later
"""Core behavior tests: bytes roundtrip, continuity levels, errors, files."""

import struct

import pytest

from conftest import parse_mesh_bytes


class TestVersion:
    def test_version_string(self, meshrepair_module):
        assert meshrepair_module.version() == "2.8.0"
        assert meshrepair_module.__version__ == "2.8.0"


class TestRepairSoupBytes:
    def test_open_box_gets_closed(self, meshrepair_module, open_box):
        data, nv, nf = open_box
        out = meshrepair_module.repair_soup_bytes(data, nv, nf, preprocess=False, max_diameter=1.0)
        assert isinstance(out, bytes)
        out_nv, _verts, out_nf, _faces = parse_mesh_bytes(out)
        # Filling adds faces; the top hole patch is at least 2 triangles.
        assert out_nf > nf
        assert out_nv >= nv

    @pytest.mark.parametrize("continuity", [0, 1, 2])
    def test_continuity_levels(self, meshrepair_module, open_box, continuity):
        data, nv, nf = open_box
        out = meshrepair_module.repair_soup_bytes(
            data, nv, nf, preprocess=False, continuity=continuity, max_diameter=1.0
        )
        _ov, _v, out_nf, _f = parse_mesh_bytes(out)
        assert out_nf > nf

    def test_empty_mesh_roundtrip(self, meshrepair_module):
        buf = struct.pack("<I", 0) + struct.pack("<I", 0)
        out = meshrepair_module.repair_soup_bytes(buf, 0, 0, preprocess=False)
        nv, _, nf, _ = parse_mesh_bytes(out)
        assert nv == 0
        assert nf == 0

    def test_invalid_count_raises(self, meshrepair_module, open_box):
        data, nv, nf = open_box
        with pytest.raises(RuntimeError):
            meshrepair_module.repair_soup_bytes(data, nv + 5, nf, preprocess=False)

    def test_truncated_data_raises(self, meshrepair_module, open_box):
        data, nv, nf = open_box
        with pytest.raises(RuntimeError):
            meshrepair_module.repair_soup_bytes(data[:15], nv, nf, preprocess=False)


class TestDetectHolesBytes:
    def test_open_box_single_hole(self, meshrepair_module, open_box):
        data, nv, nf = open_box
        holes = meshrepair_module.detect_holes_bytes(data, nv, nf)
        assert isinstance(holes, list)
        assert len(holes) == 1
        hole = holes[0]
        assert hole["boundary_size"] == 4
        assert hole["estimated_diameter"] == pytest.approx(2**0.5, rel=1e-5)
        assert hole["estimated_area"] > 0


class TestRepairFile:
    def test_obj_roundtrip(self, meshrepair_module, tmp_path):
        # Write a small open box OBJ by hand.
        obj = tmp_path / "open_box.obj"
        lines = ["v 0 0 0", "v 1 0 0", "v 1 1 0", "v 0 1 0",
                 "v 0 0 1", "v 1 0 1", "v 1 1 1", "v 0 1 1",
                 "f 1 3 2", "f 1 4 3",
                 "f 1 2 6", "f 1 6 5",
                 "f 2 3 7", "f 2 7 6",
                 "f 3 4 8", "f 3 8 7",
                 "f 4 1 5", "f 4 5 8"]
        obj.write_text("\n".join(lines) + "\n")

        out = tmp_path / "repaired.obj"
        stats = meshrepair_module.repair_file(str(obj), str(out), preprocess=False, max_diameter=1.0)

        assert stats["holes_detected"] == 1
        assert stats["holes_filled"] == 1
        assert stats["holes_failed"] == 0
        assert out.exists()

        # The repaired OBJ must exist and be non-empty. Face count may differ
        # from the input soup because polygon_soup_to_polygon_mesh merges
        # duplicate vertices and drops degenerate faces.
        content = out.read_text()
        n_faces = sum(1 for ln in content.splitlines() if ln.startswith("f "))
        assert n_faces > 0

    def test_missing_input_raises(self, meshrepair_module, tmp_path):
        with pytest.raises(RuntimeError):
            meshrepair_module.repair_file(
                str(tmp_path / "nope.obj"), str(tmp_path / "out.obj")
            )


class TestUnknownOption:
    def test_unknown_kwarg_raises(self, meshrepair_module, open_box):
        data, nv, nf = open_box
        with pytest.raises((ValueError, TypeError)):
            meshrepair_module.repair_soup_bytes(data, nv, nf, nope=True)
