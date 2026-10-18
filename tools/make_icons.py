"""Generates the panel's top-bar icons: 24x24 white glyphs on transparent, minimal line style (like an
Android status bar). The panel only uses the alpha channel and tints it with the theme colour, so any
PNG with the same names can replace these: put the files in /home/root/x45new/icons/ on the unit.

    python tools/make_icons.py          ->  deploy/icons/*.png  (needs the Pillow module: pip install pillow)
"""
import math
import os
import shutil
from PIL import Image, ImageDraw

# The glyphs are drawn once in a 24-unit grid; setup() sets the scale. The small file (24 px) is the one the
# lists and the top bar use as it is; the large one (128 px, NAME@128.png) is for the pictures of the input menu,
# so that nothing there is ever a 24 px glyph blown up.
UNITS = 24
S = 12           # drawing pixels per grid unit (supersampling)
N = 24           # final size
W = UNITS * S
LW = int(2.0 * S)
SIZES = ((24, '', 12), (128, '@128', 22))   # final px, file suffix, supersampling


def setup(n, s):
    global S, N, W, LW
    S, N = s, n
    W = UNITS * S
    LW = int(2.0 * S)
OUT = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'deploy', 'icons')
STAGE = os.path.join(os.path.dirname(os.path.abspath(__file__)), 'pkgstage', 'x45new', 'icons')
WHITE = (255, 255, 255, 255)


def canvas():
    im = Image.new('RGBA', (W, W), (255, 255, 255, 0))
    return im, ImageDraw.Draw(im)


def u(v):
    return int(round(v * S))


def circle(d, cx, cy, r, fill=False, width=None):
    width = width or LW
    box = [u(cx - r), u(cy - r), u(cx + r), u(cy + r)]
    if fill:
        d.ellipse(box, fill=WHITE)
    else:
        d.ellipse(box, outline=WHITE, width=width)


