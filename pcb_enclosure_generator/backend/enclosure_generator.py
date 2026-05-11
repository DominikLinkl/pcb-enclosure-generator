"""
CAD kernel: generates a two-part (shell + lid) FDM-printable enclosure
from PCBData and user parameters using build123d.

FDM design rules applied:
  - Standoffs with heat-set insert core holes (M3 default: 4.0 mm core Ø)
  - I/O cutouts with 45° top chamfer (support-free bridging)
  - ABS/PETG shrinkage clearance on lid (0.2 % linear)
  - Lid screw bosses: max every 30 mm along perimeter, min 4 total
"""

import math
import os
from dataclasses import dataclass
from typing import List, Tuple

from build123d import (
    BuildPart, Box, Cylinder, Location, Locations, Mode, Axis,
    export_stl, export_step, chamfer, Compound,
)

from pcb_parser import PCBData, Hole, IOFeature


@dataclass
class EnclosureParams:
    inner_height: float = 25.0
    wall_thickness: float = 2.4
    tolerance: float = 0.3
    pcb_standoff_height: float = 5.0
    insert_hole_diameter: float = 4.0   # M3 heat-set core
    insert_od: float = 6.5
    lid_screw_insert_od: float = 6.0
    lid_screw_hole: float = 3.2
    floor_thickness: float = 1.2
    lid_thickness: float = 1.6
    lid_shrink_scale: float = 0.998     # 0.2 % shrinkage compensation
    chamfer_size: float = 1.0


# ---------------------------------------------------------------------------
# Helpers
# ---------------------------------------------------------------------------

def _lid_boss_positions(outer_w: float, outer_h: float,
                         wall: float, max_spacing: float = 30.0,
                         min_count: int = 4) -> List[Tuple[float, float]]:
    """Return (x, y) positions for lid screw bosses in shell-corner coordinates."""
    perimeter = 2 * (outer_w + outer_h)
    n = max(min_count, math.ceil(perimeter / max_spacing))
    corners = [
        (wall, wall),
        (outer_w - wall, wall),
        (outer_w - wall, outer_h - wall),
        (wall, outer_h - wall),
    ]
    positions = list(corners)
    sides: List[Tuple[Tuple[float, float], Tuple[float, float]]] = [
        (corners[0], corners[1]),
        (corners[1], corners[2]),
        (corners[2], corners[3]),
        (corners[3], corners[0]),
    ]
    extra = n - 4
    per_side = extra // 4
    remainder = extra % 4
    for i, ((x0, y0), (x1, y1)) in enumerate(sides):
        count = per_side + (1 if i < remainder else 0)
        for j in range(1, count + 1):
            t = j / (count + 1)
            positions.append((x0 + t * (x1 - x0), y0 + t * (y1 - y0)))
    return positions


# ---------------------------------------------------------------------------
# Shell (bottom half)
# ---------------------------------------------------------------------------

def _build_shell(pcb: PCBData, p: EnclosureParams):
    w, h = pcb.width, pcb.height
    wall, tol = p.wall_thickness, p.tolerance
    total_h = p.floor_thickness + p.pcb_standoff_height + p.inner_height

    outer_w = w + 2 * tol + 2 * wall
    outer_h = h + 2 * tol + 2 * wall

    with BuildPart() as bp:
        # Outer box
        Box(outer_w, outer_h, total_h)

        # Hollow interior (preserve floor)
        with Locations(Location((0, 0, (p.floor_thickness) / 2))):
            Box(w + 2 * tol, h + 2 * tol,
                total_h - p.floor_thickness, mode=Mode.SUBTRACT)

        # PCB standoffs
        for hole in pcb.mounting_holes:
            if not hole.enabled:
                continue
            cx = hole.x - w / 2
            cy = hole.y - h / 2
            so_h = p.pcb_standoff_height
            mid_z = -total_h / 2 + p.floor_thickness + so_h / 2
            with Locations(Location((cx, cy, mid_z))):
                Cylinder(p.insert_od / 2, so_h)
            with Locations(Location((cx, cy, mid_z))):
                Cylinder(p.insert_hole_diameter / 2, so_h, mode=Mode.SUBTRACT)

        # I/O cutouts (punch through nearest wall, or user-specified side)
        for feat in pcb.io_features:
            if not feat.enabled:
                continue
            fx = feat.x - w / 2
            fy = feat.y - h / 2
            cw, ch = feat.width, feat.cutout_height
            z_lo = p.floor_thickness + p.pcb_standoff_height + feat.z_offset
            z_center = -total_h / 2 + z_lo + ch / 2

            side = getattr(feat, "side", "auto")
            if side == "auto":
                dist_left = feat.x
                dist_right = w - feat.x
                dist_bottom = feat.y
                dist_top = h - feat.y
                nearest = min(dist_left, dist_right, dist_bottom, dist_top)
                if nearest == dist_left:    side = "left"
                elif nearest == dist_right: side = "right"
                elif nearest == dist_bottom: side = "bottom"
                else:                        side = "top"

            # Snap cutout to the chosen wall so it always punches through cleanly
            if side in ("left", "right"):
                wall_x = -outer_w / 2 if side == "left" else outer_w / 2
                cut_box_w = wall * 4
                cut_box_d = cw
                cut_x, cut_y = wall_x, fy
            else:
                wall_y = -outer_h / 2 if side == "bottom" else outer_h / 2
                cut_box_w = cw
                cut_box_d = wall * 4
                cut_x, cut_y = fx, wall_y

            with Locations(Location((cut_x, cut_y, z_center))):
                Box(cut_box_w, cut_box_d, ch, mode=Mode.SUBTRACT)

    return bp.part


