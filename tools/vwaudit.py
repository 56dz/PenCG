#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""vwaudit.py —— 静态反投审计：确认每处 px→vw/vh 换算「反推回去仍恰好等于原 px」

为什么需要：运行时把 vw/vh 乘回像素后会取整。若换算值偏小哪怕千分之几，floor 就少 1px，
真机表现为整体位移（历史上 .tab 上移 1px 就是这么来的）。ceil 到 4 位小数可保证
value*unit >= px，从而 floor 恰好回到原值。

做法：解析 `git diff -U0` 的 hunk，把删除行与新增行**按 hunk 内序号一一配对**
（不能简单地按相邻行配对，那是错的：会把 `height:266px` 配到 `width:100vw` 上）。

用法: python tools/vwaudit.py
退出码: 0 全部合格 / 1 有不合格项
"""
import re, subprocess, sys

UNIT = {'vw': 9.6, 'vh': 2.66}
VH_PROPS = ('height', 'top', 'bottom', 'padding-top', 'padding-bottom',
            'margin-top', 'margin-bottom')
VW_PROPS = ('width', 'min-width', 'max-width', 'left', 'right',
            'padding-left', 'padding-right', 'margin-left', 'margin-right',
            'border-radius')
DECL = re.compile(r'(?<![\w-])([a-z-]+)\s*:\s*([0-9.]+)(px|vw|vh)\s*;')
HUNK = re.compile(r'^@@ -(\d+)(?:,(\d+))? \+(\d+)(?:,(\d+))? @@')

FILES = ['src/styles/base.less', 'src/pages/index/index.vue',
         'src/components/mine-body.vue', 'src/pages/search/search.vue',
         'src/pages/login/login.vue', 'src/pages/detail/detail.vue',
         'src/pages/queue/queue.vue', 'src/pages/diag/diag.vue',
         'src/pages/play/play.vue']

def git(*a):
    return subprocess.run(['git'] + list(a), capture_output=True, text=True).stdout

bad, ok = [], 0
for f in FILES:
    diff = git('diff', 'HEAD', '-U0', '--', f)
    olds, news = [], []
    def flush():
        global ok
        # 同一个 hunk 内按序号配对；数量不等说明是纯增/纯删，跳过
        for o, n in zip(olds, news):
            mo, mn = DECL.search(o), DECL.search(n)
            if not (mo and mn and mo.group(3) == 'px' and mn.group(3) in UNIT):
                continue
            prop = mn.group(1); px = float(mo.group(2))
            val = float(mn.group(2)); unit = mn.group(3)
            expect = 'vh' if prop in VH_PROPS else ('vw' if prop in VW_PROPS else None)
            if expect != unit:
                bad.append((f, prop, px, val, unit, 'unit-mismatch expect=%s' % expect)); continue
            back = val * UNIT[unit]
            if int(back) != int(px):
                bad.append((f, prop, px, val, unit, 'floor=%.6f != %s' % (back, px)))
            elif back - px > 0.01:
                bad.append((f, prop, px, val, unit, 'gap %.6f too large' % (back - px)))
            else:
                ok += 1
    for ln in diff.splitlines():
        if HUNK.match(ln):
            flush(); olds, news = [], []
        elif ln.startswith('-') and not ln.startswith('---'):
            olds.append(ln[1:])
        elif ln.startswith('+') and not ln.startswith('+++'):
            news.append(ln[1:])
    flush()

print('合格 %d 处，不合格 %d 处' % (ok, len(bad)))
for f, prop, px, val, unit, why in bad[:40]:
    print('  FAIL %-38s %-14s %gpx -> %g%s  (%s)' % (f, prop, px, val, unit, why))
if len(bad) > 40:
    print('  ... 还有 %d 条' % (len(bad) - 40))
sys.exit(1 if bad else 0)
