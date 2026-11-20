#!/bin/bash
# 把两端自测截图并排放进一个网页，并标出像素差异比例。结果目录自包含（图片复制进去、相对路径），可整体打包下载。
# 用法：scripts/compare-selftest.sh <mac 输出目录> <windows 输出目录> <结果目录>
set -euo pipefail
MAC="$1"; WIN="$2"; OUT="$3"
mkdir -p "$OUT/mac" "$OUT/win" "$OUT/diff"
python3 -P - "$MAC" "$WIN" "$OUT" <<'PY'
import os, sys, html, shutil
mac, win, out = sys.argv[1:4]
try:
    from PIL import Image, ImageChops
except Exception:
    Image = None
names = sorted(set(f for d in (mac, win) if os.path.isdir(d) for f in os.listdir(d) if f.endswith('.png')))
rows = []
for n in names:
    a, b = os.path.join(mac, n), os.path.join(win, n)
    if os.path.exists(a): shutil.copy(a, os.path.join(out, 'mac', n))
    if os.path.exists(b): shutil.copy(b, os.path.join(out, 'win', n))
    diff = ''
    if Image and os.path.exists(a) and os.path.exists(b):
        ia, ib = Image.open(a).convert('RGBA'), Image.open(b).convert('RGBA')
        if ia.size != ib.size:
            diff = '尺寸不同 %dx%d / %dx%d' % (ia.size + ib.size)
        else:
            d = ImageChops.difference(ia, ib).convert('L').point(lambda v: 255 if v > 24 else 0)
            changed = sum(d.histogram()[255:])
            diff = '差异像素 %.2f%%' % (100.0 * changed / (ia.size[0] * ia.size[1]))
            d.save(os.path.join(out, 'diff', n))
    cell = lambda rel: '<img src="%s">' % html.escape(rel) if os.path.exists(os.path.join(out, rel)) else '<span class="miss">缺少</span>'
    rows.append('<tr><th>%s<br><small>%s</small></th><td>%s</td><td>%s</td><td>%s</td></tr>' % (
        html.escape(n[:-4]), html.escape(diff), cell('mac/' + n), cell('win/' + n),
        cell('diff/' + n) if os.path.exists(os.path.join(out, 'diff', n)) else ''))
page = '''<!doctype html><meta charset="utf-8"><title>自测截图对照</title>
<style>body{font:13px -apple-system,"Microsoft YaHei",sans-serif;margin:16px;background:#f5f5f7}
table{border-collapse:collapse}th,td{border:1px solid #ddd;padding:6px;vertical-align:top;background:#fff}
th{text-align:left;white-space:nowrap}img{max-width:560px;display:block}.miss{color:#e5484d}</style>
<table><tr><th>状态</th><th>macOS</th><th>Windows</th><th>差异</th></tr>%s</table>''' % '\n'.join(rows)
open(os.path.join(out, 'index.html'), 'w', encoding='utf-8').write(page)
print(os.path.join(out, 'index.html'))
PY
