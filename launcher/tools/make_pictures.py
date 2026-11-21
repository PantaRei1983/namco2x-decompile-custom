r"""make_pictures.py -- the launcher's pictures from screen grabs.

  python launcher/tools/make_pictures.py picture <grab.png> <out.bmp> [x0 y0 x1 y1]
      a game's action shot: cropped (optional, a 16:9 box), scaled to 960 wide, 24-bit BMP (games\<Game>\picture.bmp)
  python launcher/tools/make_pictures.py logo <grab.png> <out.bmp> x0 y0 x1 y1 [key]
      a game's title logo: cropped to the box, its background made transparent, 32-bit BMP with alpha
      (games\<Game>\logo.bmp). key = how the background is found:
        black   (default) pixels near black fade out (a logo over a dark screen)
        flood   the colour at the box's corners, flood-filled from the edges with a tolerance (a plain backdrop)
        none    keep the box as it is, square (a busy backdrop that cannot be cut out)

SDL_LoadBMP reads the alpha of a BITMAPV4/V5 BMP with BI_BITFIELDS masks; Pillow does not write one, so it is written here.
Needs Pillow (pip install pillow).
"""
import struct
import sys
from collections import deque

from PIL import Image, ImageFilter


def write_bmp32(img, path):
    img = img.convert('RGBA')
    w, h = img.size
    px = img.tobytes('raw', 'BGRA', 0, -1)              # bottom-up rows, B G R A
    hdr = struct.pack('<IiiHHIIiiII', 108, w, h, 1, 32, 3, len(px), 2835, 2835, 0, 0)
    hdr += struct.pack('<IIII', 0x00FF0000, 0x0000FF00, 0x000000FF, 0xFF000000)   # R G B A masks
    hdr += b'sRGB'[::-1] + b'\0' * 36 + b'\0' * 12      # colour space 'sRGB', endpoints, gamma
    off = 14 + len(hdr)
    with open(path, 'wb') as f:
        f.write(b'BM' + struct.pack('<IHHI', off + len(px), 0, 0, off))
        f.write(hdr)
        f.write(px)


def key_black(img, lo=18, hi=60):
    """alpha from brightness: below lo transparent, above hi opaque, a ramp between (soft edges, no black fringe)"""
    rgba = img.convert('RGBA')
    px = rgba.load()
    for y in range(rgba.height):
        for x in range(rgba.width):
            r, g, b, _ = px[x, y]
            m = max(r, g, b)
            a = 0 if m <= lo else 255 if m >= hi else int((m - lo) * 255 / (hi - lo))
            px[x, y] = (r, g, b, a)
    return rgba


def key_flood(img, tol=48):
    """the backdrop = what joins the box's edges in about the corners' colour"""
    rgb = img.convert('RGB')
    w, h = rgb.size
    px = rgb.load()
    corners = [px[0, 0], px[w - 1, 0], px[0, h - 1], px[w - 1, h - 1]]
    bg = tuple(sorted(c[i] for c in corners)[1] for i in range(3))
    near = lambda c: sum(abs(c[i] - bg[i]) for i in range(3)) <= tol
    mask = Image.new('L', (w, h), 255)
    m = mask.load()
    q = deque((x, y) for x in range(w) for y in (0, h - 1))
    q.extend((x, y) for y in range(h) for x in (0, w - 1))
    while q:
        x, y = q.popleft()
        if m[x, y] == 0 or not near(px[x, y]):
            continue
        m[x, y] = 0
        for dx, dy in ((1, 0), (-1, 0), (0, 1), (0, -1)):
            nx, ny = x + dx, y + dy
            if 0 <= nx < w and 0 <= ny < h and m[nx, ny]:
                q.append((nx, ny))
    mask = mask.filter(ImageFilter.GaussianBlur(0.8))
    out = rgb.convert('RGBA')
    out.putalpha(mask)
    return out


def trim(rgba, pad=4):
    bb = rgba.getchannel('A').point(lambda a: 255 if a > 24 else 0).getbbox()
    if not bb:
        return rgba
    x0, y0, x1, y1 = bb
    return rgba.crop((max(0, x0 - pad), max(0, y0 - pad), min(rgba.width, x1 + pad), min(rgba.height, y1 + pad)))


def main(a):
    kind, src, out = a[0], a[1], a[2]
    img = Image.open(src).convert('RGB')
    box = tuple(int(v) for v in a[3:7]) if len(a) >= 7 else None
    if box:
        img = img.crop(box)
    if kind == 'picture':
        img.resize((960, round(img.height * 960 / img.width)), Image.LANCZOS).save(out, 'BMP')
    elif kind == 'logo':
        key = a[7] if len(a) > 7 else 'black'
        rgba = key_black(img) if key == 'black' else key_flood(img) if key == 'flood' else img.convert('RGBA')
        if key != 'none':
            rgba = trim(rgba)
        if rgba.width > 800:                              # plenty for the launcher, small on disk
            rgba = rgba.resize((800, round(rgba.height * 800 / rgba.width)), Image.LANCZOS)
        write_bmp32(rgba, out)
    else:
        sys.exit(__doc__)


if __name__ == '__main__':
    if len(sys.argv) < 4:
        sys.exit(__doc__)
    main(sys.argv[1:])
