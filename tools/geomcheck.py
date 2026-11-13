#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""geomcheck.py —— 布局参数化（px→vw/vh）的四条几何验收（Lead 裁示 C）

★ 路径陷阱：MSYS bash 的 /tmp 是 %TEMP%，Windows python 的 /tmp 是 D:\\tmp。
  一律传**绝对 Windows 路径**，否则会静默比到过期文件（见 docs/VERIFY-playbook.md §0.5 坑 1）。

设计：**自校准**。先用基线图（px 版）测出四条特征的实测值，再用同一套提取器测新图，
要求逐条完全相同。这样不需要我事先知道「哪个绿色块是 .btn」——同一元素在新旧图里的高度
必须一致，任何 1px 漂移都会被抓住。

用法:
  python tools/geomcheck.py <基线png> <新图png>          # 四条对比
  python tools/geomcheck.py <基线png> <新图png> -v       # 附特征明细

四条探针（任一特征不等即 FAIL）:
  1  bar_h      : x=20 竖线上 bar 底色的总像素数（=左栏高度；vh 少了会掉）
  2  tab_y      : x=20 竖线上第一段主绿(3,196,126) 的 (y0,y1)（=第一颗 tab；padding-top+brand 链）
  3  btn_h      : content 区(x>=100) 最大主绿竖段的高度（=某个绿底按钮的高度；vh 换算档位）
  4  cont_right : y=5 行最右侧不透明像素 x（=左栏宽+内容宽 = 100vw）

退出码: 0 全 PASS / 1 有 FAIL / 2 用法错
"""
import sys
from PIL import Image

BAR_BG  = (26, 34, 42)
ACCENT  = (3, 196, 126)
TOL     = 24

def close(c, t, tol=TOL):
    return all(abs(c[i] - t[i]) <= tol for i in range(3))

def runs_at_col(px, x, h):
    out, cur, start = [], px[x, 0], 0
    for y in range(1, h):
        c = px[x, y]
        if c != cur:
            out.append((start, y - 1, cur)); cur = c; start = y
    out.append((start, h - 1, cur))
    return out

def features(path):
    im = Image.open(path).convert('RGBA'); W, H = im.size; px = im.load()
    if (W, H) != (960, 266):
        raise SystemExit('尺寸不是 960x266：%s 是 %dx%d，不是 X6 Pro 视口截图' % (path, W, H))
    f = {'size': (W, H)}

    runs = runs_at_col(px, 20, H)
    # 1) 左栏全高：x=4 是一条不被 tab 打断的纯 bar 底色竖线（实测基线 0..265 单段）
    #    bar 底色 #1A222A 实测 (26,34,42)。用「从 y=0 起连续 bar 底色的高度」，
    #    这样 vh 少 1px（266→265）会立刻掉到 265，溢出也不会被误当成正常。
    top4 = runs_at_col(px, 4, H)
    f['bar_h'] = 0
    if top4 and top4[0][0] == 0 and close(top4[0][2], BAR_BG, 2):
        f['bar_h'] = top4[0][1] + 1

    # 2) 第一段主绿
    seg = None
    for a, b, c in runs:
        if close(c, ACCENT, 8):
            seg = (a, b); break
    f['tab_y'] = seg

    # 3) content 区最大主绿竖段高度
    best_h, best_at = 0, None
    for x in range(100, W):
        for a, b, c in runs_at_col(px, x, H):
            if close(c, ACCENT, 8):
                L = b - a + 1
                if L > best_h:
                    best_h, best_at = L, (x, a, b)
    f['btn_h'], f['btn_at'] = best_h, best_at

    # 4) y=5 行最右不透明像素
    f['cont_right'] = None
    for x in range(W - 1, 70, -1):
        if px[x, 5][3] != 0 and px[x, 5] != (0, 0, 0, 0):
            f['cont_right'] = x; break
    return f

def main():
    args = [a for a in sys.argv[1:] if not a.startswith('-')]
    verbose = '-v' in sys.argv or '--verbose' in sys.argv
    if len(args) != 2:
        print(__doc__); return 2
    base, new = args
    b, n = features(base), features(new)
    print('== geomcheck ==')
    print('   基线 %s' % base)
    print('   新图 %s' % new)
    if verbose:
        print('   基线特征 %s' % {k: b[k] for k in ('bar_h', 'tab_y', 'btn_h', 'btn_at', 'cont_right')})
        print('   新图特征 %s' % {k: n[k] for k in ('bar_h', 'tab_y', 'btn_h', 'btn_at', 'cont_right')})
    checks = [
        ('1 bar_h  (左栏全高，应为 266)', b['bar_h'], n['bar_h']),
        ('2 tab_y  (第一颗 tab 区间)', b['tab_y'], n['tab_y']),
        ('3 btn_h  (绿按钮高度)', b['btn_h'], n['btn_h']),
        ('4 cont_right (内容右边界)', b['cont_right'], n['cont_right']),
    ]
    print()
    fails = 0
    for name, bv, nv in checks:
        ok = bv == nv
        d = '' if ok else ('  Δ=%+d' % ((nv or 0) - (bv or 0)) if isinstance(bv, int) and isinstance(nv, int) else '  Δ=?')
        print('%s  %-30s 基线=%s 新图=%s%s' % ('PASS' if ok else 'FAIL', name, bv, nv, d))
        if not ok: fails += 1
    print()
    if fails == 0:
        print('== 四条几何全中：vh/vw 可用于布局 ==')
        print('   下一步跑 tools/pxdiff.py（>30 应 <=400，几何全中时偏高=抗锯齿残差）')
        return 0
    print('== %d 条不中：先判是几何错位还是抗锯齿，几何错位则该类属性回退 px ==' % fails)
    return 1

if __name__ == '__main__':
    sys.exit(main())
