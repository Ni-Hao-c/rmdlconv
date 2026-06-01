import argparse
import csv
import glob
import hashlib
import json
import shutil
import struct
import subprocess
import sys
from pathlib import Path


SCRIPT_DIR = Path(__file__).resolve().parent
DEFAULT_RMDLCONV = SCRIPT_DIR / "bin" / "Release" / "rmdlconv.exe"
if not DEFAULT_RMDLCONV.exists():
    DEFAULT_RMDLCONV = SCRIPT_DIR / "src" / ".." / "bin" / "Release" / "rmdlconv.exe"
EXPORT_VG_OBJ = SCRIPT_DIR / "export_vg_obj.py"
COACD_WORKER = SCRIPT_DIR / "coacd_worker.py"

HDR_LENGTH = 80
HDR_COLLISION_OFFSET = 460
HDR_STATIC_COLLISION_COUNT = 464
HDR_VTX_OFFSET = 428
HDR_VVD_OFFSET = 432
HDR_VVC_OFFSET = 436


def read_i32_file(path: Path, offset: int) -> int:
    with path.open("rb") as f:
        f.seek(offset)
        return struct.unpack("<i", f.read(4))[0]


def read_i32(buf: bytes, offset: int) -> int:
    return struct.unpack_from("<i", buf, offset)[0]


def sha256(path: Path) -> str:
    h = hashlib.sha256()
    with path.open("rb") as f:
        for chunk in iter(lambda: f.read(1024 * 1024), b""):
            h.update(chunk)
    return h.hexdigest().upper()


def run(argv, cwd=None):
    result = subprocess.run(
        [str(x) for x in argv],
        cwd=cwd,
        text=True,
        capture_output=True,
    )
    if result.returncode != 0:
        detail = "\n".join(part for part in [result.stdout, result.stderr] if part)
        raise RuntimeError(f"command failed: {' '.join(str(x) for x in argv)}\n{detail}")
    return result.stdout.strip(), result.stderr.strip()


def python_has_coacd(candidate: Path) -> bool:
    try:
        result = subprocess.run(
            [str(candidate), "-c", "import coacd, numpy, trimesh"],
            text=True,
            capture_output=True,
            timeout=8,
        )
    except Exception:
        return False
    return result.returncode == 0


def iter_python_candidates():
    seen: set[str] = set()

    def add(path):
        if not path:
            return
        p = Path(path)
        key = str(p).lower()
        if key in seen:
            return
        seen.add(key)
        yield p

    import os

    for p in add(os.environ.get("RMDLCONV_COACD_PYTHON")):
        yield p
    for p in add(sys.executable):
        yield p
    for p in add(shutil.which("python")):
        yield p
    for pattern in [
        "C:/Program Files/Blender Foundation/Blender */*/python/bin/python.exe",
        "D:/Game/steams/steamapps/common/Blender/*/python/bin/python.exe",
        "D:/SteamLibrary/steamapps/common/Blender/*/python/bin/python.exe",
    ]:
        for match in sorted(glob.glob(pattern)):
            for p in add(match):
                yield p


def resolve_coacd_python(explicit: Path | None) -> Path:
    if explicit is not None:
        return explicit

    for candidate in iter_python_candidates():
        if candidate.exists() and python_has_coacd(candidate):
            return candidate

    raise RuntimeError(
        "Could not find a Python with coacd, numpy, and trimesh. "
        "Pass --python <path-to-blender-python.exe> or set RMDLCONV_COACD_PYTHON."
    )


def infer_model_root(input_path: Path) -> Path:
    if input_path.is_dir():
        return input_path
    current = input_path.parent
    while current != current.parent:
        if current.name.lower() == "models":
            return current
        current = current.parent
    return input_path.parent


def collect_models(input_path: Path, text_filter: str | None, limit: int) -> list[Path]:
    models: list[Path] = []
    if input_path.is_dir():
        for p in input_path.rglob("*.mdl"):
            if "_conv" in p.name.lower():
                continue
            if text_filter and text_filter not in str(p).lower():
                continue
            models.append(p)
            if limit > 0 and len(models) >= limit:
                break
    elif not text_filter or text_filter in str(input_path).lower():
        models.append(input_path)
    models.sort(key=lambda p: str(p).lower())
    return models[:limit] if limit > 0 else models


def relative_from_root(model_path: Path, model_root: Path) -> Path:
    try:
        return model_path.relative_to(model_root)
    except ValueError:
        return Path(model_path.name)


def safe_job_name(relative_path: Path) -> str:
    text = relative_path.with_suffix("").as_posix()
    return "".join(c if c.isalnum() or c in "_.-" else "_" for c in text)


