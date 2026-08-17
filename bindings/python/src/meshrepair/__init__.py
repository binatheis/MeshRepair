# SPDX-License-Identifier: GPL-3.0-or-later
"""MeshRepair: CGAL-based hole filling with a partitioned parallel pipeline.

The compiled extension (``meshrepair_core_py``) is re-exported here so users
can simply ``import meshrepair``.
"""

from .meshrepair_core_py import (
    RepairSession,
    detect_holes_bytes,
    repair_file,
    repair_soup_bytes,
    version,
)

__version__ = version()

__all__ = [
    "RepairSession",
    "detect_holes_bytes",
    "repair_file",
    "repair_soup_bytes",
    "version",
    "__version__",
]
