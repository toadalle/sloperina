"""Export verified baked box volumes from the user's matching 1.1.1 NUP.

Run with OOTMENU_NUP set, then run gendata.py. Only derived geometry leaves
this tool. Classify loading/abyss areas by game logic reference. Prefer baked
geometry and use the verified Area box convention for default-box placements.
Export baked static object boxes separately.
"""
from collections import Counter, defaultdict
import json
import math
import struct

import airwalls
import byml
import gamefiles
import phive
from pathlib import Path

OUT = Path(__file__).resolve().parents[1] / "data/scene_volumes.json"
LOAD_LOGIC = "Common_FieldDungeonEntranceSet_bad1.logic.module.ain"
VOID_LOGIC = {
    "Gimmick_AreaContact_FallDown_0a27.logic.module.ain",
    "Gimmick_AreaContact_FallDownMountian_3b16.logic.module.ain",
    "Gimmick_AreaContact_FallDown_GanonCastle_b627.logic.module.ain",
}
BUILD = bytes.fromhex("E0F3CCFAF50065AE4EA01EB5AEB13B1A45DA3348")


def logic_areas(paths):
    documents = [byml.load(gamefiles.read(path)) for path in paths]
    actors = {a["Hash"]: a for doc in documents for a in doc.get("Actors", [])}
    areas = {}
    for doc in documents:
        for group in doc.get("AiGroups", []):
            logic = group.get("Logic", "").rsplit("/", 1)[-1]
            if logic != LOAD_LOGIC and logic not in VOID_LOGIC:
                continue
            for ref in group.get("References", []):
                actor = actors.get(ref["Reference"])
                if actor and actor.get("Gyaml") == "Area" and actor.get("Translate"):
                    areas[actor["Hash"]] = ("loading" if logic == LOAD_LOGIC else "void", actor)
    return areas


def tile_boxes(data, areas):
    _, sections = phive.phive_sections(data)
    hdr, _ = sections[0]
    n_actors, n_info, n_materials = struct.unpack_from("<3I", data, hdr + phive.BPHSC_COUNTS_AT + 16)

    def records(section, fmt, count):
        return [struct.unpack_from(fmt, data, sections[section][0] + i * struct.calcsize(fmt))
                for i in range(count)]

    actor_hashes = {r[2] for r in records(1, "<IIQI12x", n_actors)}
    info = records(4, "<I4BHHII", n_info)
    materials = records(5, "<IIQ", n_materials)
    masks = records(6, "<QQ", n_materials)
    pending = {h: area for h, area in areas.items() if h in actor_hashes}
    seen_layers = set()
    for offset, size in sections[11:]:
        if not size or not phive.is_tagfile(data, offset):
            continue
        tf = phive.Tagfile(data, offset)
        root, kind = tf.root()
        if kind != "hknpCompoundShape":
            continue
        instances = phive.compound_instances(data, root)
        if not instances or instances[0]["shapeTag"] >= len(info):
            continue
        layer_key = tuple(info[instances[0]["shapeTag"]][1:3])
        extra = layer_key in seen_layers and not layer_key[0]
        seen_layers.add(layer_key)
        if extra:
            continue
        for inst in instances:
            if tf.objects.get(inst["shape"]) != "hknpBoxShape" or inst["shapeTag"] >= len(info):
                continue
            _, sensor, layer, _, _, _, _, first, count = info[inst["shapeTag"]]
            if sensor and phive.name_of(phive.LAYERS_SENSOR, layer) == "StaticGeneralArea":
                matching = [(h, name) for h, (name, a) in pending.items() if area_matches(inst, a)]
                if len(matching) != 1:
                    continue
                actor_hash, category = matching[0]
                del pending[actor_hash]
            elif not sensor and phive.name_of(phive.LAYERS_ENTITY, layer) in ("Object", "GroundObject"):
                indices = range(first, min(first + count, len(materials)))
                if not any(materials[i][0] != phive.MATERIALS.index("AirWall") and
                           masks[i][0] & phive.PLAYER_LAYER_BIT for i in indices):
                    continue
                actor_hash, category = None, "hitbox"
            else:
                continue
            box = box_geometry(data, inst, category)
            if box:
                yield actor_hash, box


