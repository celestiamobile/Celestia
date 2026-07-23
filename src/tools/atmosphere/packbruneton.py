#!/usr/bin/env python3

"""Pack CosmoScout Bruneton RGB32F TIFF LUTs into one Celestia .atm file."""

from __future__ import annotations

import argparse
import struct
import sys
from dataclasses import dataclass
from pathlib import Path


MAGIC = b"CELATM\r\n"
VERSION = 1
ENDIAN_MARKER = 0x01020304
ENTRY_SIZE = 40
RGB32F = 1

TEXTURES = (
    (1, "phase.tif", 2),
    (2, "transmittance.tif", 2),
    (3, "indirect_illuminance.tif", 2),
    (4, "multiple_scattering.tif", 3),
    (5, "single_aerosols_scattering.tif", 3),
)
THETA_DEVIATION = (6, "theta_deviation.tif", 2)

TYPE_SIZES = {
    1: 1,   # BYTE
    2: 1,   # ASCII
    3: 2,   # SHORT
    4: 4,   # LONG
    5: 8,   # RATIONAL
    11: 4,  # FLOAT
    12: 8,  # DOUBLE
}


@dataclass(frozen=True)
class Texture:
    kind: int
    dimensions: int
    width: int
    height: int
    depth: int
    pixels: bytes


def _values(data: bytes, byte_order: str, field_type: int, count: int) -> tuple[int, ...]:
    formats = {1: "B", 3: "H", 4: "I"}
    try:
        return struct.unpack(byte_order + formats[field_type] * count, data)
    except KeyError as exc:
        raise ValueError(f"unsupported TIFF field type {field_type}") from exc


def _read_ifd(data: bytes, byte_order: str, offset: int) -> tuple[dict[int, tuple[int, ...]], int]:
    if offset + 2 > len(data):
        raise ValueError("truncated TIFF directory")

    count = struct.unpack_from(byte_order + "H", data, offset)[0]
    directory_end = offset + 2 + count * 12 + 4
    if directory_end > len(data):
        raise ValueError("truncated TIFF directory")

    fields: dict[int, tuple[int, ...]] = {}
    for index in range(count):
        entry_offset = offset + 2 + index * 12
        tag, field_type, value_count = struct.unpack_from(
            byte_order + "HHI", data, entry_offset
        )
        try:
            value_size = TYPE_SIZES[field_type] * value_count
        except KeyError as exc:
            raise ValueError(f"unsupported TIFF field type {field_type}") from exc

        value_or_offset = data[entry_offset + 8 : entry_offset + 12]
        if value_size <= 4:
            raw_value = value_or_offset[:value_size]
        else:
            value_offset = struct.unpack(byte_order + "I", value_or_offset)[0]
            if value_offset + value_size > len(data):
                raise ValueError("TIFF field points outside the file")
            raw_value = data[value_offset : value_offset + value_size]

        if field_type in (1, 3, 4):
            fields[tag] = _values(raw_value, byte_order, field_type, value_count)

    next_offset = struct.unpack_from(
        byte_order + "I", data, offset + 2 + count * 12
    )[0]
    return fields, next_offset


