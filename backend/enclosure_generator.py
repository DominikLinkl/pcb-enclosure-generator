"""
CAD kernel: generates a two-part (shell + lid) FDM-printable enclosure
from PCBData and user parameters using build123d.

Architecture (continuous through-channel for heat-set inserts):

    [screw head]
   ┌──────────────┐  ← lid top
   │ lid plate    │     lid_thickness
   └─[clearance]──┘  ← lid bottom (clearance hole at standoff XY)
        │
        │ screw spans inner air gap
        │
    ┌──┴───┐         ← top of shell standoff
    │ ▒ ▒ ▒│         ← heat-set insert pocket (pocket_d × depth)
    │ │░░│ │  standoff
    │ │░░│ │            (PCB rests on standoff top, around the insert)
    └──────┘         ← bottom of standoff
    ────────         ← shell floor
   (floor_thickness)

Heat-set insert specs (M2..M8, "short" variant default):
    M2:    3.2 mm Ø pocket, 3.5 mm depth   (3.5 short / 4.5 std)
    M2.5:  3.8 mm Ø pocket, 4.5 mm depth   (4.5 short / 6.0 std)
    M3:    4.5 mm Ø pocket, 4.5 mm depth   (4.5 short / 6.0 std)
    M4:    5.8 mm Ø pocket, 5.0 mm depth   (5.0 short / 8.5 std)
    M5:    7.0 mm Ø pocket, 6.5 mm depth   (6.5 short / 10.0 std)
    M6:    8.6 mm Ø pocket, 7.5 mm depth   (7.5 short / 13.0 std)
    M8:   10.2 mm Ø pocket, 10.0 mm depth (10.0 short / 13.0 std)
"""

import math
import os
from dataclasses import dataclass
from typing import List, Tuple

from build123d import (
    BuildPart, Box, Cylinder, Location, Locations, Mode,
    export_stl, export_step,
)

from pcb_parser import PCBData, Hole, IOFeature


HEAT_SET_INSERTS = {
    "M2":   {"pocket_d": 3.2,  "depth_short": 3.5,  "depth_std": 4.5,  "screw_clearance_d": 2.4},
    "M2.5": {"pocket_d": 3.8,  "depth_short": 4.5,  "depth_std": 6.0,  "screw_clearance_d": 2.9},
    "M3":   {"pocket_d": 4.5,  "depth_short": 4.5,  "depth_std": 6.0,  "screw_clearance_d": 3.4},
    "M4":   {"pocket_d": 5.8,  "depth_short": 5.0,  "depth_std": 8.5,  "screw_clearance_d": 4.5},
    "M5":   {"pocket_d": 7.0,  "depth_short": 6.5,  "depth_std": 10.0, "screw_clearance_d": 5.5},
    "M6":   {"pocket_d": 8.6,  "depth_short": 7.5,  "depth_std": 13.0, "screw_clearance_d": 6.6},
    "M8":   {"pocket_d": 10.2, "depth_short": 10.0, "depth_std": 13.0, "screw_clearance_d": 9.0},
}


@dataclass
class EnclosureParams:
    inner_height: float = 25.0
    wall_thickness: float = 2.4
    tolerance: float = 0.3
    pcb_standoff_height: float = 6.0
    floor_thickness: float = 1.5
    lid_thickness: float = 1.6
    lid_shrink_scale: float = 0.998
    chamfer_size: float = 1.0
    # Heat-set insert
    screw_type: str = "M3"
    insert_short: bool = True
    boss_wall: float = 2.0
    lid_hole_clearance: float = 0.2

    @property
    def insert(self) -> dict:
        return HEAT_SET_INSERTS[self.screw_type]

    @property
    def pocket_depth(self) -> float:
        s = self.insert
        return s["depth_short"] if self.insert_short else s["depth_std"]

    @property
    def standoff_od(self) -> float:
        # Minimum 6 mm so even M2 has enough material to be printable.
        return max(self.insert["pocket_d"] + 2 * self.boss_wall, 6.0)

    @property
    def lid_hole_d(self) -> float:
        return self.insert["screw_clearance_d"] + self.lid_hole_clearance


# ---------------------------------------------------------------------------
# Helpers
# ---------------------------------------------------------------------------

def _fallback_corner_positions(outer_w: float, outer_h: float,
                                wall: float, boss_od: float) -> List[Tuple[float, float]]:
    """Return 4 corner XY positions (shell-frame, board-centered) for
    fallback standoffs when no PCB mounting holes are selected."""
    inset = wall + boss_od / 2 + 1.0
    return [
        (-outer_w / 2 + inset, -outer_h / 2 + inset),
        ( outer_w / 2 - inset, -outer_h / 2 + inset),
        ( outer_w / 2 - inset,  outer_h / 2 - inset),
        (-outer_w / 2 + inset,  outer_h / 2 - inset),
    ]


def _active_standoff_xy(pcb: PCBData, p: EnclosureParams,
                         outer_w: float, outer_h: float) -> List[Tuple[float, float]]:
    """Return the list of (x, y) standoff positions in shell-frame coords
    (board-centered). Uses active PCB mounting holes, or 4 corner fallback
    if none are active."""
    w, h = pcb.width, pcb.height
    xys: List[Tuple[float, float]] = []
    for hole in pcb.mounting_holes:
        if hole.enabled:
            xys.append((hole.x - w / 2, hole.y - h / 2))
    if not xys:
        xys = _fallback_corner_positions(outer_w, outer_h, p.wall_thickness, p.standoff_od)
    return xys


