#!/usr/bin/env python3
# 生成 Lemon Shot 应用图标 SVG：可爱的黄色小剪刀，转轴处是作者的柠檬 logo。
# 用法：python3 -I gen_icon.py <输出目录>   之后用 rsvg-convert 渲染各尺寸
import math, sys, os

Y = '#FFE73C'      # logo 黄
G = '#5AD94C'      # logo 绿
OUT = sys.argv[1] if len(sys.argv) > 1 else '.'

def pt(cx, cy, r, deg):
    a = math.radians(deg)
    return (cx + r * math.cos(a), cy + r * math.sin(a))

def wedge(cx, cy, center_deg, spoke_half=19, corner=14, r_out=376):
    """一片果肉：两条直边平行于相邻分隔线（各让出 spoke_half），外沿为圆弧，圆角 corner。
    做法：先算向内再缩 corner 的尖角形状，再用同色 2*corner 粗、圆角接头的描边撑回原大小。"""
    off = spoke_half + corner
    ro = r_out - corner
    a1, a2 = center_deg - 30, center_deg + 30
    # 两条直边：与分隔线（a1、a2 方向）平行、向扇形内部偏移 off
    def edge_point(a, sign, r):
        # 分隔线方向单位向量 d，法向 n 指向扇形内部
        d = (math.cos(math.radians(a)), math.sin(math.radians(a)))
        n = (-d[1] * sign, d[0] * sign)
        return (cx + d[0] * r + n[0] * off, cy + d[1] * r + n[1] * off)
    # 尖端：两条偏移线的交点（在中心线方向上，距中心 off / sin(30°)）
    tip = pt(cx, cy, off / math.sin(math.radians(30)), center_deg)
    # 外弧端点：偏移线与半径 ro 圆的交点
    def arc_end(a, sign):
        d = (math.cos(math.radians(a)), math.sin(math.radians(a)))
        n = (-d[1] * sign, d[0] * sign)
        t = math.sqrt(max(ro * ro - off * off, 0))
        return (cx + d[0] * t + n[0] * off, cy + d[1] * t + n[1] * off)
    p1 = arc_end(a1, 1)
    p2 = arc_end(a2, -1)
    return 'M%.2f %.2f L%.2f %.2f A%.2f %.2f 0 0 1 %.2f %.2f Z' % (tip[0], tip[1], p1[0], p1[1], ro, ro, p2[0], p2[1]), corner * 2

def logo(cx=512, cy=512):
    """重建的柠檬 logo（1024 坐标，中心 512,512）。"""
    parts = []
    # 外圈：右下 51°~68° 断开，圆头
    r = 451.5
    a_start, a_end = 72.5, 46.5 + 360
    s = pt(cx, cy, r, a_start); e = pt(cx, cy, r, a_end)
    parts.append('<path d="M%.2f %.2f A%.2f %.2f 0 1 1 %.2f %.2f" fill="none" stroke="%s" stroke-width="75" stroke-linecap="round"/>' % (s[0], s[1], r, r, e[0], e[1], Y))
    for c in (0, 60, 120, 180, 240, 300):
        d, sw = wedge(cx, cy, c)
        color = G if c == 60 else Y
        parts.append('<path d="%s" fill="%s" stroke="%s" stroke-width="%d" stroke-linejoin="round"/>' % (d, color, color, sw))
    # 光标尾巴：沿 60° 从果肉伸出外圈断口
    a = math.radians(60); d = (math.cos(a), math.sin(a)); n = (-d[1], d[0])
    w = 50 / 2 - 8
    r0, r1 = 330, 482
    quad = [(cx + d[0] * r0 + n[0] * w, cy + d[1] * r0 + n[1] * w), (cx + d[0] * r1 + n[0] * w, cy + d[1] * r1 + n[1] * w),
            (cx + d[0] * r1 - n[0] * w, cy + d[1] * r1 - n[1] * w), (cx + d[0] * r0 - n[0] * w, cy + d[1] * r0 - n[1] * w)]
    parts.append('<path d="M%s Z" fill="%s" stroke="%s" stroke-width="16" stroke-linejoin="round"/>' % (' L'.join('%.2f %.2f' % q for q in quad), G, G))
    return '\n'.join(parts)