def area_matches(inst, actor):
    scale = airwalls._scale_of(actor)
    if any(abs(baked - 2 * placed) > .02 for baked, placed in zip(inst["s"], scale)):
        return False
    offset = phive.quat_rotate(inst["q"], (0, scale[1], 0))
    return all(abs(centre - origin - delta) < .025
               for centre, origin, delta in zip(inst["t"], actor["Translate"], offset))


def box_geometry(data, inst, category):
    shape = inst["shape"]
    bounds = phive.bbox(phive.convex_shape(data, shape))
    if not bounds:
        return None
    radius = struct.unpack_from("<f", data, shape + phive.CONVEX_RADIUS)[0]
    qx, qy, qz, qw = inst["q"]
    rotation = [math.atan2(2 * (qw*qx + qy*qz), 1 - 2 * (qx*qx + qy*qy)),
                math.asin(max(-1, min(1, 2 * (qw*qy - qz*qx)))),
                math.atan2(2 * (qw*qz + qx*qy), 1 - 2 * (qy*qy + qz*qz))]
    low, high = bounds
    centre = [(a + b) / 2 for a, b in zip(low, high)]
    half = [(b - a) / 2 + radius for a, b in zip(low, high)]
    if not all(math.isfinite(v) for v in (*inst["s"], *inst["t"], *half, *centre, *rotation)):
        return None
    return {"kind": category, "t": inst["t"], "r": rotation, "s": inst["s"],
            "shapes": [{"type": "box", "center": centre, "half": half}]}


def main():
    nup = gamefiles.nup()
    _, _, _, offset = nup.index["exefs/main"]
    nup.f.seek(offset + 0x40)
    if nup.f.read(len(BUILD)) != BUILD:
        raise SystemExit("Use the matching OoT 1.1.1 NUP")
    layouts = defaultdict(list)
    tiles = defaultdict(list)
    for path in nup.index:
        file = path.rsplit("/", 1)[-1]
        if path.startswith("romfs/Banc/") and file.endswith(airwalls.LAYOUT_SUFFIX):
            name = file[:-len(airwalls.LAYOUT_SUFFIX)].removesuffix("_Static").removesuffix("_Dynamic")
            layouts[name].append(path)
        if path.startswith("romfs/Phive/StaticCompoundBody/") and file.endswith(".bphsc.zs"):
            name = file.split(".")[0]
            parts = name.rsplit("_", 2)
            if len(parts) == 3 and parts[1].startswith("X") and parts[2].startswith("Z"):
                name = parts[0]
            tiles[name].append(path)
    maps = {}
    matched, discovered = Counter(), Counter()
    for name, paths in sorted(layouts.items()):
        areas = logic_areas(paths)
        discovered.update(kind for kind, _ in areas.values())
        volumes = []
        seen = set()
        for tile in sorted(tiles[name]):
            for actor_hash, box in tile_boxes(gamefiles.read(tile), areas):
                if actor_hash is not None:
                    if actor_hash in seen:
                        continue
                    seen.add(actor_hash)
                volumes.append(box)
                matched[box["kind"]] += 1
        for actor_hash, (kind, actor) in areas.items():
            if actor_hash in seen or actor.get("Dynamic", {}).get("RigidBodyShapeType", 1) != 1:
                continue
            scale = airwalls._scale_of(actor)
            if len(scale) != 3 or not all(math.isfinite(v) and v > 0 for v in scale):
                continue
            # The default Area box is two units across and sits on its local
            # origin, as seen in the matched baked boxes (e.g. Dodongo Cavern).
            volumes.append({"kind": kind, "t": actor["Translate"], "r": actor.get("Rotate") or [0, 0, 0],
                            "s": scale, "shapes": [{"type": "box", "center": [0, 1, 0], "half": [1, 1, 1]}]})
            matched[kind] += 1
        if volumes:
            maps[name] = sorted(volumes, key=lambda box: box["kind"] == "hitbox")
    OUT.write_text(json.dumps({"source": "1.1.1 baked Phive box volumes", "maps": maps},
                              separators=(",", ":")) + "\n", encoding="utf-8")
    print(f"{len(maps)} maps, {dict(matched)}. Logic areas found: {dict(discovered)}.")


if __name__ == "__main__":
    main()