def default_workdir(input_path: Path) -> Path:
    name = input_path.name if input_path.name else "models"
    if input_path.is_file():
        name = input_path.stem
    safe = "".join(c if c.isalnum() or c in "_.-" else "_" for c in name)
    return SCRIPT_DIR / f"_collision_work_{safe}"


def first_newest_file(directory: Path, suffix: str) -> Path | None:
    if not directory.exists():
        return None
    matches = [p for p in directory.iterdir() if p.name.lower().endswith(suffix.lower())]
    matches.sort(key=lambda p: p.stat().st_mtime, reverse=True)
    return matches[0] if matches else None


def ensure_v53(args, model_path: Path, job_dir: Path) -> Path:
    version = read_i32_file(model_path, 4)
    if version == 53:
        return model_path
    if version != 52:
        raise ValueError(f"expected v52 or v53 model, got v{version}: {model_path}")

    out_dir = job_dir / "v53"
    out_model = out_dir / model_path.name
    if out_model.exists() and not args.force:
        return out_model

    out_dir.mkdir(parents=True, exist_ok=True)
    run([
        args.rmdlconv,
        "-nopause",
        "-convertmodel",
        model_path,
        "-targetversion",
        "53",
        "-outputdir",
        out_dir,
    ])

    mdl_new = first_newest_file(out_dir, ".mdl_new")
    if mdl_new is None:
        raise RuntimeError(f"rmdlconv did not produce .mdl_new in {out_dir}")
    shutil.copyfile(mdl_new, out_model)
    return out_model


def make_visible_obj(args, v53_model: Path, job_dir: Path) -> Path:
    obj_path = job_dir / f"{v53_model.stem}.visible.obj"
    if obj_path.exists() and not args.force:
        return obj_path

    v54_dir = job_dir / "v54"
    v54_dir.mkdir(parents=True, exist_ok=True)
    run([
        args.rmdlconv,
        "-nopause",
        "-convertmodel",
        v53_model,
        "-targetversion",
        "54",
        "-outputdir",
        v54_dir,
    ])

    vg_path = v54_dir / f"{v53_model.stem}.vg"
    if not vg_path.exists():
        raise RuntimeError(f"expected VG output was not created: {vg_path}")
    run([args.python, EXPORT_VG_OBJ, vg_path, obj_path, "0"])
    return obj_path


def read_summary(path: Path) -> dict:
    buf = path.read_bytes()
    co = read_i32(buf, HDR_COLLISION_OFFSET)
    vtx = read_i32(buf, HDR_VTX_OFFSET)
    return {
        "path": str(path),
        "sha256": sha256(path),
        "version": read_i32(buf, 4),
        "length": read_i32(buf, HDR_LENGTH),
        "collisionOffset": co,
        "staticCollisionCount": read_i32(buf, HDR_STATIC_COLLISION_COUNT),
        "vtxOffset": vtx,
        "vvdOffset": read_i32(buf, HDR_VVD_OFFSET),
        "vvcOffset": read_i32(buf, HDR_VVC_OFFSET),
        "staticCollisionBytes": vtx - co,
    }


def write_reports(outdir: Path, rows: list[dict]) -> dict:
    outdir.mkdir(parents=True, exist_ok=True)
    json_path = outdir / "report.json"
    csv_path = outdir / "report.csv"
    json_path.write_text(json.dumps(rows, indent=2) + "\n", encoding="utf-8")

    headers = [
        "status",
        "sourceModel",
        "visibleObj",
        "finalModel",
        "staticCollisionCount",
        "staticCollisionBytes",
        "sha256",
        "error",
    ]
    with csv_path.open("w", newline="", encoding="utf-8") as f:
        writer = csv.writer(f)
        writer.writerow(headers)
        for row in rows:
            summary = row.get("summary") or {}
            writer.writerow([
                row.get("status", ""),
                row.get("sourceModel", ""),
                row.get("visibleObj", ""),
                row.get("finalModel", ""),
                summary.get("staticCollisionCount", ""),
                summary.get("staticCollisionBytes", ""),
                summary.get("sha256", ""),
                row.get("error", ""),
            ])
    return {"jsonPath": str(json_path), "csvPath": str(csv_path)}


def parse_args():
    parser = argparse.ArgumentParser()
    parser.add_argument("input")
    parser.add_argument("outdir")
    parser.add_argument("--workdir")
    parser.add_argument("--model-root")
    parser.add_argument("--filter")
    parser.add_argument("--limit", type=int, default=0)
    parser.add_argument("--rmdlconv", type=Path, default=DEFAULT_RMDLCONV)
    parser.add_argument("--python", type=Path)
    parser.add_argument("--coacd-worker", type=Path, default=COACD_WORKER)
    parser.add_argument("--threads", default="0")
    parser.add_argument("--dop", default="18")
    parser.add_argument("--margin", default="2")
    parser.add_argument("--max-hulls", default="32")
    parser.add_argument("--coacd-max-hulls", default="64")
    parser.add_argument("--coacd-threshold", default="0.03")
    parser.add_argument("--coacd-resolution", default="3000")
    parser.add_argument("--coacd-mcts-iterations", default="200")
    parser.add_argument("--coacd-max-ch-vertex", default="96")
    parser.add_argument("--force", action="store_true")
    return parser.parse_args()


