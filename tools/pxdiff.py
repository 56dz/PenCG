# -*- coding: utf-8 -*-
"""真机截图比对助手（布局 px→vw/vh 迁移专用）。

用法：
  python tools/pxdiff.py decode <b64文件> <输出png>       # 设备 base64 -> png
  python tools/pxdiff.py diff <基线png> <新图png>          # 逐像素比对，打印四种口径

口径说明（VW-VH-GUIDE.md §8.2 记载的坑：同一份截图换口径数字差很多，
所以必须四种口径都打印，归档时写清用的是哪一种）：
  rgba  : RGBA 四通道差值之和（0..1020），最宽松
  ch    : 单通道最大差（0..255），最严格也最常用
  gray  : 灰度差（0..255）
  diffpx: 有差异的像素个数（>=1，任一通道）

★ 路径陷阱（本机踩过，务必注意）★
  MSYS bash 与 Windows python 的 /tmp 不是同一个目录：
    bash 的 /tmp   -> C:\Users\<user>\AppData\Local\Temp   （= %TEMP%）
    python 的 /tmp -> D:\tmp
  曾因此把两份不同构建的截图比成「全 0」，得出过错误结论。
  一律传绝对 Windows 路径，不要用 /tmp 开头。diff 第一行会打印读到的
  文件字节数，数字对不上就是读错文件了。
"""
import sys, base64, re

def cmd_decode(src, dst):
    raw = open(src, 'rb').read()
    txt = raw.decode('ascii', 'ignore')
    txt = re.sub(r'[^A-Za-z0-9+/=]', '', txt.split('====')[0])
    pad = (-len(txt)) % 4
    data = base64.b64decode(txt + '=' * pad)
    open(dst, 'wb').write(data)
    print('decoded %d bytes -> %s' % (len(data), dst))

def cmd_diff(a, b):
    from PIL import Image
    A = Image.open(a).convert('RGBA')
    B = Image.open(b).convert('RGBA')
    if A.size != B.size:
        print('SIZE MISMATCH %s vs %s' % (A.size, B.size))
        return 2
    pa, pb = A.load(), B.load()
    w, h = A.size
    n_rgba = n_ch = n_gray = n_diffpx = max_rgba = max_ch = max_gray = 0
    for y in range(h):
        for x in range(w):
            r1, g1, b1, a1 = pa[x, y]
            r2, g2, b2, a2 = pb[x, y]
            d_rgba = abs(r1 - r2) + abs(g1 - g2) + abs(b1 - b2) + abs(a1 - a2)
            d_ch = max(abs(r1 - r2), abs(g1 - g2), abs(b1 - b2), abs(a1 - a2))
            y1 = (r1 * 299 + g1 * 587 + b1 * 114) // 1000
            y2 = (r2 * 299 + g2 * 587 + b2 * 114) // 1000
            d_gray = abs(y1 - y2)
            if d_rgba > 30: n_rgba += 1
            if d_ch > 30: n_ch += 1
            if d_gray > 30: n_gray += 1
            if d_rgba > 0: n_diffpx += 1
            if d_rgba > max_rgba: max_rgba = d_rgba
            if d_ch > max_ch: max_ch = d_ch
            if d_gray > max_gray: max_gray = d_gray
    total = w * h
    print('size            %dx%d  total=%d' % (w, h, total))
    print('rgba >30        %d' % n_rgba)
    print('channel >30     %d      <-- 判据用这个' % n_ch)
    print('gray >30        %d' % n_gray)
    print('diffpx (>0)     %d   (%.3f%%)' % (n_diffpx, 100.0 * n_diffpx / total))
    print('max_rgba        %d' % max_rgba)
    print('max_channel     %d' % max_ch)
    print('max_gray        %d' % max_gray)
    return 0 if n_ch == 0 else 1

if __name__ == '__main__':
    if len(sys.argv) < 2:
        print(__doc__)
        sys.exit(1)
    if sys.argv[1] == 'decode':
        cmd_decode(sys.argv[2], sys.argv[3])
    elif sys.argv[1] == 'diff':
        sys.exit(cmd_diff(sys.argv[2], sys.argv[3]))
    else:
        print(__doc__)
        sys.exit(1)