def line(d, pts, width=None):
    width = width or LW
    pts = [(u(x), u(y)) for x, y in pts]
    d.line(pts, fill=WHITE, width=width, joint='curve')
    for x, y in (pts[0], pts[-1]):
        d.ellipse([x - width // 2, y - width // 2, x + width // 2, y + width // 2], fill=WHITE)


def arc(d, cx, cy, r, a0, a1, width=None):
    width = width or LW
    d.arc([u(cx - r), u(cy - r), u(cx + r), u(cy + r)], a0, a1, fill=WHITE, width=width)


def rrect(d, x0, y0, x1, y1, r, fill=False, width=None):
    width = width or LW
    box = [u(x0), u(y0), u(x1), u(y1)]
    if fill:
        d.rounded_rectangle(box, radius=u(r), fill=WHITE)
    else:
        d.rounded_rectangle(box, radius=u(r), outline=WHITE, width=width)


def save(name, im, suffix=''):
    im = im.resize((N, N), Image.LANCZOS)
    os.makedirs(OUT, exist_ok=True)
    im.save(os.path.join(OUT, name + suffix + '.png'))


icons = {}


def icon(name):
    def deco(fn):
        icons[name] = fn
        return fn
    return deco


@icon('network')       # globe: internet radio, the library and the media servers (WiFi has its own symbol)
def _(d):
    circle(d, 12, 12, 9.3)
    d.ellipse([u(12 - 4.2), u(12 - 9.3), u(12 + 4.2), u(12 + 9.3)], outline=WHITE, width=int(1.6 * S))
    line(d, [(2.7, 12), (21.3, 12)], width=int(1.6 * S))
    line(d, [(4.4, 7.4), (19.6, 7.4)], width=int(1.6 * S))
    line(d, [(4.4, 16.6), (19.6, 16.6)], width=int(1.6 * S))


@icon('fm')            # radio
def _(d):
    rrect(d, 2.5, 8, 21.5, 21, 2.5)
    line(d, [(5, 8), (17, 2.5)])
    circle(d, 8.5, 14.5, 3.0)
    line(d, [(14, 12), (19, 12)], width=int(1.6 * S))
    line(d, [(14, 15), (19, 15)], width=int(1.6 * S))
    line(d, [(14, 18), (19, 18)], width=int(1.6 * S))


@icon('dab')           # broadcast
def _(d):
    circle(d, 12, 12, 2.0, fill=True)
    for r in (5.2, 9.0):
        arc(d, 12, 12, r, -50, 50)
        arc(d, 12, 12, r, 130, 230)


@icon('pcusb')         # monitor
def _(d):
    rrect(d, 2.5, 3.5, 21.5, 16.5, 2)
    line(d, [(12, 16.5), (12, 20.5)])
    line(d, [(7, 20.5), (17, 20.5)])


@icon('aes-ebu')       # XLR
def _(d):
    circle(d, 12, 12, 9.3)
    circle(d, 8.5, 9.5, 1.5, fill=True)
    circle(d, 15.5, 9.5, 1.5, fill=True)
    circle(d, 12, 15.5, 1.5, fill=True)


@icon('coaxial')       # RCA plug
def _(d):
    circle(d, 12, 12, 9.3)
    circle(d, 12, 12, 4.8)
    circle(d, 12, 12, 1.6, fill=True)


@icon('optical')       # toslink
def _(d):
    rrect(d, 3.5, 6.5, 20.5, 17.5, 2)
    rrect(d, 8.5, 10.0, 15.5, 14.0, 1, fill=True)
    line(d, [(12, 2), (12, 5)], width=int(1.6 * S))
    line(d, [(7.5, 3), (8.8, 5.2)], width=int(1.6 * S))
    line(d, [(16.5, 3), (15.2, 5.2)], width=int(1.6 * S))


@icon('linein')        # two RCA jacks
def _(d):
    circle(d, 7, 12, 4.6)
    circle(d, 7, 12, 1.2, fill=True)
    circle(d, 17, 12, 4.6)
    circle(d, 17, 12, 1.2, fill=True)


@icon('auxin')         # 3.5 mm plug
def _(d):
    rrect(d, 1.5, 9.5, 8, 14.5, 1.2, fill=True)
    line(d, [(8, 12), (11.5, 12)])
    rrect(d, 11.5, 10.2, 19.5, 13.8, 1.0)
    line(d, [(14.5, 10.2), (14.5, 13.8)], width=int(1.4 * S))
    line(d, [(17.2, 10.2), (17.2, 13.8)], width=int(1.4 * S))
    line(d, [(19.5, 12), (22.5, 12)])


@icon('phono')         # record
def _(d):
    circle(d, 12, 12, 9.3)
    circle(d, 12, 12, 3.8)
    circle(d, 12, 12, 1.0, fill=True)
    arc(d, 12, 12, 6.8, 200, 260, width=int(1.4 * S))


@icon('cd')            # disc
def _(d):
    circle(d, 12, 12, 9.3)
    circle(d, 12, 12, 2.4)
    arc(d, 12, 12, 6.0, 215, 290, width=int(1.4 * S))


@icon('bluetooth')
def _(d):
    line(d, [(12, 2.5), (12, 21.5), (18.5, 15.5), (5.5, 8.5)])
    line(d, [(12, 2.5), (18.5, 8.5), (5.5, 15.5)])


@icon('usb')           # usb trident
def _(d):
    line(d, [(12, 4), (12, 21)])
    circle(d, 12, 21, 1.8, fill=True)
    line(d, [(12, 14.5), (7, 11.5), (7, 8.5)])
    circle(d, 7, 7.2, 1.7)
    line(d, [(12, 11), (17, 8.5), (17, 6.8)])
    rrect(d, 15.4, 4, 18.6, 7, 0.3, fill=True)
    d.polygon([(u(12), u(1.5)), (u(9.6), u(5.5)), (u(14.4), u(5.5))], fill=WHITE)


@icon('mute')          # speaker with a slash
def _(d):
    d.polygon([(u(3), u(9)), (u(7.5), u(9)), (u(13), u(4.5)), (u(13), u(19.5)), (u(7.5), u(15)), (u(3), u(15))], fill=WHITE)
    line(d, [(4, 3.5), (20.5, 20.5)], width=int(2.2 * S))
    # keep a gap around the slash so it reads on the speaker
    gap = Image.new('L', (W, W), 0)
    ImageDraw.Draw(gap).line([(u(5.2), u(3.5)), (u(21.7), u(20.5))], fill=255, width=int(5 * S))
    return gap


@icon('warn')          # triangle with !
def _(d):
    d.polygon([(u(12), u(2.5)), (u(22.5), u(21))], fill=None)
    d.line([(u(12), u(3)), (u(22), u(20.5)), (u(2), u(20.5)), (u(12), u(3))], fill=WHITE, width=LW, joint='curve')
    line(d, [(12, 9), (12, 14.5)])
    circle(d, 12, 17.6, 1.1, fill=True)


@icon('rec')
def _(d):
    circle(d, 12, 12, 7, fill=True)


@icon('wake')          # for the screen-off option label (sun)
def _(d):
    circle(d, 12, 12, 4.2)
    for i in range(8):
        a = i * math.pi / 4
        line(d, [(12 + 7.2 * math.cos(a), 12 + 7.2 * math.sin(a)), (12 + 10 * math.cos(a), 12 + 10 * math.sin(a))],
             width=int(1.6 * S))


def render(fn):
    im, d = canvas()
    mask = fn(d)
    if mask is not None:       # the mute icon carves a gap
        from PIL import ImageChops
        carve = ImageChops.invert(mask)
        # redraw: keep the speaker where the gap is not, and add the slash itself on top
        sp = Image.new('RGBA', (W, W), (255, 255, 255, 0))
        sd = ImageDraw.Draw(sp)
        sd.polygon([(u(3), u(9)), (u(7.5), u(9)), (u(13), u(4.5)), (u(13), u(19.5)), (u(7.5), u(15)), (u(3), u(15))], fill=WHITE)
        a2 = ImageChops.multiply(sp.split()[3], carve)
        im = Image.new('RGBA', (W, W), (255, 255, 255, 0))
        im.putalpha(a2)
        d = ImageDraw.Draw(im)
        line(d, [(4, 3.5), (20.5, 20.5)], width=int(2.2 * S))
    return im


def main():
    sheet = Image.new('RGBA', (len(icons) * 36 + 12, 48), (22, 26, 36, 255))
    for i, (name, fn) in enumerate(icons.items()):
        for n, suffix, s in SIZES:
            setup(n, s)
            im = render(fn)
            save(name, im, suffix)
            if not suffix:
                sheet.alpha_composite(im.resize((N, N), Image.LANCZOS), (12 + i * 36, 12))
    sheet = sheet.resize((sheet.width * 3, sheet.height * 3), Image.NEAREST)
    here = os.path.dirname(os.path.abspath(__file__))
    if os.path.isdir(os.path.join(here, 'out')):      # an overview picture, only where a folder for it exists
        sheet.convert('RGB').save(os.path.join(here, 'out', 'icons_sheet.png'))
    if os.path.isdir(os.path.dirname(STAGE)):         # copy into a staging tree, only where there is one
        os.makedirs(STAGE, exist_ok=True)
        for f in os.listdir(OUT):
            shutil.copy(os.path.join(OUT, f), os.path.join(STAGE, f))
    print(len(icons), 'icons, in', len(SIZES), 'sizes')


main()
