# SPDX-License-Identifier: GPL-3.0-or-later
"""RepairSession stateful workflow tests."""

import struct

import pytest

from conftest import parse_mesh_bytes


def parse(data):
    nv, _, nf, _ = parse_mesh_bytes(data)
    return nv, nf


class TestRepairSession:
    def test_full_workflow(self, meshrepair_module, open_box):
        data, nv, nf = open_box
        s = meshrepair_module.RepairSession(threads=2)

        s.load_mesh_bytes(data, nv, nf)

        holes = s.detect_holes(preprocess=False)
        assert len(holes) == 1
        assert holes[0]["boundary_size"] == 4

        stats = s.fill_holes(preprocess=False, max_diameter=1.0)
        assert stats["holes_filled"] == 1
        assert stats["holes_failed"] == 0

        out = s.mesh_bytes()
        out_nv, out_nf = parse(out)
        assert out_nf > nf

    def test_preprocess_then_fill(self, meshrepair_module, open_box):
        data, nv, nf = open_box
        s = meshrepair_module.RepairSession()

        s.load_mesh_bytes(data, nv, nf)
        pre_stats = s.preprocess()
        assert "total_time_ms" in pre_stats

        stats = s.fill_holes(max_diameter=1.0)
        assert stats["holes_filled"] == 1

    def test_save_mesh(self, meshrepair_module, open_box, tmp_path):
        data, nv, nf = open_box
        s = meshrepair_module.RepairSession()
        s.load_mesh_bytes(data, nv, nf)
        s.fill_holes(preprocess=False, max_diameter=1.0)

        out = tmp_path / "session_out.obj"
        s.save_mesh(str(out))
        assert out.exists()
        assert sum(1 for ln in out.read_text().splitlines() if ln.startswith("f ")) > nf

    def test_no_mesh_loaded_raises(self, meshrepair_module):
        s = meshrepair_module.RepairSession()
        with pytest.raises(RuntimeError):
            s.detect_holes()

    def test_load_file(self, meshrepair_module, tmp_path):
        # Open tetrahedron: 3 side faces, missing the bottom -> 1 hole.
        obj = tmp_path / "open_tet.obj"
        lines = ["v 0 0 0", "v 1 0 0", "v 0 1 0", "v 0 0 1",
                 "f 1 2 4", "f 1 4 3", "f 2 3 4"]
        obj.write_text("\n".join(lines) + "\n")

        s = meshrepair_module.RepairSession()
        s.load_file(str(obj))
        holes = s.detect_holes(preprocess=False)
        assert len(holes) == 1  # the missing bottom face

    def test_cancel_flag_is_harmless(self, meshrepair_module, open_box):
        data, nv, nf = open_box
        s = meshrepair_module.RepairSession()
        s.load_mesh_bytes(data, nv, nf)
        s.cancel()
        s.clear_cancel()
        stats = s.fill_holes(preprocess=False, max_diameter=1.0)
        assert stats["holes_filled"] == 1