def read_tiff(path: Path, kind: int, dimensions: int) -> Texture:
    data = path.read_bytes()
    if len(data) < 8 or data[:2] not in (b"II", b"MM"):
        raise ValueError(f"{path}: invalid TIFF header")

    byte_order = "<" if data[:2] == b"II" else ">"
    if struct.unpack_from(byte_order + "H", data, 2)[0] != 42:
        raise ValueError(f"{path}: unsupported TIFF version")

    directory_offset = struct.unpack_from(byte_order + "I", data, 4)[0]
    pages: list[bytes] = []
    visited_directories: set[int] = set()
    width = height = 0
    while directory_offset != 0:
        if directory_offset in visited_directories:
            raise ValueError(f"{path}: cyclic TIFF directory")
        visited_directories.add(directory_offset)
        fields, directory_offset = _read_ifd(data, byte_order, directory_offset)

        page_width = fields.get(256, (0,))[0]
        page_height = fields.get(257, (0,))[0]
        if page_width == 0 or page_height == 0:
            raise ValueError(f"{path}: invalid TIFF dimensions")
        if width == 0:
            width, height = page_width, page_height
        elif (page_width, page_height) != (width, height):
            raise ValueError(f"{path}: TIFF pages have inconsistent dimensions")

        required = {
            258: (32, 32, 32),
            259: (1,),
            277: (3,),
            278: (1,),
            284: (1,),
            339: (3, 3, 3),
        }
        for tag, expected in required.items():
            if fields.get(tag) != expected:
                raise ValueError(f"{path}: unsupported TIFF tag {tag}")

        strip_offsets = fields.get(273)
        strip_sizes = fields.get(279)
        if strip_offsets is None or strip_sizes is None or len(strip_offsets) != len(strip_sizes):
            raise ValueError(f"{path}: invalid TIFF strip directory")

        page = bytearray()
        for strip_offset, strip_size in zip(strip_offsets, strip_sizes):
            if strip_offset + strip_size > len(data):
                raise ValueError(f"{path}: TIFF strip points outside the file")
            page.extend(data[strip_offset : strip_offset + strip_size])

        expected_size = width * height * 3 * 4
        if len(page) != expected_size:
            raise ValueError(f"{path}: unexpected TIFF pixel payload size")
        pages.append(bytes(page))

    expected_depth = len(pages) if dimensions == 3 else 1
    if not pages or (dimensions == 2 and len(pages) != 1):
        raise ValueError(f"{path}: unexpected TIFF page count")

    pixels = b"".join(pages)
    if byte_order == ">":
        values = struct.unpack(">" + "f" * (len(pixels) // 4), pixels)
        pixels = struct.pack("<" + "f" * len(values), *values)

    return Texture(kind, dimensions, width, height, expected_depth, pixels)


def align(value: int, alignment: int) -> int:
    return (value + alignment - 1) // alignment * alignment


def pack(source: Path, destination: Path) -> None:
    texture_specs = list(TEXTURES)
    if (source / THETA_DEVIATION[1]).is_file():
        texture_specs.append(THETA_DEVIATION)
    textures = [
        read_tiff(source / filename, kind, dimensions)
        for kind, filename, dimensions in texture_specs
    ]

    header_size = 24 + len(textures) * ENTRY_SIZE
    offset = align(header_size, 16)
    entries: list[bytes] = []
    for texture in textures:
        entries.append(
            struct.pack(
                "<IIIIIIQQ",
                texture.kind,
                texture.dimensions,
                texture.width,
                texture.height,
                texture.depth,
                RGB32F,
                offset,
                len(texture.pixels),
            )
        )
        offset = align(offset + len(texture.pixels), 16)

    destination.parent.mkdir(parents=True, exist_ok=True)
    with destination.open("wb") as output:
        output.write(
            struct.pack(
                "<8sIIII", MAGIC, VERSION, ENDIAN_MARKER, len(textures), ENTRY_SIZE
            )
        )
        output.write(b"".join(entries))
        for texture, entry in zip(textures, entries):
            payload_offset = struct.unpack_from("<Q", entry, 24)[0]
            output.write(b"\0" * (payload_offset - output.tell()))
            output.write(texture.pixels)

    print(f"Wrote {destination} ({destination.stat().st_size} bytes)")
    for texture, (_, filename, _) in zip(textures, texture_specs):
        print(
            f"  {filename}: {texture.width}x{texture.height}x{texture.depth} RGB32F"
        )


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("source", type=Path, help="CosmoScout atmosphere data directory")
    parser.add_argument("destination", type=Path, help="output .atm file")
    args = parser.parse_args()

    try:
        pack(args.source, args.destination)
    except (OSError, ValueError, struct.error) as exc:
        print(f"packbruneton: {exc}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
