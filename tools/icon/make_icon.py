# -*- coding: utf-8 -*-
"""PenCG 图标生成器 —— 基于网易云游戏官方 logo 的矢量化重绘

为什么要重绘
------------
官方只提供两个位图：
  https://cg.163.com/favicon.ico   48x48   （桌面用，糊）
  https://cg.163.com/logo2.png    180x180  （og:image，可做源图）
  https://cg.163.com/logo.png     144x144  （透明底，云朵本体 112x88）
Falcon 平台硬性要求 app_icon.png 必须是 256x256，直接把 48x48 放大必然糊。
所以这里对 180x180 源图做「软掩膜 -> 1024 超采样 -> 软阈值重建 -> LANCZOS 缩到 256」，
得到边缘干净、无锯齿、细节（缺口/圆点）保留的图。

构图（与官网一致）
------------------
  底  : 深藏青圆角方块 #304051（官网 logo2 的底色，比页面底 #1E2831 亮一档，图标更立得住）
  云  : 官方双箭头云 —— 绿色上行箭头 + 白色下行箭头，官方主绿 #03C47E
  光  : 底部极淡的绿色投影，让云在深底上有厚度

用法
----
    python tools/icon/make_icon.py [源图路径]
默认源图 tools/icon/logo2.png（不是的话先去 cg.163.com/logo2.png 下载）
"""
import os
import sys
from PIL import Image, ImageDraw, ImageFilter, ImageChops

S = 1024                      # 超采样画布
OUT = 256                     # 平台要求的最终尺寸
BG = (48, 64, 81)             # #304051 官方 logo2 底色
GREEN = (3, 196, 126)         # #03C47E 官方主绿
WHITE = (255, 255, 255)
DARK = (0, 0, 0, 0)


def soft_masks(src):
    """把源图拆成 绿/白 两张软掩膜（0..255），保留抗锯齿的边缘过渡。"""
    im = src.convert("RGBA")
    w, h = im.size
    px = im.load()
    g = Image.new("L", (w, h), 0)
    wt = Image.new("L", (w, h), 0)
    gp, wp = g.load(), wt.load()
    for y in range(h):
        for x in range(w):
            r, gg, b, a = px[x, y]
            if a < 8:
                continue
            # 与两种目标色的距离 -> 软权重，避免把抗锯齿像素判成第三色
            dg = abs(r - GREEN[0]) + abs(gg - GREEN[1]) + abs(b - GREEN[2])
            dw = abs(r - WHITE[0]) + abs(gg - WHITE[1]) + abs(b - WHITE[2])
            if dg < dw:
                v = max(0, 255 - dg * 2)
                gp[x, y] = min(255, v * a // 255)
            else:
                v = max(0, 255 - dw * 2)
                wp[x, y] = min(255, v * a // 255)
    return g, wt


def upscale_mask(m, size, crisp=0.55):
    """掩膜放大到 size，然后软阈值重建清晰边缘（再轻微羽化做抗锯齿）。"""
    big = m.resize((size, size), Image.LANCZOS)
    lo, hi = int(255 * crisp - 60), int(255 * crisp + 60)
    if hi <= lo:
        hi = lo + 1
    lut = []
    for v in range(256):
        if v <= lo:
            lut.append(0)
        elif v >= hi:
            lut.append(255)
        else:
            t = (v - lo) / float(hi - lo)
            t = t * t * (3 - 2 * t)          # smoothstep
            lut.append(int(255 * t))
    big = big.point(lut)
    return big.filter(ImageFilter.GaussianBlur(size * 0.0018))


def rounded_mask(size, radius):
    m = Image.new("L", (size, size), 0)
    ImageDraw.Draw(m).rounded_rectangle([0, 0, size - 1, size - 1], radius=radius, fill=255)
    return m


def build(src_path):
    src = Image.open(src_path)
    g_small, w_small = soft_masks(src)

    # 源图里云朵的实际包围盒（去掉四周留白），重绘时重新居中
    bbox = ImageChops.lighter(g_small.convert("L"), w_small.convert("L")).getbbox()
    if not bbox:
        raise SystemExit("源图里找不到云朵（全透明）")
    pad = 2
    cx0, cy0, cx1, cy1 = bbox
    cx0, cy0 = max(0, cx0 - pad), max(0, cy0 - pad)
    cx1 = min(src.size[0], cx1 + pad)
    cy1 = min(src.size[1], cy1 + pad)
    g_small = g_small.crop((cx0, cy0, cx1, cy1))
    w_small = w_small.crop((cx0, cy0, cx1, cy1))
    bw, bh = g_small.size

    # 目标：云朵在画布中占 62% 宽，水平居中，垂直略偏上（视觉重心）
    canvas = Image.new("RGBA", (S, S), (0, 0, 0, 0))
    target_w = int(S * 0.640)
    target_h = int(round(target_w * bh / float(bw)))
    if target_h > S * 0.640:
        target_h = int(S * 0.640)
        target_w = int(round(target_h * bw / float(bh)))
    ox = (S - target_w) // 2
    oy = int(S * 0.5 - target_h * 0.52)

    g_big = upscale_mask(g_small, max(target_w, target_h))  # 先按正方放大
    w_big = upscale_mask(w_small, max(target_w, target_h))
    side = max(target_w, target_h)
    # 等比缩放到目标尺寸（源 bbox 已是云朵紧包围盒，直接 resize 到 target）
    g_big = g_big.resize((target_w, target_h), Image.LANCZOS)
    w_big = w_big.resize((target_w, target_h), Image.LANCZOS)
    assert side > 0

    # 深底
    bg = Image.new("RGB", (S, S), BG)

    # 云影：绿色系投影，向下偏移
    shadow_src = ImageChops.lighter(g_big, w_big)
    sh = Image.new("L", (S, S), 0)
    sh.paste(shadow_src, (ox, oy + int(S * 0.012)))
    sh = sh.filter(ImageFilter.GaussianBlur(S * 0.020))
    bg = Image.composite(Image.new("RGB", (S, S), (1, 78, 56)), bg,
                         sh.point(lambda v: int(v * 0.42)))

    # 白箭头（下层）后绿箭头（上层）
    w_layer = Image.new("L", (S, S), 0)
    w_layer.paste(w_big, (ox, oy))
    bg = Image.composite(Image.new("RGB", (S, S), WHITE), bg, w_layer)

    g_layer = Image.new("L", (S, S), 0)
    g_layer.paste(g_big, (ox, oy))
    bg = Image.composite(Image.new("RGB", (S, S), GREEN), bg, g_layer)

    out = Image.new("RGBA", (S, S), DARK)
    out.paste(bg, (0, 0), rounded_mask(S, int(S * 0.215)))
    out = out.resize((OUT, OUT), Image.LANCZOS)

    here = os.path.dirname(os.path.abspath(__file__))
    dst = os.path.abspath(os.path.join(here, "..", "..", "app_icon.png"))
    out.save(dst, "PNG", optimize=True)
    print("src  ", src_path, src.size)
    print("cloud", (bw, bh), "->", (target_w, target_h), "at", (ox, oy))
    print("out  ", dst, os.path.getsize(dst), "bytes", out.size)


if __name__ == "__main__":
    here = os.path.dirname(os.path.abspath(__file__))
    default = os.path.join(here, "logo2.png")
    build(sys.argv[1] if len(sys.argv) > 1 else default)