def squircle(x, y, size):
    """macOS 风格的圆角矩形（连续曲率近似：圆角半径 22.37%，控制点系数 0.55）。"""
    r = size * 0.2237; k = r * 0.552
    x2, y2 = x + size, y + size
    return ('M%.1f %.1f H%.1f C%.1f %.1f %.1f %.1f %.1f %.1f V%.1f C%.1f %.1f %.1f %.1f %.1f %.1f H%.1f '
            'C%.1f %.1f %.1f %.1f %.1f %.1f V%.1f C%.1f %.1f %.1f %.1f %.1f %.1f Z') % (
        x + r, y, x2 - r, x2 - r + k, y, x2, y + r - k, x2, y + r, y2 - r, x2, y2 - r + k, x2 - r + k, y2, x2 - r, y2,
        x + r, x + r - k, y2, x, y2 - r + k, x, y2 - r, y + r, x, y + r - k, x + r - k, y, x + r, y)

Y_DARK = '#E9B40C'   # 黄色描边（卡通描边用同色系深色）
G_DARK = '#38B42E'   # 绿色描边

def d_shape(xf, y0, y1, w, k, side):
    """D 形：直边在 x=xf（朝剪刀中轴），外侧半椭圆向 side 方向鼓出（-1 向左，1 向右）。k 为向内收缩量。"""
    xf2 = xf + side * k
    ym = (y0 + y1) / 2
    rx = max(w - 2 * k, 1)
    ry = max((y1 - y0) / 2 - k, 1)
    sweep = 0 if side < 0 else 1
    xe = xf2 + side * rx
    return (f'M{xf2:.1f} {ym - ry:.1f} A{rx:.1f} {ry:.1f} 0 0 {sweep} {xe:.1f} {ym:.1f} '
            f'A{rx:.1f} {ry:.1f} 0 0 {sweep} {xf2:.1f} {ym + ry:.1f} Z')

def handle(side, uid):
    """D 形指环把手（side=-1 左、1 右）：外侧半圆弧、内侧直边，中间 D 形孔，粗柄连到转轴。
    圆角做法：形状先多收缩 c，再用同色 2c 宽、圆角接头的描边撑回来。卡通描边：先画外扩 O 的深色层。"""
    xf, y0, y1, w = 24 * side, 104, 292, 124  # 直边位置、上下端、鼓出宽度
    T, C, CH, O = 34, 24, 18, 7               # 环厚、外圆角、孔圆角、描边粗细的一半
    outer = d_shape(xf, y0, y1, w, C, side)
    hole_band = d_shape(xf, y0, y1, w, T + CH, side)        # 孔（含描边带）
    hole_cut = d_shape(xf, y0, y1, w, T + O + CH, side)     # 真正挖空的部分
    sx, sy = 34 * side, 112
    hl = d_shape(xf, y0, y1, w, T / 2 + 2, side)            # 高光沿环中线走一小段
    return f'''
      <mask id="hole{uid}" maskUnits="userSpaceOnUse" x="-400" y="-400" width="800" height="800">
        <rect x="-400" y="-400" width="800" height="800" fill="#FFFFFF"/>
        <path d="{hole_cut}" fill="#000000" stroke="#000000" stroke-width="{2 * CH}" stroke-linejoin="round"/>
      </mask>
      <g mask="url(#hole{uid})">
        <path d="M0 0 L{sx} {sy}" stroke="{G_DARK}" stroke-width="{58 + 2 * O}" stroke-linecap="round" fill="none"/>
        <path d="{outer}" fill="{G_DARK}" stroke="{G_DARK}" stroke-width="{2 * (C + O)}" stroke-linejoin="round"/>
        <path d="M0 0 L{sx} {sy}" stroke="url(#green)" stroke-width="58" stroke-linecap="round" fill="none"/>
        <path d="{outer}" fill="url(#green)" stroke="url(#green)" stroke-width="{2 * C}" stroke-linejoin="round"/>
        <path d="{hole_band}" fill="{G_DARK}" stroke="{G_DARK}" stroke-width="{2 * CH}" stroke-linejoin="round"/>
      </g>
      <path d="{hl}" fill="none" stroke="#FFFFFF" stroke-opacity="0.65" stroke-width="11" stroke-linecap="round"
            stroke-dasharray="70 1000" stroke-dashoffset="-18"/>'''

