import argparse
import json
import traceback
from pathlib import Path

import coacd
import numpy as np
import trimesh


def write_hulls_obj(out_path: Path, hulls):
    lines = []
    vertex_base = 1
    for idx, (verts, faces) in enumerate(hulls):
        lines.append(f"o hull_{idx:03d}")
        for x, y, z in verts:
            lines.append(f"v {x} {y} {z}")
        for a, b, c in faces:
            lines.append(f"f {vertex_base + a} {vertex_base + b} {vertex_base + c}")
        vertex_base += len(verts)
    out_path.write_text("\n".join(lines) + "\n", encoding="utf-8")


def run_coacd_job(input_path: Path, output_path: Path, args):
    if output_path.exists() and not args.force:
        return {
            "status": "cached",
            "input_obj": str(input_path),
            "output_obj": str(output_path),
        }

    mesh = trimesh.load(input_path, force="mesh")
    coacd_mesh = coacd.Mesh(
        np.asarray(mesh.vertices, dtype=np.float64),
        np.asarray(mesh.faces, dtype=np.int32),
    )

    hulls = coacd.run_coacd(
        coacd_mesh,
        threshold=args.threshold,
        max_convex_hull=args.max_hulls,
        resolution=args.resolution,
        mcts_iterations=args.mcts_iterations,
        preprocess_mode="auto",
        preprocess_resolution=50,
        merge=True,
        decimate=False,
        max_ch_vertex=args.max_ch_vertex,
        extrude=False,
        pca=False,
        seed=0,
        apx_mode="ch",
        real_metric=False,
    )

    output_path.parent.mkdir(parents=True, exist_ok=True)
    write_hulls_obj(output_path, hulls)

    stats = {
        "input_obj": str(input_path),
        "output_obj": str(output_path),
        "threshold": args.threshold,
        "max_hulls": args.max_hulls,
        "resolution": args.resolution,
        "mcts_iterations": args.mcts_iterations,
        "max_ch_vertex": args.max_ch_vertex,
        "input_vertices": int(len(mesh.vertices)),
        "input_faces": int(len(mesh.faces)),
        "hull_count": int(len(hulls)),
        "hulls": [],
    }

    for idx, (verts, faces) in enumerate(hulls):
        verts = np.asarray(verts)
        bounds = None
        if len(verts):
            bounds = [verts.min(axis=0).tolist(), verts.max(axis=0).tolist()]
        stats["hulls"].append(
            {
                "index": idx,
                "vertices": int(len(verts)),
                "faces": int(len(faces)),
                "bounds": bounds,
            }
        )

    stats_path = output_path.with_suffix(".stats.json")
    stats_path.write_text(json.dumps(stats, indent=2) + "\n", encoding="utf-8")

    return {
        "status": "ok",
        "input_obj": str(input_path),
        "output_obj": str(output_path),
        "stats": str(stats_path),
        "hulls": int(len(hulls)),
    }


def read_jobs(path: Path):
    jobs = []
    for line_number, raw in enumerate(path.read_text(encoding="utf-8").splitlines(), start=1):
        line = raw.strip()
        if not line or line.startswith("#"):
            continue
        parts = line.split("\t")
        if len(parts) != 2:
            raise ValueError(f"{path}:{line_number}: expected input_obj<TAB>output_obj")
        jobs.append((Path(parts[0]).resolve(), Path(parts[1]).resolve()))
    return jobs


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--jobs", required=True)
    parser.add_argument("--report", required=True)
    parser.add_argument("--threshold", type=float, default=0.03)
    parser.add_argument("--max-hulls", type=int, default=64)
    parser.add_argument("--resolution", type=int, default=3000)
    parser.add_argument("--mcts-iterations", type=int, default=200)
    parser.add_argument("--max-ch-vertex", type=int, default=96)
    parser.add_argument("--force", action="store_true")
    args = parser.parse_args()

    jobs = read_jobs(Path(args.jobs).resolve())
    report_path = Path(args.report).resolve()
    report_path.parent.mkdir(parents=True, exist_ok=True)

    with report_path.open("w", encoding="utf-8") as report:
        for index, (input_path, output_path) in enumerate(jobs):
            try:
                row = run_coacd_job(input_path, output_path, args)
            except Exception as exc:
                row = {
                    "status": "error",
                    "input_obj": str(input_path),
                    "output_obj": str(output_path),
                    "error": str(exc),
                    "traceback": traceback.format_exc(),
                }
            row["index"] = index
            text = json.dumps(row, ensure_ascii=True)
            print(text, flush=True)
            report.write(text + "\n")
            report.flush()


if __name__ == "__main__":
    main()