# ---------------------------------------------------------------------------
# Shell (bottom half)
# ---------------------------------------------------------------------------

def _build_shell(pcb: PCBData, p: EnclosureParams):
    w, h = pcb.width, pcb.height
    wall, tol = p.wall_thickness, p.tolerance
    floor = p.floor_thickness
    standoff_h = p.pcb_standoff_height
    pocket_depth = p.pocket_depth
    pocket_d = p.insert["pocket_d"]
    standoff_od = p.standoff_od

    # Grow the standoff if needed so the heat-set pocket fits with 0.8 mm
    # of solid material below it.
    effective_standoff_h = max(standoff_h, pocket_depth + 0.8)
    total_h = floor + effective_standoff_h + p.inner_height

    outer_w = w + 2 * tol + 2 * wall
    outer_h = h + 2 * tol + 2 * wall

    with BuildPart() as bp:
        # Outer box
        Box(outer_w, outer_h, total_h)

        # Hollow interior (preserve floor)
        with Locations(Location((0, 0, floor / 2))):
            Box(w + 2 * tol, h + 2 * tol,
                total_h - floor, mode=Mode.SUBTRACT)

        # Standoffs at every active mounting position (PCB holes, or fallback)
        positions = _active_standoff_xy(pcb, p, outer_w, outer_h)
        for cx, cy in positions:
            so_mid_z = -total_h / 2 + floor + effective_standoff_h / 2
            with Locations(Location((cx, cy, so_mid_z))):
                Cylinder(standoff_od / 2, effective_standoff_h)

            # Heat-set insert pocket — cut downward from standoff top
            pocket_top_z = -total_h / 2 + floor + effective_standoff_h
            pocket_mid_z = pocket_top_z - pocket_depth / 2
            with Locations(Location((cx, cy, pocket_mid_z))):
                Cylinder(pocket_d / 2, pocket_depth, mode=Mode.SUBTRACT)

        # I/O cutouts (punch through nearest wall, or user-specified side)
        for feat in pcb.io_features:
            if not feat.enabled:
                continue
            fx = feat.x - w / 2
            fy = feat.y - h / 2
            cw, ch = feat.width, feat.cutout_height
            z_lo = floor + effective_standoff_h + feat.z_offset
            z_center = -total_h / 2 + z_lo + ch / 2

            side = getattr(feat, "side", "auto")
            if side == "auto":
                dist_left = feat.x
                dist_right = w - feat.x
                dist_bottom = feat.y
                dist_top = h - feat.y
                nearest = min(dist_left, dist_right, dist_bottom, dist_top)
                if   nearest == dist_left:   side = "left"
                elif nearest == dist_right:  side = "right"
                elif nearest == dist_bottom: side = "bottom"
                else:                        side = "top"

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
    floor = p.floor_thickness
    effective_standoff_h = max(p.pcb_standoff_height, p.pocket_depth + 0.8)

    outer_w = (w + 2 * tol + 2 * wall) * scale
    outer_h = (h + 2 * tol + 2 * wall) * scale
    lid_h = p.lid_thickness
    lip_depth = 2.0   # mm, inner lip that seats inside shell

    # Lid frame: z=0 = lid plate center. Plate spans [-lid_h/2, +lid_h/2].
    # Lip extends below the plate, [-lid_h/2 - lip_depth, -lid_h/2].
    with BuildPart() as bp:
        Box(outer_w, outer_h, lid_h)

        # Inner lip (snap-fit seat into the shell)
        lip_w = (w + 2 * tol - 0.4) * scale
        lip_h_dim = (h + 2 * tol - 0.4) * scale
        lip_z = -lid_h / 2 - lip_depth / 2
        with Locations(Location((0, 0, lip_z))):
            Box(lip_w, lip_h_dim, lip_depth)
        with Locations(Location((0, 0, lip_z))):
            Box(lip_w - 2 * wall, lip_h_dim - 2 * wall,
                lip_depth, mode=Mode.SUBTRACT)

        # Clearance through-holes aligned to shell standoffs.
        # We use the same active-standoff list as the shell so they always
        # match up — this is the "continuous through-channel" the user wants.
        # Lid coords are scaled (lid_shrink_scale), so positions are too.
        # NB: outer_w/outer_h here are already the scaled lid outer; pass the
        # *unscaled* shell outer to the standoff resolver so the fallback
        # corner positions match the shell's corners.
        shell_outer_w = w + 2 * tol + 2 * wall
        shell_outer_h = h + 2 * tol + 2 * wall
        positions = _active_standoff_xy(pcb, p, shell_outer_w, shell_outer_h)

        hole_len = lid_h + lip_depth + 0.4   # +0.2 mm padding each side
        hole_z = -lip_depth / 2
        for cx, cy in positions:
            sx, sy = cx * scale, cy * scale
            with Locations(Location((sx, sy, hole_z))):
                Cylinder(p.lid_hole_d / 2, hole_len, mode=Mode.SUBTRACT)

    return bp.part


# ---------------------------------------------------------------------------
# Public API
# ---------------------------------------------------------------------------

def generate_enclosure(pcb: PCBData, params: EnclosureParams,
                        output_dir: str) -> dict:
    """Generate shell and lid as STL + STEP. Returns dict of output paths."""
    if params.screw_type not in HEAT_SET_INSERTS:
        raise ValueError(f"Unknown screw_type '{params.screw_type}'. "
                          f"Choose one of {list(HEAT_SET_INSERTS)}.")

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
