"""FastAPI backend for the PCB Enclosure Generator SaaS."""

import os
import uuid
import zipfile
import shutil
from pathlib import Path
from typing import List, Optional

from fastapi import FastAPI, File, UploadFile, HTTPException
from fastapi.middleware.cors import CORSMiddleware
from fastapi.responses import FileResponse
from fastapi.staticfiles import StaticFiles
from pydantic import BaseModel, Field, validator

from pcb_parser import parse_gerber_zip, parse_step_file, PCBData, Hole, IOFeature
from enclosure_generator import generate_enclosure, EnclosureParams

app = FastAPI(title="PCB Enclosure Generator", version="1.0.0")

app.add_middleware(
    CORSMiddleware,
    allow_origins=["*"],
    allow_methods=["*"],
    allow_headers=["*"],
)

BASE_DIR = Path(__file__).parent
UPLOAD_DIR = BASE_DIR / "uploads"
UPLOAD_DIR.mkdir(exist_ok=True)

# In-memory job store (keyed by job_id)
jobs: dict[str, dict] = {}


# ---------------------------------------------------------------------------
# Pydantic models
# ---------------------------------------------------------------------------

class HoleDTO(BaseModel):
    x: float
    y: float
    diameter: float
    is_mounting: bool
    enabled: bool = True


class IOFeatureDTO(BaseModel):
    x: float
    y: float
    width: float
    height: float
    z_offset: float = 0.0
    cutout_height: float = 8.0
    label: str = ""
    enabled: bool = True
    side: str = "auto"   # "auto" | "left" | "right" | "bottom" | "top"


class AnalysisResult(BaseModel):
    job_id: str
    board_width: float
    board_height: float
    outline_points: List[List[float]]
    mounting_holes: List[HoleDTO]
    io_features: List[IOFeatureDTO]
    source_format: str


class GenerateRequest(BaseModel):
    job_id: str
    inner_height: float = Field(default=25.0, ge=5.0, le=200.0)
    wall_thickness: float = Field(default=2.4, ge=1.2, le=8.0)
    tolerance: float = Field(default=0.3, ge=0.0, le=2.0)
    pcb_standoff_height: float = Field(default=5.0, ge=2.0, le=30.0)
    insert_hole_diameter: float = Field(default=4.0, ge=2.0, le=8.0)
    floor_thickness: float = Field(default=1.2, ge=0.8, le=4.0)
    lid_thickness: float = Field(default=1.6, ge=0.8, le=4.0)
    mounting_holes: List[HoleDTO] = []
    io_features: List[IOFeatureDTO] = []


class GenerateResult(BaseModel):
    job_id: str
    files: List[dict]   # [{name, format, url}]


# ---------------------------------------------------------------------------
# Endpoints
# ---------------------------------------------------------------------------

@app.post("/api/upload", response_model=AnalysisResult)
async def upload_file(file: UploadFile = File(...)):
    """Accept a Gerber ZIP or STEP file, return extracted PCB geometry."""
    data = await file.read()
    filename = (file.filename or "").lower()

    try:
        if filename.endswith(".zip"):
            pcb = parse_gerber_zip(data)
        elif filename.endswith((".step", ".stp")):
            pcb = parse_step_file(data)
        else:
            raise HTTPException(status_code=400,
                                detail="Only .zip (Gerber) or .step/.stp files are supported.")
    except ValueError as exc:
        raise HTTPException(status_code=422, detail=str(exc))

    job_id = str(uuid.uuid4())
    job_dir = UPLOAD_DIR / job_id
    job_dir.mkdir()
    (job_dir / "source").write_bytes(data)

    jobs[job_id] = {"pcb": pcb, "status": "analysed", "dir": str(job_dir)}

    return AnalysisResult(
        job_id=job_id,
        board_width=pcb.width,
        board_height=pcb.height,
        outline_points=[[x, y] for x, y in pcb.outline_points],
        mounting_holes=[HoleDTO(**h.__dict__) for h in pcb.mounting_holes],
        io_features=[IOFeatureDTO(**f.__dict__) for f in pcb.io_features],
        source_format=pcb.source_format,
    )


@app.post("/api/generate", response_model=GenerateResult)
async def generate(req: GenerateRequest):
    """Generate the enclosure from the analysed PCB + user parameters."""
    if req.job_id not in jobs:
        raise HTTPException(status_code=404, detail="Job not found. Please upload a file first.")

    job = jobs[req.job_id]
    pcb: PCBData = job["pcb"]

    # Replace lists entirely so the user can also add manual entries
    pcb.mounting_holes = [Hole(
        x=h.x, y=h.y, diameter=h.diameter,
        is_mounting=h.is_mounting, enabled=h.enabled,
    ) for h in req.mounting_holes]

    pcb.io_features = [IOFeature(
        x=f.x, y=f.y, width=f.width, height=f.height,
        z_offset=f.z_offset, cutout_height=f.cutout_height,
        label=f.label, enabled=f.enabled, side=f.side,
    ) for f in req.io_features]

    params = EnclosureParams(
        inner_height=req.inner_height,
        wall_thickness=req.wall_thickness,
        tolerance=req.tolerance,
        pcb_standoff_height=req.pcb_standoff_height,
        insert_hole_diameter=req.insert_hole_diameter,
        floor_thickness=req.floor_thickness,
        lid_thickness=req.lid_thickness,
    )

    out_dir = str(Path(job["dir"]) / "output")
    os.makedirs(out_dir, exist_ok=True)

    try:
        paths = generate_enclosure(pcb, params, out_dir)
    except Exception as exc:
        raise HTTPException(status_code=500, detail=f"CAD generation failed: {exc}")

    job["status"] = "generated"
    job["output_paths"] = paths

    files = [
        {"name": "shell.stl",  "format": "STL",  "url": f"/api/download/{req.job_id}/shell.stl"},
        {"name": "shell.step", "format": "STEP", "url": f"/api/download/{req.job_id}/shell.step"},
        {"name": "lid.stl",    "format": "STL",  "url": f"/api/download/{req.job_id}/lid.stl"},
        {"name": "lid.step",   "format": "STEP", "url": f"/api/download/{req.job_id}/lid.step"},
    ]
    return GenerateResult(job_id=req.job_id, files=files)


@app.get("/api/download/{job_id}/{filename}")
async def download(job_id: str, filename: str):
    """Download a generated enclosure file."""
    if job_id not in jobs:
        raise HTTPException(status_code=404, detail="Job not found.")
    safe_name = Path(filename).name  # prevent path traversal
    file_path = Path(jobs[job_id]["dir"]) / "output" / safe_name
    if not file_path.exists():
        raise HTTPException(status_code=404, detail="File not yet generated.")
    return FileResponse(str(file_path), filename=safe_name)


# Serve frontend static files
frontend_dir = BASE_DIR.parent / "frontend"
if frontend_dir.exists():
    app.mount("/", StaticFiles(directory=str(frontend_dir), html=True), name="static")


if __name__ == "__main__":
    import uvicorn
    uvicorn.run("main:app", host="0.0.0.0", port=8000, reload=True)
