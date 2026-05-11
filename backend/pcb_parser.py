"""
Parses Gerber ZIP archives and STEP files to extract:
  - Board outline (bounding box + polygon points)
  - Mounting holes (position + diameter, only holes > 2 mm)
  - I/O features near board edges (detected from drill file)
"""

import re
import math
import zipfile
import io
from dataclasses import dataclass, field
from typing import List, Tuple, Optional


EDGE_LAYER_NAMES = (
    "edge_cuts", "boardoutline", "outline", ".gko", ".gm1", ".gm-mechanical1",
    "board outline", "profile", ".gm2", ".gm3",
)

DRILL_FILE_EXTS = (".drl", ".exc", ".xln", ".ncd", ".drill")


@dataclass
class Hole:
    x: float
    y: float
    diameter: float
    is_mounting: bool  # True when diameter > 2 mm
    enabled: bool = True  # user can toggle off false-positives


@dataclass
class IOFeature:
    x: float
    y: float
    width: float
    height: float
    z_offset: float = 0.0   # mm above PCB surface
    cutout_height: float = 8.0  # mm, user-adjustable
    label: str = ""
    enabled: bool = True
    side: str = "auto"      # "auto" | "left" | "right" | "bottom" | "top"


@dataclass
class PCBData:
    width: float          # board X dimension in mm
    height: float         # board Y dimension in mm
    origin_x: float = 0.0
    origin_y: float = 0.0
    outline_points: List[Tuple[float, float]] = field(default_factory=list)
    mounting_holes: List[Hole] = field(default_factory=list)
    io_features: List[IOFeature] = field(default_factory=list)
    source_format: str = "gerber"


# ---------------------------------------------------------------------------
# Coordinate conversion helpers
# ---------------------------------------------------------------------------

def _parse_gerber_coord(raw: str, fmt_int: int, fmt_dec: int, zero_omit: str) -> float:
    """Convert a Gerber coordinate string to mm."""
    total_digits = fmt_int + fmt_dec
    if zero_omit == "L":
        raw = raw.zfill(total_digits + (1 if raw.startswith("-") else 0))
    else:
        if raw.startswith("-"):
            raw = "-" + raw[1:].ljust(total_digits, "0")
        else:
            raw = raw.ljust(total_digits, "0")
    value = int(raw)
    return value / (10 ** fmt_dec)


# ---------------------------------------------------------------------------
# Gerber edge layer parser
# ---------------------------------------------------------------------------

def _parse_edge_layer(text: str) -> List[Tuple[float, float]]:
    """Return a list of (x, y) mm points from a Gerber edge-cuts layer."""
    fmt_int, fmt_dec, zero_omit = 3, 5, "L"
    fmt_match = re.search(r"%FSLA?X(\d)(\d)Y\d\d\*%", text)
    if fmt_match:
        fmt_int, fmt_dec = int(fmt_match.group(1)), int(fmt_match.group(2))
    trail_match = re.search(r"%FSTL?X(\d)(\d)Y\d\d\*%", text)
    if trail_match:
        fmt_int, fmt_dec = int(trail_match.group(1)), int(trail_match.group(2))
        zero_omit = "T"

    points: List[Tuple[float, float]] = []
    cur_x, cur_y = 0.0, 0.0

    for line in text.splitlines():
        line = line.strip().rstrip("*")
        coord_match = re.match(r"^(X([+-]?\d+))?(Y([+-]?\d+))?D0?[12]$", line)
        if coord_match:
            if coord_match.group(2):
                cur_x = _parse_gerber_coord(coord_match.group(2), fmt_int, fmt_dec, zero_omit)
            if coord_match.group(4):
                cur_y = _parse_gerber_coord(coord_match.group(4), fmt_int, fmt_dec, zero_omit)
            points.append((cur_x, cur_y))

    return points


def _bounding_box(points: List[Tuple[float, float]]) -> Tuple[float, float, float, float]:
    xs = [p[0] for p in points]
    ys = [p[1] for p in points]
    return min(xs), min(ys), max(xs), max(ys)


# ---------------------------------------------------------------------------
# Excellon drill file parser
# ---------------------------------------------------------------------------

def _parse_drill_file(text: str) -> List[Hole]:
    """Parse an Excellon drill file and return Hole objects."""
    holes: List[Hole] = []
    tools: dict[str, float] = {}  # tool number -> diameter mm
    current_tool = None
    unit_mm = True
    metric_match = re.search(r"METRIC|INCH", text, re.IGNORECASE)
    if metric_match and "INCH" in metric_match.group().upper():
        unit_mm = False

    for line in text.splitlines():
        line = line.strip()
        tool_def = re.match(r"T(\d+)C([\d.]+)", line)
        if tool_def:
            dia = float(tool_def.group(2))
            if not unit_mm:
                dia *= 25.4
            tools[tool_def.group(1)] = dia
            continue

        tool_sel = re.match(r"^T(\d+)$", line)
        if tool_sel:
            current_tool = tool_sel.group(1)
            continue

        coord = re.match(r"X([+-]?[\d.]+)Y([+-]?[\d.]+)", line)
        if coord and current_tool and current_tool in tools:
            x = float(coord.group(1))
            y = float(coord.group(2))
            # Excellon coords can be integers (implicit decimal) — normalise
            if "." not in coord.group(1):
                x /= 1000.0 if unit_mm else 10000.0
            if "." not in coord.group(2):
                y /= 1000.0 if unit_mm else 10000.0
            if not unit_mm:
                x *= 25.4
                y *= 25.4
            dia = tools[current_tool]
            holes.append(Hole(x=x, y=y, diameter=dia, is_mounting=(dia > 2.0)))

    return holes