def blade(side):
    """刀刃（朝上，side=-1 左刃 / 1 右刃）：内侧是直的刀口，外侧圆鼓，刀尖圆钝（儿童安全剪刀）。"""
    k = side
    d = (f'M{-6 * k} 44 L{-6 * k} -244 C{-6 * k} -280 {26 * k} -290 {40 * k} -262 '
         f'C{72 * k} -196 {84 * k} -70 {66 * k} 20 C{58 * k} 52 {22 * k} 62 {-6 * k} 44 Z')
    hl = f'M{24 * k} -70 C{30 * k} -140 {32 * k} -196 {28 * k} -232'
    return f'''
      <path d="{d}" fill="url(#yellow)" stroke="{Y_DARK}" stroke-width="13" stroke-linejoin="round"/>
      <path d="{hl}" fill="none" stroke="#FFFFFF" stroke-opacity="0.8" stroke-width="12" stroke-linecap="round"/>'''

SPARKLE = '<path d="M0 -34 C4 -8 8 -4 34 0 C8 4 4 8 0 34 C-4 8 -8 4 -34 0 C-8 -4 -4 -8 0 -34 Z" fill="#FFFFFF"/>'

def scissors_group(px, py, scale=1.1, shadow=True):
    flt = ' filter="url(#soft)"' if shadow else ''
    return f'''
  <g transform="translate({px} {py}) rotate(45) scale({scale})"{flt}>
    <!-- 把手：D 形，外侧圆弧、内侧直边相对 -->
    <g>{handle(-1, 'L')}</g>
    <g>{handle(1, 'R')}</g>
    <g transform="rotate(-13)">{blade(-1)}</g>
    <g transform="rotate(13)">{blade(1)}</g>
    <!-- 转轴螺丝 -->
    <circle r="30" fill="#FFFFFF" stroke="{Y_DARK}" stroke-width="12"/>
    <circle r="10" fill="{Y_DARK}"/>
  </g>'''

DEFS = '''
    <linearGradient id="bg" x1="0" y1="0" x2="0" y2="1">
      <stop offset="0" stop-color="#E8F2FF"/><stop offset="1" stop-color="#C6DCFF"/>
    </linearGradient>
    <linearGradient id="yellow" x1="0" y1="0" x2="1" y2="1">
      <stop offset="0" stop-color="#FFF06A"/><stop offset="1" stop-color="#FFE73C"/>
    </linearGradient>
    <linearGradient id="green" x1="0" y1="0" x2="1" y2="1">
      <stop offset="0" stop-color="#7BE86C"/><stop offset="1" stop-color="#5AD94C"/>
    </linearGradient>
    <filter id="drop" x="-10%" y="-10%" width="120%" height="125%">
      <feGaussianBlur in="SourceAlpha" stdDeviation="14"/><feOffset dy="12"/>
      <feComponentTransfer><feFuncA type="linear" slope="0.28"/></feComponentTransfer>
      <feMerge><feMergeNode/><feMergeNode in="SourceGraphic"/></feMerge>
    </filter>
    <filter id="soft" x="-20%" y="-20%" width="140%" height="140%">
      <feGaussianBlur in="SourceAlpha" stdDeviation="10"/><feOffset dy="12"/>
      <feComponentTransfer><feFuncA type="linear" slope="0.2"/></feComponentTransfer>
      <feMerge><feMergeNode/><feMergeNode in="SourceGraphic"/></feMerge>
    </filter>
    <filter id="plate" x="-30%" y="-30%" width="160%" height="160%">
      <feGaussianBlur in="SourceAlpha" stdDeviation="9"/><feOffset dy="8"/>
      <feComponentTransfer><feFuncA type="linear" slope="0.22"/></feComponentTransfer>
      <feMerge><feMergeNode/><feMergeNode in="SourceGraphic"/></feMerge>
    </filter>
    <filter id="black" color-interpolation-filters="sRGB">
      <feColorMatrix type="matrix" values="0 0 0 0 0  0 0 0 0 0  0 0 0 0 0  0 0 0 1 0"/>
    </filter>'''

