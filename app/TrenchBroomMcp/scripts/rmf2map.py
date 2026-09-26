#!/usr/bin/env python3
#
# Copyright (C) 2026 Nikita Rabykin
#
# This file is part of TrenchBroom.
#
# TrenchBroom is free software: you can redistribute it and/or modify
# it under the terms of the GNU General Public License as published by
# the Free Software Foundation, either version 3 of the License, or
# (at your option) any later version.
#
# TrenchBroom is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
# GNU General Public License for more details.
#
# You should have received a copy of the GNU General Public License
# along with TrenchBroom. If not, see <http://www.gnu.org/licenses/>.

"""Convert a Valve Hammer / Worldcraft RMF file to a TrenchBroom .map file.

TrenchBroom cannot read RMF, the binary format of Hammer 3.x and earlier. This
script converts it so that the map can be opened in TrenchBroom (and thus
edited through the MCP server).

Supported RMF versions:
  2.2  Hammer 3.4+: faces carry Valve 220 texture axes -> written as Valve format.
  1.8  Worldcraft / early Hammer (e.g. the Half-Life SDK 2.3 sample maps): faces
       carry Quake-style shift/rotation/scale -> written as Standard format.

Hammer visgroups become TrenchBroom layers (name, visibility and order are
kept), Hammer groups become TrenchBroom groups. Paths and editor cameras are
not converted.

Usage:
  rmf2map.py input.rmf [output.map] [--wad "a.wad;b.wad"]
"""

import argparse
import struct
import sys
from dataclasses import dataclass, field


@dataclass
class Face:
    points: list
    texture: str
    u_axis: tuple  # None for RMF 1.8
    v_axis: tuple  # None for RMF 1.8
    u_shift: float
    v_shift: float
    rotation: float
    u_scale: float
    v_scale: float


@dataclass
class Solid:
    visgroup: int
    faces: list


@dataclass
class Entity:
    visgroup: int
    classname: str
    properties: list
    children: list


@dataclass
class Group:
    visgroup: int
    children: list = field(default_factory=list)


@dataclass
class Visgroup:
    name: str
    id: int
    visible: bool


class Reader:
    def __init__(self, data):
        self.data = data
        self.pos = 0

    def unpack(self, fmt):
        values = struct.unpack_from("<" + fmt, self.data, self.pos)
        self.pos += struct.calcsize("<" + fmt)
        return values

    def int(self):
        return self.unpack("i")[0]

    def skip(self, count):
        self.pos += count

    def fixed_string(self, length):
        value = self.data[self.pos : self.pos + length].split(b"\0")[0]
        self.pos += length
        return value.decode("latin1")

    def string(self):
        return self.fixed_string(self.unpack("B")[0])


class RmfParser:
    def __init__(self, data):
        self.reader = Reader(data)

    def parse(self):
        r = self.reader
        self.version = round(r.unpack("f")[0], 1)
        if r.data[4:7] != b"RMF":
            raise ValueError("not an RMF file")
        r.skip(3)
        if self.version not in (1.8, 2.2):
            raise ValueError(f"unsupported RMF version {self.version}")

        visgroups = []
        for _ in range(r.int()):
            name = r.fixed_string(128)
            r.skip(4)  # color
            visgroup_id = r.int()
            visible = r.unpack("B")[0] != 0
            r.skip(3)
            visgroups.append(Visgroup(name, visgroup_id, visible))

        if r.string() != "CMapWorld":
            raise ValueError("missing CMapWorld")
        r.skip(7)  # visgroup, color
        children = self.objects()
        classname, properties = self.entity_data()
        return visgroups, children, properties

    def objects(self):
        r = self.reader
        result = []
        for _ in range(r.int()):
            object_type = r.string()
            if object_type == "CMapSolid":
                result.append(self.solid())
            elif object_type == "CMapEntity":
                visgroup = r.int()
                r.skip(3)  # color
                children = self.objects()
                classname, properties = self.entity_data()
                r.skip(14)
                origin = r.unpack("3f")
                r.skip(4)
                if not children:
                    properties.append(("origin", "%s %s %s" % tuple(map(fmt_num, origin))))
                result.append(Entity(visgroup, classname, properties, children))
            elif object_type == "CMapGroup":
                visgroup = r.int()
                r.skip(3)  # color
                result.append(Group(visgroup, self.objects()))
            else:
                raise ValueError(f"unknown object type {object_type!r} at offset {r.pos}")
        return result

    def solid(self):
        r = self.reader
        visgroup = r.int()
        r.skip(3 + 4)  # color, unused
        faces = []
        for _ in range(r.int()):
            texture = r.fixed_string(256)
            r.skip(4)
            if self.version < 2.0:
                rotation, u_shift, v_shift, u_scale, v_scale = r.unpack("5f")
                u_axis = v_axis = None
            else:
                *u_axis, u_shift = r.unpack("4f")
                *v_axis, v_shift = r.unpack("4f")
                rotation, u_scale, v_scale = r.unpack("3f")
            r.skip(16)
            r.skip(12 * r.int())  # vertices
            points = [r.unpack("3f") for _ in range(3)]
            faces.append(
                Face(
                    points,
                    texture,
                    u_axis,
                    v_axis,
                    u_shift,
                    v_shift,
                    rotation,
                    u_scale,
                    v_scale,
                )
            )
        return Solid(visgroup, faces)

    def entity_data(self):
        r = self.reader
        classname = r.string()
        r.skip(4)
        spawnflags = r.int()
        properties = [(r.string(), r.string()) for _ in range(r.int())]
        if spawnflags and not any(key == "spawnflags" for key, _ in properties):
            properties.append(("spawnflags", str(spawnflags)))
        return classname, properties


