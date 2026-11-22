#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Turn render.cpp's raw frames into the README's GIFs and PNGs (needs Pillow).

    tools/render/make_media.py <frames-dir> <out-dir>

Frames are 240x320 palette indices, so every GIF uses the firmware's exact
palette; images are scaled 2x with nearest-neighbour to keep the pixels crisp.
"""
import sys
from pathlib import Path

from PIL import Image

W, H, SCALE = 240, 320, 2


def load(frames: Path, name: str, palette: list[int]) -> list[Image.Image]:
    raw = (frames / f"{name}.raw").read_bytes()
    out = []
    for i in range(len(raw) // (W * H)):
        im = Image.frombytes("P", (W, H), raw[i * W * H:(i + 1) * W * H])
        im.putpalette(palette)
        out.append(im.resize((W * SCALE, H * SCALE), Image.NEAREST))
    return out


def gif(frames: list[Image.Image], path: Path, ms: int = 66) -> None:
    frames[0].save(path, save_all=True, append_images=frames[1:], duration=ms, loop=0, optimize=False, disposal=1)
    print(f"{path}  {len(frames)} frames  {path.stat().st_size // 1024} KB")


def main(frames_dir: str, out_dir: str) -> None:
    frames, out = Path(frames_dir), Path(out_dir)
    out.mkdir(parents=True, exist_ok=True)
    pal = list((frames / "palette.bin").read_bytes())
    pal += [0] * (768 - len(pal))
    for name in ("hero", "handoff", "whisper", "tickle", "pet"):
        gif(load(frames, name, pal), out / f"{name}.gif")
    stills = load(frames, "states", pal)
    gap = 16
    strip = Image.new("RGB", (len(stills) * W * SCALE + (len(stills) - 1) * gap, H * SCALE), (13, 17, 23))
    for i, im in enumerate(stills):
        strip.paste(im.convert("RGB"), (i * (W * SCALE + gap), 0))
    strip.save(out / "states.png", optimize=True)
    print(f"{out / 'states.png'}  {(out / 'states.png').stat().st_size // 1024} KB")
    stills[2].convert("RGB").save(out / "working.png", optimize=True)


if __name__ == "__main__":
    main(*sys.argv[1:3])
