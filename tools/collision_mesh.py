"""Export a simplified, spatially indexed wireframe from player-facing static collision."""
import argparse
import itertools
from collections import defaultdict
from pathlib import Path
import struct

import phive


CELL = 8
PER_CELL = 96
KINDS = {"floor": 0, "wall": 1, "ceiling": 2}


def add_triangles(cells, triangles, bounds):
    for tri in triangles:
        a, b, c, attrs = tri
        if not attrs["hitsPlayer"] or attrs["material"] == "AirWall":
            continue
        if bounds:
            x0, z0, x1, z1 = bounds
            if not (x0 <= a[0] <= x1 and z0 <= a[2] <= z1):
                continue
        kind = KINDS.get(phive.classify(tri))
        if kind is None:
            continue
        # Cluster the dense source mesh while retaining a shared grid across tile seams.
        step = (16, 8, 16) if kind == 0 else (8, 8, 8)
        verts = [tuple(round(v * 16 / s) * s for v, s in zip(point, step)) for point in (a, b, c)]
        if any(not -32768 <= v <= 32767 for point in verts for v in point):
            continue
        for p, q in itertools.combinations(sorted(verts), 2):
            length = sum((u - v) ** 2 for u, v in zip(p, q))
            if not 0 < length <= (CELL * 16 * 3) ** 2:
                continue
            key = ((p[0] + q[0]) // (CELL * 32), (p[2] + q[2]) // (CELL * 32))
            cells[key].add((p, q, kind))


def write_mesh(output, cells):
    index, body = bytearray(), bytearray()
    for (x, z), edges in sorted(cells.items()):
        # Round-robin surface types so a detailed floor cannot erase all walls.
        groups = [iter(sorted(e for e in edges if e[2] == k)) for k in KINDS.values()]
        selected = [edge for row in itertools.zip_longest(*groups) for edge in row if edge][:PER_CELL]
        index += struct.pack("<hhII", x, z, len(body) // 14, len(selected))
        for a, b, kind in selected:
            body += struct.pack("<6hH", *a, *b, kind)
    output.parent.mkdir(parents=True, exist_ok=True)
    output.write_bytes(struct.pack("<4sI", b"SGM1", len(cells)) + index + body)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("area")
    parser.add_argument("map", nargs="+")
    parser.add_argument("--output", type=Path, default=Path("data/mesh"))
    parser.add_argument("--bounds", nargs=4, type=float, metavar=("X0", "Z0", "X1", "Z1"))
    args = parser.parse_args()
    for map_name in args.map:
        cells = defaultdict(set)
        paths = phive.map_tiles(args.area, map_name)
        if not paths:
            parser.error(f"no collision tiles for {args.area}/{map_name}")
        for path in paths:
            tile = phive.decode_bphsc(phive.load(path))
            tris, _ = phive._tile_triangles(tile, everything=False)
            add_triangles(cells, tris, args.bounds)
        write_mesh(args.output / (map_name + ".mesh"), cells)
        print(f"{map_name}: {len(paths)} tiles, {len(cells)} occupied cells", flush=True)


if __name__ == "__main__":
    main()
