import io, struct
from PIL import Image, ImageDraw
import numpy as np

SRC = 'logo_source/'
OUT = 'assets/'
INK = (0x18, 0x18, 0x18)       # the logo's own near-black
PAPER = (0xF5, 0xF5, 0xF7)     # near-white used for ink on dark backgrounds

def lum(im):
    a = np.asarray(im.convert('RGB'), dtype=np.float32)
    return (0.299*a[...,0] + 0.587*a[...,1] + 0.114*a[...,2])

def rgba(alpha, color):
    h, w = alpha.shape
    out = np.zeros((h, w, 4), dtype=np.uint8)
    out[...,0], out[...,1], out[...,2] = color
    out[...,3] = np.clip(alpha*255 + 0.5, 0, 255).astype(np.uint8)
    return Image.fromarray(out, 'RGBA')

def crop_pad(im, pad_frac=0.0, thresh=6):
    a = np.asarray(im)[...,3]
    ys, xs = np.where(a > thresh)
    x0, x1, y0, y1 = xs.min(), xs.max()+1, ys.min(), ys.max()+1
    im = im.crop((x0, y0, x1, y1))
    if pad_frac:
        p = int(max(im.size) * pad_frac)
        canvas = Image.new('RGBA', (im.width + 2*p, im.height + 2*p), (0,0,0,0)); canvas.paste(im, (p, p)); im = canvas
    return im

# --- marks (2.png: ink on white = for light UI ; 3.png: white on ink = for dark UI) ---
l2 = lum(Image.open(SRC+'2.png')); a2 = np.clip((255.0 - l2) / (255.0 - 24.0), 0, 1)
l3 = lum(Image.open(SRC+'3.png')); a3 = np.clip((l3 - 24.0) / (255.0 - 24.0), 0, 1)
mark_for_light = crop_pad(rgba(a2, INK))
mark_for_dark  = crop_pad(rgba(a3, PAPER))

# --- lockup (1.png has only a dark version; the ink is monochrome, so recolour for dark UI) ---
l1 = lum(Image.open(SRC+'1.png')); a1 = np.clip((255.0 - l1) / (255.0 - 24.0), 0, 1)       # normalise so the #181818 mark and the black wordmark both reach full opacity
lock_for_light = crop_pad(rgba(a1, INK))
lock_for_dark  = crop_pad(rgba(a1, PAPER))

def save(im, name, maxw=None, maxh=None):
    if maxh and im.height > maxh: im = im.resize((round(im.width*maxh/im.height), maxh), Image.LANCZOS)
    if maxw and im.width  > maxw: im = im.resize((maxw, round(im.height*maxw/im.width)), Image.LANCZOS)
    im.save(OUT+name, optimize=True); print(name, im.size)
save(mark_for_light, 'mark_for_light.png', maxh=640)
save(mark_for_dark,  'mark_for_dark.png',  maxh=640)
save(lock_for_light, 'lockup_for_light.png', maxh=480)
save(lock_for_dark,  'lockup_for_dark.png',  maxh=480)

# --- app icon: rounded dark tile + white mark, drawn fresh at each size so small sizes stay crisp ---
def tile(size):
    S = size * 8                                         # supersample
    img = Image.new('RGBA', (S, S), (0,0,0,0)); d = ImageDraw.Draw(img)
    d.rounded_rectangle((0, 0, S-1, S-1), radius=int(S*0.225), fill=INK+(255,), outline=(255,255,255,34), width=max(2, S//96))   # faint edge so the tile reads on dark taskbars
    frac = 0.66 if size >= 48 else (0.72 if size >= 24 else 0.78)      # mark fills more of the tile at tiny sizes
    m = mark_for_dark; k = S*frac / m.height
    mm = m.resize((max(1, round(m.width*k)), max(1, round(m.height*k))), Image.LANCZOS)
    img.alpha_composite(mm, ((S-mm.width)//2, (S-mm.height)//2))
    return img.resize((size, size), Image.LANCZOS)
sizes = [16, 20, 24, 32, 40, 48, 64, 96, 128, 256]
pngs = []
for s in sizes:
    b = io.BytesIO(); tile(s).save(b, 'PNG', optimize=True); pngs.append(b.getvalue())
hdr = struct.pack('<HHH', 0, 1, len(sizes)); off = 6 + 16*len(sizes); ent = b''; data = b''
for s, p in zip(sizes, pngs):
    ent += struct.pack('<BBBBHHII', s % 256, s % 256, 0, 0, 1, 32, len(p), off + len(data)); data += p
open('seek.ico', 'wb').write(hdr + ent + data); print('seek.ico', len(hdr+ent+data), 'bytes')
tile(256).save('assets/icon_preview.png')
