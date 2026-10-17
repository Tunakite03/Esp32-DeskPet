"""Build bounded RGB565 animation assets for the DeskPet data partition."""
from __future__ import annotations

import json
import struct
import zlib
from pathlib import Path

from PIL import Image, ImageChops

ROOT = Path(__file__).resolve().parents[1]  # sketch root; this script lives in tools/
ASSETS = ROOT / "assets"
HEADER = struct.Struct("<8sIIII")
ENTRY = struct.Struct("<24sHHIII")
FRAME = struct.Struct("<III")
LIMIT = 0x9E0000  # size of the "assets" partition in partitions.csv


def rgb565(image: Image.Image) -> list[int]:
    rgba = image.convert("RGBA")
    background = Image.new("RGBA", rgba.size, "black")
    background.alpha_composite(rgba)
    return [(r >> 3) << 11 | (g >> 2) << 5 | (b >> 3) for r, g, b, _ in background.getdata()]


def encode_runs(pixels: list[int], sample_format: str) -> bytes:
    output = bytearray()
    cursor = 0
    while cursor < len(pixels):
        end = cursor + 1
        while end < len(pixels) and pixels[end] == pixels[cursor] and end - cursor < 32767:
            end += 1
        if end - cursor >= 3:
            output += struct.pack("<H" + sample_format, 0x8000 | (end - cursor), pixels[cursor])
            cursor = end
            continue
        start = cursor
        cursor = end
        while cursor < len(pixels) and cursor - start < 32765:
            if cursor + 2 < len(pixels) and pixels[cursor] == pixels[cursor + 1] == pixels[cursor + 2]:
                break
            cursor += 1
        output += struct.pack("<H", cursor - start)
        output += struct.pack(f"<{cursor - start}{sample_format}", *pixels[start:cursor])
    return bytes(output)


def encode(pixels: list[int]) -> bytes:
    direct = b"\x00" + encode_runs(pixels, "H")
    palette = list(dict.fromkeys(pixels))
    if len(palette) > 256:
        return direct
    indices = {color: index for index, color in enumerate(palette)}
    indexed = b"\x01" + struct.pack("<H", len(palette)) + struct.pack(f"<{len(palette)}H", *palette)
    indexed += encode_runs([indices[color] for color in pixels], "B")
    return indexed if len(indexed) < len(direct) else direct


def decode(data: bytes) -> list[int]:
    output: list[int] = []
    cursor = 1
    palette: tuple[int, ...] = ()
    if data[0] == 1:
        count = struct.unpack_from("<H", data, cursor)[0]
        cursor += 2
        palette = struct.unpack_from(f"<{count}H", data, cursor)
        cursor += count * 2
    sample_format, width = ("B", 1) if palette else ("H", 2)
    while cursor < len(data):
        tag = struct.unpack_from("<H", data, cursor)[0]
        cursor += 2
        count = tag & 0x7FFF
        if tag & 0x8000:
            value = struct.unpack_from("<" + sample_format, data, cursor)[0]
            cursor += width
            output.extend([palette[value] if palette else value] * count)
        else:
            values = struct.unpack_from(f"<{count}{sample_format}", data, cursor)
            output.extend(palette[value] if palette else value for value in values)
            cursor += count * width
    return output


def mascot_frames(name: str) -> list[tuple[list[int], int]]:
    cells: list[Image.Image] = []
    coverage = Image.new("L", (360, 360))
    for mode in ("directions", "reactions"):
        sheet = Image.open(ASSETS / "page-mascot" / f"{name}-{mode}.webp").convert("RGBA")
        if sheet.size != (1080, 1080):
            raise ValueError(f"Unexpected sprite sheet size: {name}/{mode}")
        for index in range(9):
            x, y = index % 3 * 360, index // 3 * 360
            cell = sheet.crop((x, y, x + 360, y + 360))
            cells.append(cell)
            coverage = ImageChops.lighter(coverage, cell.getchannel("A"))
    bounds = coverage.point(lambda value: 255 if value > 24 else 0).getbbox()
    if bounds is None:
        raise ValueError(f"Empty sprite: {name}")
    side = min(360, max(bounds[2] - bounds[0], bounds[3] - bounds[1]) + 8)
    left = max(0, min(360 - side, (bounds[0] + bounds[2] - side) // 2))
    top = max(0, min(360 - side, (bounds[1] + bounds[3] - side) // 2))
    return [(rgb565(cell.crop((left, top, left + side, top + side)).resize((144, 144), Image.Resampling.LANCZOS)), 0) for cell in cells]


def main() -> None:
    groups: list[tuple[str, int, int, list[tuple[list[int], int]]]] = []
    # Sprite sheet name → name shown on the board (at most 12 characters).
    for species, title in (("cat", "Cat"), ("bunny", "Bunny"), ("dino", "DinoVip"), ("frog", "Frog")):
        groups.append((title, 0, 144, mascot_frames(species)))
    index_size = HEADER.size + len(groups) * ENTRY.size + sum(len(frames) for _, _, _, frames in groups) * FRAME.size
    payload = bytearray()
    directory = bytearray()
    entries = bytearray()
    frame_offset = HEADER.size + len(groups) * ENTRY.size
    manifest: list[dict[str, str | int]] = []
    decoded_crc = 0
    for title, kind, side, frames in groups:
        duration = frames[-1][1]
        directory += ENTRY.pack(title.encode().ljust(24, b"\0"), kind, side, len(frames), frame_offset, duration)
        before = len(payload)
        for pixels, end in frames:
            packed = encode(pixels)
            if decode(packed) != pixels:
                raise ValueError(f"RGB565 round-trip failed: {title}")
            decoded_crc = zlib.crc32(struct.pack(f"<{len(pixels)}H", *pixels), decoded_crc)
            entries += FRAME.pack(index_size + len(payload), len(packed), end)
            payload += packed
        frame_offset += len(frames) * FRAME.size
        manifest.append({"name": title, "side": side, "frames": len(frames), "duration_ms": duration, "bytes": len(payload) - before})
    license_text = (ASSETS / "page-mascot" / "LICENSE").read_bytes()
    body = directory + entries + payload + b"\npage-mascot attribution\n" + license_text
    total = HEADER.size + len(body)
    if total > LIMIT:
        raise ValueError(f"Asset pack exceeds partition: {total} > {LIMIT}")
    pack = HEADER.pack(b"DPETAS01", 1, total, zlib.crc32(body), len(groups)) + body
    (ASSETS / "pets.bin").write_bytes(pack)
    (ASSETS / "manifest.json").write_text(json.dumps({"bytes": total, "crc32": zlib.crc32(body), "decoded_crc32": decoded_crc, "assets": manifest}, indent=2), encoding="utf-8")
    print(json.dumps({"bytes": total, "capacity": LIMIT, "assets": manifest}, indent=2))


if __name__ == "__main__":
    main()
