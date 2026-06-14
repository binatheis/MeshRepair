#!/usr/bin/env python3
import os
import shutil
import struct
import zlib


SIZES = (16, 24, 32, 48, 64, 128, 256, 512, 1024)


def read_chunks(path):
    with open(path, "rb") as f:
        data = f.read()
    if data[:8] != b"\x89PNG\r\n\x1a\n":
        raise RuntimeError(f"{path}: not a PNG")
    pos = 8
    chunks = []
    while pos < len(data):
        length = struct.unpack(">I", data[pos:pos + 4])[0]
        kind = data[pos + 4:pos + 8]
        payload = data[pos + 8:pos + 8 + length]
        chunks.append((kind, payload))
        pos += 12 + length
        if kind == b"IEND":
            break
    return chunks


def paeth(a, b, c):
    p = a + b - c
    pa = abs(p - a)
    pb = abs(p - b)
    pc = abs(p - c)
    if pa <= pb and pa <= pc:
        return a
    if pb <= pc:
        return b
    return c


def unfilter(raw, width, height, bpp):
    stride = width * bpp
    rows = []
    pos = 0
    prev = bytearray(stride)
    for _ in range(height):
        filter_type = raw[pos]
        pos += 1
        row = bytearray(raw[pos:pos + stride])
        pos += stride
        for i in range(stride):
            left = row[i - bpp] if i >= bpp else 0
            up = prev[i]
            up_left = prev[i - bpp] if i >= bpp else 0
            if filter_type == 1:
                row[i] = (row[i] + left) & 0xff
            elif filter_type == 2:
                row[i] = (row[i] + up) & 0xff
            elif filter_type == 3:
                row[i] = (row[i] + ((left + up) >> 1)) & 0xff
            elif filter_type == 4:
                row[i] = (row[i] + paeth(left, up, up_left)) & 0xff
            elif filter_type != 0:
                raise RuntimeError(f"unsupported PNG filter {filter_type}")
        rows.append(bytes(row))
        prev = row
    return rows


def decode_png_rgba(path):
    chunks = read_chunks(path)
    width = height = bit_depth = color_type = None
    palette = None
    transparency = b""
    compressed = []

    for kind, payload in chunks:
        if kind == b"IHDR":
            width, height, bit_depth, color_type, compression, filter_method, interlace = struct.unpack(">IIBBBBB", payload)
            if compression != 0 or filter_method != 0 or interlace != 0 or bit_depth != 8:
                raise RuntimeError(f"{path}: unsupported PNG encoding")
        elif kind == b"PLTE":
            palette = [tuple(payload[i:i + 3]) for i in range(0, len(payload), 3)]
        elif kind == b"tRNS":
            transparency = payload
        elif kind == b"IDAT":
            compressed.append(payload)

    raw = zlib.decompress(b"".join(compressed))
    if color_type == 3:
        if palette is None:
            raise RuntimeError(f"{path}: indexed PNG has no palette")
        rows = unfilter(raw, width, height, 1)
        rgba_rows = []
        for row in rows:
            out = bytearray()
            for idx in row:
                r, g, b = palette[idx]
                a = transparency[idx] if idx < len(transparency) else 255
                out.extend((r, g, b, a))
            rgba_rows.append(bytes(out))
        return width, height, rgba_rows
    if color_type == 2:
        rows = unfilter(raw, width, height, 3)
        return width, height, [b"".join(bytes((row[i], row[i + 1], row[i + 2], 255)) for i in range(0, len(row), 3)) for row in rows]
    if color_type == 6:
        return width, height, unfilter(raw, width, height, 4)
    raise RuntimeError(f"{path}: unsupported PNG color type {color_type}")


def png_chunk(kind, payload):
    return struct.pack(">I", len(payload)) + kind + payload + struct.pack(">I", zlib.crc32(kind + payload) & 0xffffffff)


