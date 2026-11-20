#!/usr/bin/env python3
# 把 design/icon 的设计稿导出成两端要用的图标文件（需要 rsvg-convert、iconutil、Pillow）：
#   lemon-shot-macos/Packaging/AppIcon.icns
#   lemon-shot-windows/res/app.ico（16–48 用简化版，64 以上用完整版；256 用 PNG 压缩，其余 BMP，Win7 可用）
#   lemon-shot-core/resources/brand/app-icon.png（关于页，512）
#   lemon-shot-core/resources/brand/glyph-<n>.png（菜单栏/托盘单色剪影，黑色 + 透明度）
# 用法：python3 -I design/icon/export_icons.py
import io, os, struct, subprocess, sys, tempfile
from PIL import Image

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, '..', '..'))
TMP = tempfile.mkdtemp(prefix='lemonshot-icon-')
subprocess.run([sys.executable, '-I', os.path.join(HERE, 'gen_icon.py'), TMP], check=True)

def render(svg, size, out=None, view=None):
    out = out or os.path.join(TMP, f'{os.path.basename(svg)}-{size}.png')
    src = os.path.join(TMP, svg + '.svg')
    if view:  # 改 viewBox 后渲染（剪影按外接框裁切）
        text = open(src).read().replace('viewBox="0 0 1024 1024"', f'viewBox="{view}"', 1)
        src = os.path.join(TMP, f'{svg}-view.svg')
        open(src, 'w').write(text)
    subprocess.run(['rsvg-convert', '-w', str(size), '-h', str(size), src, '-o', out], check=True)
    return out

# ---------------------------------------------------------------- macOS .icns
iconset = os.path.join(TMP, 'AppIcon.iconset')
os.makedirs(iconset)
for base in (16, 32, 128, 256, 512):
    for scale in (1, 2):
        px = base * scale
        svg = 'icon-small' if px <= 32 else 'icon'
        name = f'icon_{base}x{base}' + ('@2x' if scale == 2 else '') + '.png'
        render(svg, px, os.path.join(iconset, name))
icns = os.path.join(ROOT, 'lemon-shot-macos', 'Packaging', 'AppIcon.icns')
subprocess.run(['iconutil', '-c', 'icns', iconset, '-o', icns], check=True)

# ---------------------------------------------------------------- Windows .ico
def bmp_entry(im):
    """ICO 里的 BMP 图：BITMAPINFOHEADER（高度×2）+ 自下而上 BGRA + 1 位 AND 掩码。"""
    w, h = im.size
    px = im.convert('RGBA').load()
    rows = []
    for y in range(h - 1, -1, -1):
        rows.append(b''.join(struct.pack('BBBB', px[x, y][2], px[x, y][1], px[x, y][0], px[x, y][3]) for x in range(w)))
    mask_stride = ((w + 31) // 32) * 4
    mask = b'\0' * (mask_stride * h)
    header = struct.pack('<IiiHHIIiiII', 40, w, h * 2, 1, 32, 0, len(b''.join(rows)) + len(mask), 0, 0, 0, 0)
    return header + b''.join(rows) + mask

entries = []
for size in (16, 20, 24, 32, 40, 48, 64, 256):
    svg = 'icon-win-small' if size <= 48 else 'icon-win'
    im = Image.open(render(svg, size)).convert('RGBA')
    if size == 256:
        buf = io.BytesIO(); im.save(buf, 'PNG', optimize=True); data = buf.getvalue()
    else:
        data = bmp_entry(im)
    entries.append((size, data))
ico = struct.pack('<HHH', 0, 1, len(entries))
offset = 6 + 16 * len(entries)
body = b''
for size, data in entries:
    ico += struct.pack('<BBBBHHII', size % 256, size % 256, 0, 0, 1, 32, len(data), offset + len(body))
    body += data
open(os.path.join(ROOT, 'lemon-shot-windows', 'res', 'app.ico'), 'wb').write(ico + body)

# ---------------------------------------------------------------- 关于页 + 单色剪影
brand = os.path.join(ROOT, 'lemon-shot-core', 'resources', 'brand')
os.makedirs(brand, exist_ok=True)
Image.open(render('icon', 512)).save(os.path.join(brand, 'app-icon.png'), optimize=True)
# 剪影：先高分辨率渲染求外接框，再按外接框（加 4% 边距、补成正方形）在各目标尺寸直接渲染，保证小图清晰
big = Image.open(render('glyph', 1024)).getchannel('A')
x0, y0, x1, y1 = big.getbbox()
side = max(x1 - x0, y1 - y0) * 1.04
cx, cy = (x0 + x1) / 2, (y0 + y1) / 2
view = f'{cx - side / 2:.1f} {cy - side / 2:.1f} {side:.1f} {side:.1f}'
for size in (16, 18, 20, 24, 32, 36, 40, 48):
    out = os.path.join(brand, f'glyph-{size}.png')
    render('glyph', size, out, view)
    Image.open(out).save(out, optimize=True)
print('icns', os.path.getsize(icns), 'ico', len(ico + body))