# ---------------------------------------------------------------------------
# I/O feature detection (large holes / slots near board edge)
# ---------------------------------------------------------------------------

_IO_EDGE_MARGIN_MM = 3.0   # hole center within this distance of board edge


def _detect_io_features(holes: List[Hole], board_x0: float, board_y0: float,
                         board_w: float, board_h: float) -> List[IOFeature]:
    """Holes > 2 mm near the board edge are treated as I/O cutout candidates."""
    features: List[IOFeature] = []
    x1, y1 = board_x0, board_y0
    x2, y2 = board_x0 + board_w, board_y0 + board_h

    for h in holes:
        if h.diameter <= 2.0:
            continue
        near_left = abs(h.x - x1) < _IO_EDGE_MARGIN_MM
        near_right = abs(h.x - x2) < _IO_EDGE_MARGIN_MM
        near_bottom = abs(h.y - y1) < _IO_EDGE_MARGIN_MM
        near_top = abs(h.y - y2) < _IO_EDGE_MARGIN_MM
        if near_left or near_right or near_bottom or near_top:
            features.append(IOFeature(
                x=h.x - board_x0,
                y=h.y - board_y0,
                width=h.diameter + 1.0,
                height=h.diameter + 1.0,
                z_offset=0.0,
                cutout_height=max(h.diameter + 2.0, 8.0),
                label=f"IO_{len(features)+1}",
            ))
    return features


# ---------------------------------------------------------------------------
# Public entry points
# ---------------------------------------------------------------------------

def parse_gerber_zip(data: bytes) -> PCBData:
    """Extract PCB geometry from a Gerber ZIP archive."""
    with zipfile.ZipFile(io.BytesIO(data)) as zf:
        names = zf.namelist()

        # Locate edge-cuts layer
        edge_file = None
        for name in names:
            lower = name.lower()
            if any(k in lower for k in EDGE_LAYER_NAMES):
                edge_file = name
                break

        # Locate drill file
        drill_file = None
        for name in names:
            if any(name.lower().endswith(ext) for ext in DRILL_FILE_EXTS):
                drill_file = name
                break

        outline_points: List[Tuple[float, float]] = []
        if edge_file:
            text = zf.read(edge_file).decode("utf-8", errors="replace")
            outline_points = _parse_edge_layer(text)

        holes: List[Hole] = []
        if drill_file:
            text = zf.read(drill_file).decode("utf-8", errors="replace")
            holes = _parse_drill_file(text)

    if not outline_points:
        raise ValueError("No Edge.Cuts / board outline layer found in ZIP.")

    x0, y0, x1, y1 = _bounding_box(outline_points)
    width = round(x1 - x0, 3)
    height = round(y1 - y0, 3)

    # Normalise outline to origin
    norm_pts = [(round(p[0] - x0, 3), round(p[1] - y0, 3)) for p in outline_points]

    # Normalise hole coordinates too
    norm_holes = [Hole(
        x=round(h.x - x0, 3),
        y=round(h.y - y0, 3),
        diameter=h.diameter,
        is_mounting=h.is_mounting,
    ) for h in holes]

    mounting_holes = [h for h in norm_holes if h.is_mounting]
    io_features = _detect_io_features(holes, x0, y0, width, height)

    return PCBData(
        width=width,
        height=height,
        origin_x=x0,
        origin_y=y0,
        outline_points=norm_pts,
        mounting_holes=mounting_holes,
        io_features=io_features,
        source_format="gerber",
    )


def parse_step_file(data: bytes) -> PCBData:
    """Extract bounding box from a STEP file using build123d."""
    try:
        import tempfile, os
        import build123d as _b

        # API name changed across versions
        _import_step = getattr(_b, "import_step", None) or getattr(_b, "import_step_file", None)
        if _import_step is None:
            raise ImportError("Cannot find import_step in build123d. Update to >= 0.6.")

        with tempfile.NamedTemporaryFile(suffix=".step", delete=False) as tmp:
            tmp.write(data)
            tmp_path = tmp.name
        try:
            shape = _import_step(tmp_path)
            bb = shape.bounding_box()
            width  = round(bb.size.X, 3)
            height = round(bb.size.Y, 3)
        finally:
            os.unlink(tmp_path)
    except Exception as exc:
        raise ValueError(f"Failed to parse STEP file: {exc}") from exc

    return PCBData(
        width=width,
        height=height,
        outline_points=[(0, 0), (width, 0), (width, height), (0, height)],
        mounting_holes=[],
        io_features=[],
        source_format="step",
    )
