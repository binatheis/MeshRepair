# SPDX-License-Identifier: GPL-3.0-or-later
"""Shared fixtures: open-box binary mesh in the engine IPC format."""

import struct

import pytest


def parse_mesh_bytes(data):
    """Parse the engine binary mesh format: (nv, verts, nf, faces)."""
    (nv,) = struct.unpack_from("<I", data, 0)
    verts = struct.unpack_from(f"<{nv * 3}f", data, 4)
    (nf,) = struct.unpack_from("<I", data, 4 + nv * 12)
    faces = struct.unpack_from(f"<{nf * 3}I", data, 4 + nv * 12 + 4)
    return nv, verts, nf, faces


def open_box_bytes(size=1.0):
    """Open box (missing top face) as engine binary mesh bytes.

    Returns (data, vertex_count, face_count).
    """
    c = size
    verts = [
        0, 0, 0, c, 0, 0, c, c, 0, 0, c, 0,  # bottom
        0, 0, c, c, 0, c, c, c, c, 0, c, c,  # top
    ]
    faces = [
        0, 2, 1, 0, 3, 2,        # bottom
        0, 1, 5, 0, 5, 4,        # sides
        1, 2, 6, 1, 6, 5,
        2, 3, 7, 2, 7, 6,
        3, 0, 4, 3, 4, 7,
    ]
    nv, nf = len(verts) // 3, len(faces) // 3
    buf = struct.pack("<I", nv)
    buf += struct.pack(f"<{len(verts)}f", *verts)
    buf += struct.pack("<I", nf)
    buf += struct.pack(f"<{len(faces)}I", *faces)
    return buf, nv, nf


@pytest.fixture
def open_box():
    return open_box_bytes()


@pytest.fixture
def meshrepair_module():
    import meshrepair

    return meshrepair
