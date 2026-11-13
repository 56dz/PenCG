# -*- coding: utf-8 -*-
"""布局类 px -> vw/vh 批量转换（规则 4 多机型适配）。

为什么要有这个脚本而不是手改：629 处，手改必出错；而且必须能一眼复核。

单位规则（依据 docs/VW-VH-GUIDE.md 的真机实测：1vw=9.6px、1vh=2.66px）：
  横向 / 固定尺寸类 -> vw（÷9.6）: width min-width max-width left right
                                   padding-left padding-right margin-left margin-right
                                   border-radius
  纵向类          -> vh（÷2.66）: height top bottom
                                   padding-top padding-bottom margin-top margin-bottom
  保持 px 不动    -> border-width（1px 描边必须是 1 个物理像素）、flex-basis（0 值语义特殊）

用法：
  python tools/px2vw.py --dry   <文件...>   # 只打印将要发生的替换，不写盘
  python tools/px2vw.py --apply <文件...>   # 写盘（强制 LF）
  python tools/px2vw.py --apply --no-vh <文件...>  # 只转横向类，纵向类留 px（隔离实验用）
  python tools/px2vw.py --apply --exclude-sel .stage <文件...>
                                                 # 跳过指定选择器内的所有声明（红线保护）
  python tools/px2vw.py --apply --only-props height,margin-top <文件...>
                                                 # 只转这些属性（分批验证用）
"""
import io
import re
import sys
import math

VW = ('width', 'min-width', 'max-width', 'left', 'right',
      'padding-left', 'padding-right', 'margin-left', 'margin-right',
      'border-radius')
VH = ('height', 'top', 'bottom',
      'padding-top', 'padding-bottom', 'margin-top', 'margin-bottom')
SKIP = ('font-size', 'line-height', 'border-width', 'flex-basis', 'border-top-width',
        'border-bottom-width', 'border-left-width', 'border-right-width')

LINE_RE = re.compile(r'^(\s*)([a-zA-Z-]+)(\s*:\s*)([0-9.]+)(px)(\s*;?\s*)$')

# 小值保护：1px 的描边/分隔线/间距在 vw 下会变成 0.1042vw ≈ 0.9999px，
# 取整后可能变 0 而消失。这些一律保持 px。
MIN_KEEP_PX = 1.0

STYLE_OPEN = re.compile(r'<style[^>]*>')
STYLE_CLOSE = re.compile(r'</style>')


def fmt(n):
    """4 位小数，**向上取整**，去尾零。

    为什么必须向上取整（2026-11-14 真机实测）：
    朴素四舍五入会让换算结果落在整数下方千分之一，例如
      8px / 2.66 = 3.007519...  ->  取 4 位得 3.0075
      3.0075 * 2.66 = 7.99995   ->  运行时取整得 7px，**少 1px**
      38px / 2.66 = 14.285714  ->  14.2857 * 2.66 = 37.99996 -> 37px，少 1px
    实测后果：左侧导航 5 个 tab 全部上移 1px、品牌圆标上移 2px，
    逐像素比对 channel>30 从 0 变成 1422（判据不通过）。
    向上取整后 3.0076 * 2.66 = 8.000216，取整仍是 8px，误差 <0.001px 可忽略。
    """
    v = math.ceil(n * 10000) / 10000.0
    s = '%.4f' % v
    if '.' in s:
        s = s.rstrip('0').rstrip('.')
    return s or '0'


def conv(prop, px):
    if prop in VW:
        return fmt(px / 9.6), 'vw'
    if prop in VH:
        return fmt(px / 2.66), 'vh'
    return None, None


def line_in_style(lines, idx):
    """判断第 idx 行是否位于 <style> 块内（.less 文件视为全程在 style 内）。"""
    depth = 0
    for i in range(idx + 1):
        line = lines[i]
        for _ in STYLE_OPEN.finditer(line):
            depth += 1
        for _ in STYLE_CLOSE.finditer(line):
            depth -= 1
    return depth > 0


SEL_RE = re.compile(r'^\s*([^@/][^{};]*?)\s*\{\s*$')


def sel_excluded(cur_sel, excludes):
    if not excludes:
        return False
    for ex in excludes:
        if cur_sel.startswith(ex):
            return True
    return False


def process(path, dry, excludes=(), only_props=()):
    is_less = path.endswith('.less')
    text = io.open(path, encoding='utf-8', newline='').read()
    lines = text.split('\n')
    changes = []
    out = []
    cur_sel = ''
    for i, line in enumerate(lines):
        sm = SEL_RE.match(line)
        if sm:
            cur_sel = sm.group(1).strip().replace(' ', '')
        m = LINE_RE.match(line)
        if not m:
            out.append(line)
            continue
        if sel_excluded(cur_sel, excludes):
            out.append(line)
            continue
        if only_props and m.group(2) not in only_props:
            out.append(line)
            continue
        indent, prop, sep, val, _unit, tail = m.groups()
        if prop in SKIP:
            out.append(line)
            continue
        px = float(val)
        if px <= MIN_KEEP_PX:
            out.append(line)
            continue
        if not is_less and not line_in_style(lines, i):
            out.append(line)
            continue
        # 纯满屏高度：266px 正好是 100vh，直接写满屏比 99.6241vh 更准确
        if prop == 'height' and abs(px - 266.0) < 0.01:
            new = indent + prop + sep + '100' + 'vh' + tail
            changes.append((i + 1, line.strip(), new.strip()))
            out.append(new)
            continue
        # 纯满屏宽度同理
        if prop == 'width' and abs(px - 960.0) < 0.01:
            new = indent + prop + sep + '100' + 'vw' + tail
            changes.append((i + 1, line.strip(), new.strip()))
            out.append(new)
            continue
        v, unit = conv(prop, px)
        if v is None:
            out.append(line)
            continue
        new = indent + prop + sep + v + unit + tail
        changes.append((i + 1, line.strip(), new.strip()))
        out.append(new)

    new_text = '\n'.join(out)
    print('=== %s : %d 处 ===' % (path, len(changes)))
    if dry:
        for ln, old, new in changes:
            print('  %4d  %-34s -> %s' % (ln, old, new))
    if not dry and changes:
        io.open(path, 'w', encoding='utf-8', newline='\n').write(new_text)
        print('  written, %d bytes' % len(new_text.encode('utf-8')))
    return len(changes)


if __name__ == '__main__':
    if len(sys.argv) < 3:
        print(__doc__)
        sys.exit(1)
    args = sys.argv[1:]
    dry = '--dry' in args
    if '--no-vh' in args:
        # 隔离实验用：纵向类完全不改，验证「差异是否只来自 vh」
        globals()['VH'] = ()
    excludes = ()
    only_props = ()
    for a in args:
        if a.startswith('--exclude-sel='):
            excludes = tuple(x.strip() for x in a.split('=', 1)[1].split(',') if x.strip())
        elif a.startswith('--only-props='):
            only_props = tuple(x.strip() for x in a.split('=', 1)[1].split(',') if x.strip())
    files = [a for a in args if not a.startswith('--')]
    total = 0
    for p in files:
        total += process(p, dry, excludes, only_props)
    print('TOTAL %d' % total)