def wrap(body, view='0 0 1024 1024', size=1024):
    return f'''<svg xmlns="http://www.w3.org/2000/svg" viewBox="{view}" width="{size}" height="{size}">
  <defs>{DEFS}
  </defs>
{body}
</svg>
'''

def icon(with_badge=True):
    """完整图标（1024 画布，macOS 规范：圆角方底 824、四周留 100 给阴影）。with_badge=False 为小尺寸简化版。"""
    LX, LY, LR = 782, 782, 98
    s = (LR - 10) / 489.0
    body = f'  <path d="{squircle(100, 100, 824)}" fill="url(#bg)" filter="url(#drop)"/>\n'
    if with_badge:
        body += f'  <g transform="translate(250 236) scale(0.75)">{SPARKLE}</g>\n  <g transform="translate(316 182) scale(0.38)">{SPARKLE}</g>\n'
        body += scissors_group(478, 506)
        body += f'''
  <g filter="url(#plate)"><circle cx="{LX}" cy="{LY}" r="{LR}" fill="#FFFFFF"/></g>
  <g transform="translate({LX - 512 * s:.2f} {LY - 512 * s:.2f}) scale({s:.5f})">
{logo()}
  </g>'''
    else:
        # 小尺寸：去掉徽章和亮星，剪刀放大居中
        body += scissors_group(512, 486, scale=1.24)
    return body

def glyph():
    """单色剪影（菜单栏、托盘）：同一把剪刀压成黑色，转轴挖一个小孔。画布 1024，剪刀居中撑满。"""
    return f'''  <mask id="pivot" maskUnits="userSpaceOnUse" x="0" y="0" width="1024" height="1024">
    <rect width="1024" height="1024" fill="#FFFFFF"/><circle cx="512" cy="470" r="34" fill="#000000"/>
  </mask>
  <g filter="url(#black)" mask="url(#pivot)">{scissors_group(512, 470, scale=1.62, shadow=False)}
  </g>'''

os.makedirs(OUT, exist_ok=True)
open(os.path.join(OUT, 'icon.svg'), 'w').write(wrap(icon(True)))
open(os.path.join(OUT, 'icon-small.svg'), 'w').write(wrap(icon(False)))
open(os.path.join(OUT, 'glyph.svg'), 'w').write(wrap(glyph()))
# Windows 不要四周阴影留白：只取圆角方底范围
open(os.path.join(OUT, 'icon-win.svg'), 'w').write(wrap(icon(True), view='96 96 832 832'))
open(os.path.join(OUT, 'icon-win-small.svg'), 'w').write(wrap(icon(False), view='96 96 832 832'))
# 单独的矢量 logo，便于核对重建精度
open(os.path.join(OUT, 'logo.svg'), 'w').write('<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 1024 1024" width="1024" height="1024"><rect width="1024" height="1024" fill="#fff"/>' + logo() + '</svg>')
print('ok')
