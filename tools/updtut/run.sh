#!/bin/bash
# 检查更新模块的主机侧单测。
#
# 做法：把真实的 src/services/updater.js 复制到本目录，再把它的四个依赖
# （net.js / log.js / storage.js / version.js）换成 stub/ 下的空壳，然后用
# node 直接跑断言。被测代码是仓库里那份，不是复刻的模型 —— 改 updater.js
# 之后重跑本脚本即可，不需要同步任何东西。
set -e
cd "$(dirname "$0")"
cp ../../src/services/updater.js ./updater.link.js
cp stub/net.js stub/log.js stub/storage.js stub/version.js ./
node ut.mjs
rc=$?
rm -f updater.link.js net.js log.js storage.js version.js
exit $rc