def write_rgba_png(path, width, height, rgba_rows):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    raw = b"".join(b"\x00" + row for row in rgba_rows)
    payload = struct.pack(">IIBBBBB", width, height, 8, 6, 0, 0, 0)
    with open(path, "wb") as f:
        f.write(b"\x89PNG\r\n\x1a\n")
        f.write(png_chunk(b"IHDR", payload))
        f.write(png_chunk(b"IDAT", zlib.compress(raw, 9)))
        f.write(png_chunk(b"IEND", b""))


def write_ico(path, png_paths):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    entries = []
    payloads = []
    offset = 6 + 16 * len(png_paths)
    for png_path in png_paths:
        width, height, _ = decode_png_rgba(png_path)
        with open(png_path, "rb") as f:
            data = f.read()
        entries.append((width, height, len(data), offset))
        payloads.append(data)
        offset += len(data)

    with open(path, "wb") as f:
        f.write(struct.pack("<HHH", 0, 1, len(entries)))
        for width, height, size, offset in entries:
            f.write(struct.pack("<BBBBHHII", 0 if width == 256 else width, 0 if height == 256 else height, 0, 0, 1, 32, size, offset))
        for data in payloads:
            f.write(data)


def write_icns(path, png_paths_by_type):
    chunks = []
    for icon_type, png_path in png_paths_by_type:
        with open(png_path, "rb") as f:
            data = f.read()
        chunks.append(icon_type.encode("ascii") + struct.pack(">I", len(data) + 8) + data)
    payload = b"".join(chunks)
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "wb") as f:
        f.write(b"icns")
        f.write(struct.pack(">I", len(payload) + 8))
        f.write(payload)


def main():
    root = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))
    logo_dir = os.path.join(root, "logo")
    generated_dir = os.path.join(root, "build", "generated-icons")

    for size in SIZES:
        src = os.path.join(logo_dir, f"MeshRepair_{size}.png")
        width, height, rgba_rows = decode_png_rgba(src)
        if width != size or height != size:
            raise RuntimeError(f"{src}: expected {size}x{size}, got {width}x{height}")
        write_rgba_png(os.path.join(generated_dir, f"MeshRepair_{size}.png"), width, height, rgba_rows)

    ico_pngs = [os.path.join(generated_dir, f"MeshRepair_{size}.png") for size in (16, 24, 32, 48, 64, 128, 256)]
    write_ico(os.path.join(root, "meshrepair-gui", "resources", "meshrepair.ico"), ico_pngs)

    for size in SIZES:
        dst = os.path.join(root, "packaging", "linux", "icons", "hicolor", f"{size}x{size}", "apps", "meshrepair.png")
        os.makedirs(os.path.dirname(dst), exist_ok=True)
        shutil.copyfile(os.path.join(generated_dir, f"MeshRepair_{size}.png"), dst)

    iconset = os.path.join(generated_dir, "MeshRepair.iconset")
    os.makedirs(iconset, exist_ok=True)
    mapping = {
        "icon_16x16.png": 16,
        "icon_16x16@2x.png": 32,
        "icon_32x32.png": 32,
        "icon_32x32@2x.png": 64,
        "icon_128x128.png": 128,
        "icon_128x128@2x.png": 256,
        "icon_256x256.png": 256,
        "icon_256x256@2x.png": 512,
        "icon_512x512.png": 512,
        "icon_512x512@2x.png": 1024,
    }
    for name, size in mapping.items():
        shutil.copyfile(os.path.join(generated_dir, f"MeshRepair_{size}.png"), os.path.join(iconset, name))

    write_icns(os.path.join(root, "packaging", "macos", "MeshRepair.icns"), [
        ("ic07", os.path.join(generated_dir, "MeshRepair_128.png")),
        ("ic08", os.path.join(generated_dir, "MeshRepair_256.png")),
        ("ic09", os.path.join(generated_dir, "MeshRepair_512.png")),
        ("ic10", os.path.join(generated_dir, "MeshRepair_1024.png")),
        ("ic13", os.path.join(generated_dir, "MeshRepair_256.png")),
        ("ic14", os.path.join(generated_dir, "MeshRepair_512.png")),
    ])


if __name__ == "__main__":
    main()
