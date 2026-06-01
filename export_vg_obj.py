import argparse
import json
import struct
from pathlib import Path


FLAGS_POSITION = 0x1
FLAGS_POSITION_PACKED = 0x2

HEADER = {
    "mesh_offset": 32,
    "mesh_count": 40,
    "index_offset": 48,
    "vert_offset": 64,
    "lod_offset": 112,
    "lod_count": 120,
}

MESH = {
    "size": 0x48,
    "flags": 0,
    "vert_offset": 8,
    "vert_cache_size": 12,
    "vert_count": 16,
    "index_offset": 32,
    "index_count": 36,
}

LOD = {
    "size": 8,
    "mesh_offset": 0,
    "mesh_count": 2,
    "switch_point": 4,
}


def u16(buf, off):
    return struct.unpack_from("<H", buf, off)[0]


def u32(buf, off):
    return struct.unpack_from("<I", buf, off)[0]


def u64(buf, off):
    return struct.unpack_from("<Q", buf, off)[0]


def f32(buf, off):
    return struct.unpack_from("<f", buf, off)[0]


def unpack_vector64(buf, off):
    packed = u64(buf, off)
    mask21 = (1 << 21) - 1
    mask22 = (1 << 22) - 1
    x = packed & mask21
    y = (packed >> 21) & mask21
    z = (packed >> 42) & mask22
    return [
        x * 0.0009765625 - 1024.0,
        y * 0.0009765625 - 1024.0,
        z * 0.0009765625 - 2048.0,
    ]


def read_position(buf, off, flags):
    if flags & FLAGS_POSITION:
        return [f32(buf, off), f32(buf, off + 4), f32(buf, off + 8)]
    if flags & FLAGS_POSITION_PACKED:
        return unpack_vector64(buf, off)
    raise ValueError(f"mesh at 0x{off:x} has no position flag")


def export_vg_obj(vg_path: Path, obj_path: Path, lod_level: int):
    buf = vg_path.read_bytes()

    mesh_base = u64(buf, HEADER["mesh_offset"])
    mesh_count = u64(buf, HEADER["mesh_count"])
    index_base = u64(buf, HEADER["index_offset"])
    vert_base = u64(buf, HEADER["vert_offset"])
    lod_base = u64(buf, HEADER["lod_offset"])
    lod_count = u64(buf, HEADER["lod_count"])

    if lod_level < 0 or lod_level >= lod_count:
        raise ValueError(f"lod {lod_level} out of range, file has {lod_count} lod(s)")

    lod_off = lod_base + lod_level * LOD["size"]
    lod_mesh_offset = u16(buf, lod_off + LOD["mesh_offset"])
    lod_mesh_count = u16(buf, lod_off + LOD["mesh_count"])
    switch_point = f32(buf, lod_off + LOD["switch_point"])

    lines = [
        f"# source: {vg_path}",
        f"# lod: {lod_level}",
        f"# switchPoint: {switch_point}",
    ]
    stats = {
        "vgPath": str(vg_path),
        "objPath": str(obj_path),
        "lodLevel": lod_level,
        "totalMeshCount": mesh_count,
        "lodCount": lod_count,
        "lodMeshOffset": lod_mesh_offset,
        "lodMeshCount": lod_mesh_count,
        "meshes": [],
    }

    vertex_base = 1
    for mesh_idx in range(lod_mesh_count):
        mesh_index = lod_mesh_offset + mesh_idx
        if mesh_index >= mesh_count:
            raise ValueError(f"mesh index {mesh_index} exceeds file mesh count {mesh_count}")

        mesh_off = mesh_base + mesh_index * MESH["size"]
        flags = u64(buf, mesh_off + MESH["flags"])
        vert_offset = u32(buf, mesh_off + MESH["vert_offset"])
        vert_cache_size = u32(buf, mesh_off + MESH["vert_cache_size"])
        vert_count = u32(buf, mesh_off + MESH["vert_count"])
        index_offset = u32(buf, mesh_off + MESH["index_offset"])
        index_count = u32(buf, mesh_off + MESH["index_count"])

        lines.append(f"o lod{lod_level}_mesh{mesh_index}")
        for i in range(vert_count):
            v_off = vert_base + vert_offset + i * vert_cache_size
            pos = read_position(buf, v_off, flags)
            lines.append(f"v {pos[0]} {pos[1]} {pos[2]}")

        indices = [u16(buf, index_base + index_offset + i * 2) for i in range(index_count)]
        index_min = min(indices) if indices else None
        index_max = max(indices) if indices else None
        uses_global_indices = index_max is not None and index_max >= vert_count
        tri_count = 0

        for i in range(0, index_count - 2, 3):
            a, b, c = indices[i], indices[i + 1], indices[i + 2]
            if a == b or b == c or a == c:
                continue
            face_base = 1 if uses_global_indices else vertex_base
            lines.append(f"f {face_base + a} {face_base + b} {face_base + c}")
            tri_count += 1

        stats["meshes"].append(
            {
                "meshIndex": mesh_index,
                "flags": f"0x{flags:x}",
                "vertOffset": vert_offset,
                "vertCacheSize": vert_cache_size,
                "vertCount": vert_count,
                "indexOffset": index_offset,
                "indexCount": index_count,
                "indexMin": index_min,
                "indexMax": index_max,
                "usesGlobalIndices": uses_global_indices,
                "triCount": tri_count,
            }
        )
        vertex_base += vert_count

    obj_path.parent.mkdir(parents=True, exist_ok=True)
    obj_path.write_text("\n".join(lines) + "\n", encoding="utf-8")
    stats_path = obj_path.with_suffix(".stats.json")
    stats_path.write_text(json.dumps(stats, indent=2) + "\n", encoding="utf-8")
    print(f"wrote {obj_path}")
    print(f"wrote {stats_path}")


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("vg_path")
    parser.add_argument("obj_path")
    parser.add_argument("lod", nargs="?", type=int, default=0)
    args = parser.parse_args()
    export_vg_obj(Path(args.vg_path).resolve(), Path(args.obj_path).resolve(), args.lod)


if __name__ == "__main__":
    main()