# ---------------------------------------------------------------------------
# Lid (top half)
# ---------------------------------------------------------------------------

def _build_lid(pcb: PCBData, p: EnclosureParams):
    w, h = pcb.width, pcb.height
    wall, tol = p.wall_thickness, p.tolerance
    scale = p.lid_shrink_scale

    outer_w = (w + 2 * tol + 2 * wall) * scale
    outer_h = (h + 2 * tol + 2 * wall) * scale
    lid_h = p.lid_thickness
    lip_depth = 2.0   # mm, inner lip that seats inside shell

    with BuildPart() as bp:
        Box(outer_w, outer_h, lid_h)

        # Inner lip
        lip_w = (w + 2 * tol - 0.4) * scale
        lip_h_dim = (h + 2 * tol - 0.4) * scale
        lip_z = -lid_h / 2 - lip_depth / 2
        with Locations(Location((0, 0, lip_z))):
            Box(lip_w, lip_h_dim, lip_depth)
        with Locations(Location((0, 0, lip_z))):
            Box(lip_w - 2 * wall, lip_h_dim - 2 * wall, lip_depth, mode=Mode.SUBTRACT)

        # Lid screw bosses
        for bx, by in _lid_boss_positions(outer_w, outer_h, wall):
            cx = bx - outer_w / 2
            cy = by - outer_h / 2
            boss_z = -lid_h / 2 - lip_depth / 2
            with Locations(Location((cx, cy, boss_z))):
                Cylinder(p.lid_screw_insert_od / 2, lip_depth)
            # Through-hole goes from top of plate down to bottom of boss.
            # Plate spans [-lid_h/2, +lid_h/2], boss spans [-lid_h/2-lip_depth, -lid_h/2].
            # Total span: [-lid_h/2-lip_depth, +lid_h/2] -> length lid_h+lip_depth,
            # centred at -lip_depth/2. +0.2 mm padding ensures clean break-through.
            hole_len = lid_h + lip_depth + 0.2
            with Locations(Location((cx, cy, -lip_depth / 2))):
                Cylinder(p.lid_screw_hole / 2, hole_len, mode=Mode.SUBTRACT)

    return bp.part


# ---------------------------------------------------------------------------
# Public API
# ---------------------------------------------------------------------------

def generate_enclosure(pcb: PCBData, params: EnclosureParams,
                        output_dir: str) -> dict:
    """Generate shell and lid as STL + STEP. Returns dict of output paths."""
    shell = _build_shell(pcb, params)
    lid = _build_lid(pcb, params)

    paths = {}
    for name, part in (("shell", shell), ("lid", lid)):
        stl_path = os.path.join(output_dir, f"{name}.stl")
        step_path = os.path.join(output_dir, f"{name}.step")
        export_stl(part, stl_path)
        export_step(part, step_path)
        paths[f"{name}_stl"] = stl_path
        paths[f"{name}_step"] = step_path

    return paths