def fmt_num(value):
    text = ("%.6f" % value).rstrip("0").rstrip(".")
    return "0" if text in ("-0", "") else text


class MapWriter:
    def __init__(self, valve_format):
        self.valve_format = valve_format
        self.lines = []
        self.next_id = 1

    def allocate_id(self):
        result = self.next_id
        self.next_id += 1
        return result

    def property(self, key, value):
        self.lines.append('"%s" "%s"' % (key, value))

    def brush(self, solid):
        self.lines.append("{")
        for face in solid.faces:
            points = " ".join(
                "( %s )" % " ".join(fmt_num(c) for c in point) for point in face.points
            )
            if self.valve_format:
                u = " ".join(fmt_num(c) for c in (*face.u_axis, face.u_shift))
                v = " ".join(fmt_num(c) for c in (*face.v_axis, face.v_shift))
                uv = "[ %s ] [ %s ]" % (u, v)
            else:
                uv = "%s %s" % (fmt_num(face.u_shift), fmt_num(face.v_shift))
            self.lines.append(
                "%s %s %s %s %s %s"
                % (
                    points,
                    face.texture,
                    uv,
                    fmt_num(face.rotation),
                    fmt_num(face.u_scale),
                    fmt_num(face.v_scale),
                )
            )
        self.lines.append("}")

    def entity(self, entity, container_properties):
        self.lines.append("{")
        self.property("classname", entity.classname)
        for key, value in entity.properties + container_properties:
            self.property(key, value)
        for child in entity.children:
            if isinstance(child, Solid):
                self.brush(child)
        self.lines.append("}")


def convert(data, wad=None):
    parser = RmfParser(data)
    visgroups, children, world_properties = parser.parse()
    valve_format = parser.version >= 2.0
    writer = MapWriter(valve_format)

    layer_ids = {visgroup.id: writer.allocate_id() for visgroup in visgroups}

    def layer_of(visgroup):
        return layer_ids.get(visgroup)

    # Sort top level objects into the default layer and the visgroup layers.
    buckets = {None: []}
    buckets.update({layer_id: [] for layer_id in layer_ids.values()})
    for child in children:
        buckets[layer_of(child.visgroup)].append(child)

    # Groups and entities are written after their containing layer or group, so
    # collect them while writing brushes and flush them at the end.
    deferred = []

    def emit_contents(objects, container_properties):
        for child in objects:
            if isinstance(child, Solid):
                writer.brush(child)
            else:
                deferred.append((child, container_properties))

    def emit_deferred():
        while deferred:
            child, container_properties = deferred.pop(0)
            if isinstance(child, Entity):
                writer.entity(child, container_properties)
            else:
                group_id = writer.allocate_id()
                writer.lines.append("{")
                writer.property("classname", "func_group")
                writer.property("_tb_type", "_tb_group")
                writer.property("_tb_name", "group %d" % group_id)
                writer.property("_tb_id", group_id)
                for key, value in container_properties:
                    writer.property(key, value)
                emit_contents(child.children, [("_tb_group", str(group_id))])
                writer.lines.append("}")

    header = "Valve" if valve_format else "Standard"
    writer.lines += ["// Game: Half-Life", "// Format: " + header, "{"]
    writer.property("classname", "worldspawn")
    if wad:
        writer.property("wad", wad)
    for key, value in world_properties:
        if key == "classname" or (wad and key == "wad"):
            continue
        writer.property(key, value)
    if valve_format and not any(key == "mapversion" for key, _ in world_properties):
        writer.property("mapversion", "220")
    emit_contents(buckets[None], [])
    writer.lines.append("}")
    emit_deferred()

    for sort_index, visgroup in enumerate(visgroups):
        layer_id = layer_ids[visgroup.id]
        writer.lines.append("{")
        writer.property("classname", "func_group")
        writer.property("_tb_type", "_tb_layer")
        writer.property("_tb_name", visgroup.name or "visgroup %d" % visgroup.id)
        writer.property("_tb_id", layer_id)
        writer.property("_tb_layer_sort_index", sort_index)
        if not visgroup.visible:
            writer.property("_tb_layer_hidden", "1")
        emit_contents(buckets[layer_id], [("_tb_layer", str(layer_id))])
        writer.lines.append("}")
        emit_deferred()

    return parser.version, visgroups, "\n".join(writer.lines) + "\n"


def main():
    arg_parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    arg_parser.add_argument("input", help="RMF file to convert")
    arg_parser.add_argument("output", nargs="?", help="output .map file (default: input with .map)")
    arg_parser.add_argument("--wad", help="semicolon separated WAD list to store in worldspawn")
    args = arg_parser.parse_args()

    output = args.output or args.input.rsplit(".", 1)[0] + ".map"
    with open(args.input, "rb") as file:
        data = file.read()
    version, visgroups, text = convert(data, args.wad)
    with open(output, "w", encoding="latin1") as file:
        file.write(text)

    brushes = sum(line == "{" for line in text.splitlines()) - text.count('"classname"')
    print(
        f"{args.input}: RMF {version}, {len(visgroups)} visgroups, "
        f"{brushes} brushes -> {output}",
        file=sys.stderr,
    )


if __name__ == "__main__":
    main()
