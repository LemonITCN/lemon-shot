import sys, os, re, json, xml.etree.ElementTree as ET
src, out, lic_out = sys.argv[1], sys.argv[2], sys.argv[3]
names = sys.argv[4].split(',')
def f(v): return float(v)
def num(x):
    s = ('%.3f' % x).rstrip('0').rstrip('.')
    return s if s not in ('-0', '') else '0'
def circle(cx, cy, rx, ry):
    return 'M%s %sA%s %s 0 1 0 %s %sA%s %s 0 1 0 %s %sZ' % (num(cx-rx), num(cy), num(rx), num(ry), num(cx+rx), num(cy), num(rx), num(ry), num(cx-rx), num(cy))
def rect(x, y, w, h, rx, ry):
    if rx <= 0 and ry <= 0:
        return 'M%s %sH%sV%sH%sZ' % (num(x), num(y), num(x+w), num(y+h), num(x))
    if rx <= 0: rx = ry
    if ry <= 0: ry = rx
    rx = min(rx, w/2); ry = min(ry, h/2)
    return ('M%s %sH%sA%s %s 0 0 1 %s %sV%sA%s %s 0 0 1 %s %sH%sA%s %s 0 0 1 %s %sV%sA%s %s 0 0 1 %s %sZ' % (
        num(x+rx), num(y), num(x+w-rx), num(rx), num(ry), num(x+w), num(y+ry), num(y+h-ry), num(rx), num(ry), num(x+w-rx), num(y+h),
        num(x+rx), num(rx), num(ry), num(x), num(y+h-ry), num(y+ry), num(rx), num(ry), num(x+rx), num(y)))
def absolute_start(d):
    # 多个元素拼成一条路径时，元素开头的相对 m 会变成相对上一段终点。
    # 路径开头的 m 本来就相对原点，等价于 M；其后的隐含坐标对是相对 lineto，补一个 l。
    toks = re.findall(r'[A-Za-z]|[-+]?(?:\d*\.\d+|\d+\.?)(?:[eE][-+]?\d+)?', d)
    if not toks or toks[0] != 'm':
        return d
    out = ['M', toks[1], toks[2]]
    rest = toks[3:]
    if rest and not re.match(r'[A-Za-z]', rest[0]):
        out.append('l')
    out += rest
    s = ''
    for t in out:
        if s and not (re.match(r'[A-Za-z]', t) or re.match(r'[A-Za-z]', s[-1])):
            s += ' '
        s += t
    return s
res = {}
missing = []
for n in names:
    p = os.path.join(src, n + '.svg')
    if not os.path.exists(p):
        missing.append(n); continue
    root = ET.parse(p).getroot()
    parts = []; fills = []
    for el in root.iter():
        tag = el.tag.split('}')[-1]
        a = el.attrib
        d = None
        if tag == 'path': d = absolute_start(a['d'])
        elif tag == 'circle': d = circle(f(a['cx']), f(a['cy']), f(a['r']), f(a['r']))
        elif tag == 'ellipse': d = circle(f(a['cx']), f(a['cy']), f(a['rx']), f(a['ry']))
        elif tag == 'rect': d = rect(f(a.get('x', 0)), f(a.get('y', 0)), f(a['width']), f(a['height']), f(a.get('rx', 0)), f(a.get('ry', 0)))
        elif tag == 'line': d = 'M%s %sL%s %s' % (a['x1'], a['y1'], a['x2'], a['y2'])
        elif tag in ('polyline', 'polygon'):
            pts = re.split(r'[\s,]+', a['points'].strip())
            d = 'M' + ' '.join(pts[0:2]) + ''.join('L%s %s' % (pts[i], pts[i+1]) for i in range(2, len(pts), 2)) + ('Z' if tag == 'polygon' else '')
        if d is not None:
            if a.get('fill') not in (None, 'none'): fills.append(d)
            else: parts.append(d)
    item = {'d': ' '.join(parts)}
    if fills: item['fill'] = ' '.join(fills)
    res[n] = item
json.dump({'source': 'lucide-static 1.76.0 (ISC)', 'viewBox': 24, 'strokeWidth': 2, 'icons': res}, open(out, 'w'), ensure_ascii=False, indent=1, sort_keys=True)
open(lic_out, 'w').write(open(os.path.join(os.path.dirname(src), 'LICENSE')).read())
print('icons', len(res), 'missing', missing)
