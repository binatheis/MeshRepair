# ##### BEGIN GPL LICENSE BLOCK #####
#
#  This program is free software; you can redistribute it and/or
#  modify it under the terms of the GNU General Public License
#  as published by the Free Software Foundation; either version 2
#  of the License, or (at your option) any later version.
#
# ##### END GPL LICENSE BLOCK #####

import bpy
import os
import platform
import stat
from bpy.types import Operator
from ..preferences import get_prefs


def _addon_root():
    return os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))


def _is_executable(path):
    if not path or not os.path.isfile(path):
        return False
    if platform.system() != "Windows":
        try:
            mode = os.stat(path).st_mode
            if not (mode & stat.S_IXUSR):
                os.chmod(path, mode | stat.S_IXUSR | stat.S_IXGRP | stat.S_IXOTH)
        except OSError:
            pass
    return os.access(path, os.X_OK) if platform.system() != "Windows" else os.path.isfile(path)


def default_engine_search_paths():
    addon_dir = _addon_root()
    system_name = platform.system()
    machine = platform.machine().lower()

    if system_name == "Windows":
        return [
            os.path.join(addon_dir, "bin", "windows", "x64", "meshrepair.exe"),
            "C:\\Program Files\\MeshRepair\\meshrepair.exe",
            os.path.join(addon_dir, "..", "build", "Release", "meshrepair.exe"),
        ]

    if system_name == "Linux":
        return [
            os.path.join(addon_dir, "bin", "linux", "x86_64", "meshrepair"),
            "/usr/local/bin/meshrepair",
            "/usr/bin/meshrepair",
            os.path.join(addon_dir, "..", "build", "meshrepair"),
        ]

    if system_name == "Darwin":
        arch_dir = "arm64" if machine in ("arm64", "aarch64") else "x86_64"
        return [
            os.path.join(addon_dir, "bin", "macos", "universal", "meshrepair"),
            os.path.join(addon_dir, "bin", "macos", arch_dir, "meshrepair"),
            "/Applications/MeshRepair.app/Contents/Helpers/meshrepair",
            "/usr/local/bin/meshrepair",
            os.path.join(addon_dir, "..", "build", "meshrepair"),
            os.path.join(addon_dir, "..", "build", "INSTALL", "bin", "meshrepair"),
        ]

    return []


def auto_detect_engine_path(prefs=None, update_prefs=True):
    if prefs is None:
        prefs = get_prefs()

    existing_path = os.path.abspath(os.path.expanduser(prefs.engine_path)) if getattr(prefs, "engine_path", "") else ""
    if existing_path and _is_executable(existing_path):
        if update_prefs:
            prefs.engine_path = existing_path
            prefs.engine_initialized = True
        return existing_path

    for path in default_engine_search_paths():
        path = os.path.abspath(os.path.expanduser(path))
        if _is_executable(path):
            if update_prefs:
                prefs.engine_path = path
                prefs.engine_initialized = True
                prefs.engine_version = ""
            return path

    if update_prefs:
        prefs.engine_initialized = False
    return ""


class MESHREPAIR_OT_detect_engine(Operator):
    """Detect meshrepair executable"""
    bl_idname = "meshrepair.detect_engine"
    bl_label = "Detect Engine"
    bl_description = "Automatically detect meshrepair executable"

    def execute(self, context):
        prefs = get_prefs()

        found_path = auto_detect_engine_path(prefs)
        if found_path:
            self.report({'INFO'}, f"Engine found: {found_path}")
        else:
            self.report({'WARNING'}, "Engine not found in standard locations")
            return {'CANCELLED'}

        return {'FINISHED'}


class MESHREPAIR_OT_test_engine(Operator):
    """Test engine connection"""
    bl_idname = "meshrepair.test_engine"
    bl_label = "Test Engine"
    bl_description = "Test connection to meshrepair"

    def execute(self, context):
        from ..engine.engine_session import EngineSession

        prefs = get_prefs()

        if not prefs.engine_path or not os.path.exists(prefs.engine_path):
            self.report({'ERROR'}, "Engine executable not found")
            return {'CANCELLED'}

        # Test engine by starting it and getting info
        try:
            session = EngineSession(
                prefs.engine_path,
                verbosity=int(prefs.verbosity_level),
                socket_mode=prefs.use_socket_mode,
                socket_host=prefs.socket_host,
                socket_port=prefs.socket_port,
                temp_dir=prefs.temp_dir
            )
            info = session.test()
            session.stop()

            # Extract info from response
            mesh_info = info.get('mesh_info', {})
            preprocessing_stats = info.get('preprocessing_stats', {})
            version = info.get('version', 'unknown')
            build_date = info.get('build_date', 'unknown')
            build_time = info.get('build_time', 'unknown')

            # Build info message
            msg = f"Engine test successful | "
            msg += f"Version: {version}, Built: {build_date} {build_time} | "
            msg += f"Mesh: {mesh_info.get('vertices', 0)} verts, {mesh_info.get('faces', 0)} faces"

            if preprocessing_stats:
                msg += f" | Preprocessing available"

            self.report({'INFO'}, msg)
            prefs.engine_initialized = True
            return {'FINISHED'}

        except Exception as ex:
            self.report({'ERROR'}, f"Engine test failed: {str(ex)}")
            prefs.engine_initialized = False
            return {'CANCELLED'}


class MESHREPAIR_OT_clear_cache(Operator):
    """Clear cache files"""
    bl_idname = "meshrepair.clear_cache"
    bl_label = "Clear Cache"
    bl_description = "Clear temporary cache files"

    def execute(self, context):
        # TODO: Implement cache clearing
        self.report({'INFO'}, "Cache cleared (stub)")
        return {'FINISHED'}


class MESHREPAIR_OT_reset_settings(Operator):
    """Reset all settings to defaults"""
    bl_idname = "meshrepair.reset_settings"
    bl_label = "Reset Settings"
    bl_description = "Reset all addon settings to default values"

    def execute(self, context):
        props = context.scene.meshrepair_props

        # Reset operation settings
        props.operation_mode = 'FULL'
        props.mesh_scope = 'SELECTION'
        props.selection_dilation = 0

        # Reset preprocessing
        props.enable_preprocessing = True
        props.preprocess_remove_duplicates = True
        props.preprocess_remove_non_manifold = True
        props.preprocess_remove_3_face_fans = True
        props.preprocess_remove_isolated = True
        props.preprocess_keep_largest = False
        props.preprocess_remove_long_edges = False
        props.preprocess_max_edge_ratio = 0.125
        props.preprocess_remove_thin_bridges = False
        props.preprocess_thin_bridge_max_hops = 2
        props.preprocess_thin_bridge_min_boundary_separation = 0
        props.preprocess_nm_passes = 10
        props.preprocess_duplicate_threshold = 0.0001

        # Reset filling
        props.filling_continuity = '1'
        props.filling_refine = True
        props.filling_use_2d_cdt = True
        props.filling_use_3d_delaunay = True
        props.filling_skip_cubic = False
        props.filling_use_partitioned = True
        props.filling_max_boundary = 1000
        props.filling_max_diameter_ratio = 0.1

        self.report({'INFO'}, "Settings reset to defaults")
        return {'FINISHED'}


classes = (
    MESHREPAIR_OT_detect_engine,
    MESHREPAIR_OT_test_engine,
    MESHREPAIR_OT_clear_cache,
    MESHREPAIR_OT_reset_settings,
)


def register():
    for cls in classes:
        bpy.utils.register_class(cls)


def unregister():
    for cls in reversed(classes):
        bpy.utils.unregister_class(cls)
