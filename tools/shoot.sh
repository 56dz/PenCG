#!/bin/sh
# 真机截图助手：重启 app 到首页，等画面稳定（连续两次截图完全一致）后落盘。
#
# 用法:  sh tools/shoot.sh <输出png前缀>  [等待秒数]
#
# 为什么要「等稳定」：画面里有异步加载的图标和公告，早抓会抓到半成品；
# 之前吃过一次亏 —— 同一构建隔 6 秒抓两次差 62%，其实是画面换了页面。
set -e
ADB="/d/Users/Daniel/code/DSH/x6pro/haas-x6pro/env/adb/adb.exe -s 192.168.5.107:5555"
OUT="$1"
WAIT="${2:-8}"
TMP="${TEMP:-/tmp}"

$ADB shell "hal-screen on" >/dev/null 2>&1 || true
$ADB shell "miniapp_cli start 8002026110200003 index" >/dev/null 2>&1 || true
sleep "$WAIT"

prev=""
i=0
while [ "$i" -lt 6 ]; do
  i=$((i+1))
  $ADB shell "miniapp_cli capture /tmp/_sh.png" >/dev/null 2>&1
  cur=$($ADB shell "md5sum /tmp/_sh.png" | tr -d '\r' | cut -d' ' -f1)
  if [ "$cur" = "$prev" ]; then
    break
  fi
  prev="$cur"
  sleep 3
done

$ADB shell "base64 -w0 /tmp/_sh.png" > "$TMP/_sh.b64" 2>/dev/null
python "$(dirname "$0")/pxdiff.py" decode "$TMP/_sh.b64" "$OUT" >/dev/null
echo "$OUT  md5=$prev  tries=$i"
