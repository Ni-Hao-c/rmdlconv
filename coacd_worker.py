import argparse
import json
import os
import traceback
from concurrent.futures import ProcessPoolExecutor, as_completed
from pathlib import Path

import coacd
import numpy as np
import trimesh


def parse_obj_index(token: str, vertex_count: int) -> int:
    value_text = token.split("/", 1)[0]
    if not value_text:
        raise ValueError(f"OBJ face token has no vertex index: {token}")
    value = int(value_text)
    index = value - 1 if value > 0 else vertex_count + value
    if index < 0 or index >= vertex_count:
        raise ValueError(f"OBJ vertex index {value} is outside 1..{vertex_count}")
    return index


def load_obj_mesh(path: Path):
    vertices = []
    faces = []
    for line_number, raw in enumerate(path.read_text(encoding="utf-8", errors="replace").splitlines(), start=1):
        line = raw.strip()
        if not line or line.startswith("#"):
            continue
        parts = line.split()
        if parts[0] == "v":
            if len(parts) < 4:
                raise ValueError(f"{path}:{line_number}: vertex line needs at least 3 values")
            vertices.append([float(parts[1]), float(parts[2]), float(parts[3])])
        elif parts[0] == "f":
            if len(parts) < 4:
                continue
            indices = [parse_obj_index(token, len(vertices)) for token in parts[1:]]
            for i in range(1, len(indices) - 1):
                faces.append([indices[0], indices[i], indices[i + 1]])
    if not vertices or not faces:
        raise ValueError(f"OBJ has no triangle mesh: {path}")
    return np.asarray(vertices, dtype=np.float64), np.asarray(faces, dtype=np.int32)


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

    vertices, faces = load_obj_mesh(input_path)
    coacd_mesh = coacd.Mesh(vertices, faces)

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
        "input_vertices": int(len(vertices)),
        "input_faces": int(len(faces)),
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


def run_indexed_job(job):
    index, input_path, output_path, args = job
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
    return row


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--jobs", required=True)
    parser.add_argument("--report", required=True)
    parser.add_argument("--threshold", type=float, default=0.03)
    parser.add_argument("--max-hulls", type=int, default=64)
    parser.add_argument("--resolution", type=int, default=3000)
    parser.add_argument("--mcts-iterations", type=int, default=200)
    parser.add_argument("--max-ch-vertex", type=int, default=96)
    parser.add_argument("--workers", type=int, default=1)
    parser.add_argument("--force", action="store_true")
    args = parser.parse_args()

    jobs = read_jobs(Path(args.jobs).resolve())
    report_path = Path(args.report).resolve()
    report_path.parent.mkdir(parents=True, exist_ok=True)
    workers = args.workers if args.workers > 0 else (os.cpu_count() or 1)
    workers = max(1, min(workers, len(jobs) if jobs else 1))

    with report_path.open("w", encoding="utf-8") as report:
        if workers == 1:
            for job in ((index, input_path, output_path, args) for index, (input_path, output_path) in enumerate(jobs)):
                row = run_indexed_job(job)
                text = json.dumps(row, ensure_ascii=True)
                print(text, flush=True)
                report.write(text + "\n")
                report.flush()
        else:
            indexed_jobs = [
                (index, input_path, output_path, args)
                for index, (input_path, output_path) in enumerate(jobs)
            ]
            with ProcessPoolExecutor(max_workers=workers) as executor:
                futures = [executor.submit(run_indexed_job, job) for job in indexed_jobs]
                for future in as_completed(futures):
                    row = future.result()
                    text = json.dumps(row, ensure_ascii=True)
                    print(text, flush=True)
                    report.write(text + "\n")
                    report.flush()


if __name__ == "__main__":
    main()
