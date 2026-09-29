"""Regenerates resources/app.ico from resources/icon.svg.

Large sizes (40 px and up): the SVG is rendered by headless Chrome or Edge at
1024 px and area-averaged down, keeping its gradients.

Small sizes (16-32 px, the tray and small Explorer views): gradients and
anti-aliased edges turn to mush when a 1024 px render is shrunk that far, so
these are drawn directly from the same design, simplified: flat colors, no
border or baseline, and bars snapped to whole pixels at each size.

Only needed after editing the design; the generated .ico is committed, so
building the app does not depend on this.

Usage:  python tools/make_icon.py        (needs: pip install pillow)
"""

import os
import subprocess
import sys
import tempfile
from pathlib import Path

from PIL import Image, ImageDraw

ROOT = Path(__file__).resolve().parent.parent
SVG = ROOT / "resources" / "icon.svg"
ICO = ROOT / "resources" / "app.ico"
RENDER_SIZE = 1024
SMALL_SIZES = [16, 20, 24, 32]
LARGE_SIZES = [40, 48, 64, 96, 128, 256]

# The design, in icon.svg's 256-unit coordinates. Keep in sync with the SVG.
TILE = (12, 244)                  # tile spans x/y 12..244
TILE_RADIUS = 52
BAR_X = (52, 108, 164)
BAR_WIDTH = 40
BAR_HEIGHTS = (128, 72, 96)       # FPS, CPU, GPU
# Flat versions of the SVG gradients (roughly their midpoints).
TILE_COLOR = (32, 36, 45, 255)
BAR_COLORS = ((144, 217, 94, 255), (103, 182, 242, 255), (248, 159, 78, 255))
SUPERSAMPLE = 8                   # for anti-aliased tile corners only


def draw_small(size: int) -> Image.Image:
    """Pixel-snapped, flat version of the icon. The tile fills the whole canvas."""
    k = size / (TILE[1] - TILE[0])  # design units -> pixels
    bar_w = max(2, round(BAR_WIDTH * k))
    gap = max(1, round((BAR_X[1] - BAR_X[0] - BAR_WIDTH) * k))
    left = (size - (3 * bar_w + 2 * gap) + 1) // 2
    heights = [max(2, round(h * k)) for h in BAR_HEIGHTS]
    top = (size - max(heights)) // 2   # no baseline here, so center the bars vertically
    bottom = top + max(heights)

    s = SUPERSAMPLE
    img = Image.new("RGBA", (size * s, size * s), (0, 0, 0, 0))
    draw = ImageDraw.Draw(img)
    draw.rounded_rectangle((0, 0, size * s - 1, size * s - 1), radius=round(TILE_RADIUS * k) * s, fill=TILE_COLOR)
    for i, (height, color) in enumerate(zip(heights, BAR_COLORS)):
        x = left + i * (bar_w + gap)
        draw.rectangle((x * s, (bottom - height) * s, (x + bar_w) * s - 1, bottom * s - 1), fill=color)
    # Bar edges sit on whole pixels, so averaging keeps them crisp; only the corners blend.
    return img.reduce(s)

BROWSERS = [
    Path(os.environ.get("ProgramFiles", r"C:\Program Files")) / "Google/Chrome/Application/chrome.exe",
    Path(os.environ.get("ProgramFiles(x86)", r"C:\Program Files (x86)")) / "Google/Chrome/Application/chrome.exe",
    Path(os.environ.get("ProgramFiles(x86)", r"C:\Program Files (x86)")) / "Microsoft/Edge/Application/msedge.exe",
    Path(os.environ.get("ProgramFiles", r"C:\Program Files")) / "Microsoft/Edge/Application/msedge.exe",
]


def find_browser() -> Path:
    for path in BROWSERS:
        if path.exists():
            return path
    sys.exit("Chrome or Edge is needed to render the SVG")


def render_svg(browser: Path, work: Path) -> Image.Image:
    html = work / "icon.html"
    html.write_text(
        '<html><body style="margin:0;background:transparent">'
        f'<img src="{SVG.as_uri()}" width="{RENDER_SIZE}" height="{RENDER_SIZE}" style="display:block">'
        "</body></html>",
        encoding="utf-8",
    )
    png = work / "icon.png"
    subprocess.run(
        [
            str(browser),
            "--headless=new",
            "--disable-gpu",
            "--hide-scrollbars",
            "--no-first-run",
            f"--user-data-dir={work / 'profile'}",  # never touches the user's running browser
            "--default-background-color=00000000",  # transparent corners
            "--force-device-scale-factor=1",
            f"--window-size={RENDER_SIZE},{RENDER_SIZE}",
            f"--screenshot={png}",
            html.as_uri(),
        ],
        check=True,
        capture_output=True,
        timeout=60,
    )
    image = Image.open(png).convert("RGBA")
    if image.size != (RENDER_SIZE, RENDER_SIZE):
        image = image.crop((0, 0, RENDER_SIZE, RENDER_SIZE))
    if image.getpixel((0, 0))[3] != 0:
        sys.exit("render has no transparency; is the browser too old?")
    return image


def main() -> None:
    with tempfile.TemporaryDirectory() as tmp:
        master = render_svg(find_browser(), Path(tmp))
    frames = [draw_small(s) for s in SMALL_SIZES]
    # BOX = plain area averaging: no ringing halos around the bars, unlike Lanczos.
    frames +=[master.resize((s, s), Image.Resampling.BOX) for s in LARGE_SIZES]
    sizes = SMALL_SIZES + LARGE_SIZES
    frames[-1].save(ICO, format="ICO", sizes=[(s, s) for s in sizes], append_images=frames[:-1])
    print(f"wrote {ICO.relative_to(ROOT)} ({', '.join(map(str, sizes))} px)")


if __name__ == "__main__":
    main()
