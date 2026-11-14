#!/usr/bin/env bash
# 主机侧单元测试：用真实的 native/csrc/cgmedia/cm_rtp.c 处理真实码流。
#
# 为什么要有这个：python 模拟脚本（.tmp/vpu/sim_*.py）只是「模型」，模型与 C
# 不一致时得出的结论毫无价值 —— 本项目已经因此误判过两次。这个 harness 链接
# 的是真代码，除 gst/网络外行为与真机一致。
#
# 用法：bash PenCG/tools/hostut/run.sh [h264 文件] [mtu]
set -e
# 把 h264 参数先转成绝对路径（下面要 cd 进 PenCG）
if [ -n "$1" ]; then
  case "$1" in
    /*|[A-Za-z]:/*) ;;
    *) set -- "$(cd "$(dirname "$1")" && pwd)/$(basename "$1")" "${@:2}" ;;
  esac
fi
cd "$(dirname "$0")/../.."
ZIG="${ZIG_EXE:-../env/pyzig/ziglang/zig.exe}"
SRC="native/csrc/cgmedia"
OUT="${TMPDIR:-/tmp}/pencg-ut"
mkdir -p "$OUT"
"$ZIG" cc -target x86_64-windows-gnu -O1 \
  -I tools/hostut/shim -I "$SRC" \
  -o "$OUT/ut.exe" \
  tools/hostut/ut_main.c "$SRC/cm_rtp.c" "$SRC/cm_util.c"
exec "$OUT/ut.exe" "$@"