def main():
    args = parse_args()
    input_path = Path(args.input).resolve()
    args.outdir = Path(args.outdir).resolve()
    args.workdir = Path(args.workdir).resolve() if args.workdir else default_workdir(input_path)
    args.model_root = Path(args.model_root).resolve() if args.model_root else infer_model_root(input_path)
    args.python = resolve_coacd_python(args.python)

    if not input_path.exists():
        raise FileNotFoundError(input_path)
    if not args.rmdlconv.exists():
        raise FileNotFoundError(args.rmdlconv)
    if not EXPORT_VG_OBJ.exists():
        raise FileNotFoundError(EXPORT_VG_OBJ)
    if not args.coacd_worker.exists():
        raise FileNotFoundError(args.coacd_worker)

    args.outdir.mkdir(parents=True, exist_ok=True)
    args.workdir.mkdir(parents=True, exist_ok=True)

    models = collect_models(input_path, args.filter.lower() if args.filter else None, args.limit)
    rows: list[dict] = []
    pipeline_jobs: list[tuple[Path, Path, Path]] = []
    jobs_path = args.workdir / "pipeline_jobs.tsv"

    for model_path in models:
        relative_path = relative_from_root(model_path, args.model_root)
        job_dir = args.workdir / safe_job_name(relative_path)
        final_model = args.outdir / relative_path
        job_dir.mkdir(parents=True, exist_ok=True)
        final_model.parent.mkdir(parents=True, exist_ok=True)

        try:
            v53_model = ensure_v53(args, model_path, job_dir)
            visible_obj = make_visible_obj(args, v53_model, job_dir)
            rows.append({
                "status": "prepared",
                "sourceModel": str(model_path),
                "relativeModelPath": relative_path.as_posix(),
                "v53Model": str(v53_model),
                "visibleObj": str(visible_obj),
                "finalModel": str(final_model),
                "jobDir": str(job_dir),
            })
            pipeline_jobs.append((v53_model, visible_obj, final_model))
        except Exception as exc:
            rows.append({
                "status": "error",
                "sourceModel": str(model_path),
                "relativeModelPath": relative_path.as_posix(),
                "finalModel": str(final_model),
                "jobDir": str(job_dir),
                "error": repr(exc),
            })
        write_reports(args.outdir, rows)

    jobs_path.write_text(
        "".join(f"{model}\t{visible}\t{out}\n" for model, visible, out in pipeline_jobs),
        encoding="utf-8",
    )

    if pipeline_jobs:
        argv = [
            args.rmdlconv,
            "-nopause",
            "-batchbuildstaticcollision",
            jobs_path,
            "-workdir",
            args.workdir / "_cpp_pipeline",
            "-python",
            args.python,
            "-coacdworker",
            args.coacd_worker,
            "-threads",
            args.threads,
            "-dop",
            args.dop,
            "-margin",
            args.margin,
            "-max-hulls",
            args.max_hulls,
            "-coacd-max-hulls",
            args.coacd_max_hulls,
            "-coacd-threshold",
            args.coacd_threshold,
            "-coacd-resolution",
            args.coacd_resolution,
            "-coacd-mcts-iterations",
            args.coacd_mcts_iterations,
            "-coacd-max-ch-vertex",
            args.coacd_max_ch_vertex,
        ]
        if args.force:
            argv.append("-force")
        run(argv, cwd=Path.cwd())

    for row in rows:
        if row["status"] != "prepared":
            continue
        final_model = Path(row["finalModel"])
        if not final_model.exists():
            row["status"] = "error"
            row["error"] = "C++ pipeline did not produce output model"
            continue
        row["status"] = "ok"
        row["summary"] = read_summary(final_model)

    report = write_reports(args.outdir, rows)
    ok = sum(1 for row in rows if row["status"] == "ok")
    print(json.dumps({
        "input": str(input_path),
        "modelRoot": str(args.model_root),
        "outdir": str(args.outdir),
        "workdir": str(args.workdir),
        "total": len(rows),
        "ok": ok,
        "errors": len(rows) - ok,
        "pipelineJobs": str(jobs_path),
        "report": report,
    }, indent=2))


if __name__ == "__main__":
    main()
