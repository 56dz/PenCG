# 检查更新的主机侧单测

`src/services/updater.js` 里有一半是**纯逻辑**（版本比较、资产挑选、镜像 URL、
提示决策、正文切行），这些完全可以脱离设备验证。本目录就是干这个的。

## 跑法

```bash
bash PenCG/tools/updtut/run.sh
```

不需要设备、不需要 QuickJS、不需要装依赖，只需要 node。

## 做法

把真实的 `src/services/updater.js` 复制成本目录的 `updater.link.js`，再把它的
四个依赖（`net.js` / `log.js` / `storage.js` / `version.js`）换成 `stub/` 下的
空壳，然后 `node ut.mjs` 跑断言。

**被测代码是仓库里那份，不是复刻的模型。** 改 `updater.js` 之后重跑即可，
不需要同步任何东西 —— 这一点与本项目 `tools/hostut` 的动机一样：模型与 C
不一致时，模拟出来的结论一文不值。

## 断言覆盖（76 条）

| 分组 | 覆盖 |
|---|---|
| `parseVersion` | `v` 前缀、缺位、空串/非字符串/垃圾输入、预发布标记 |
| `compareVersion` | 数值比较（`1.10 > 1.9`）、缺位补 0、预发布更小、解析失败返回 0 |
| `isNewer` | 更新 / 相同 / 更旧 / 不可解析一律不提示 |
| `normalizeRelease` | draft / prerelease / `release_status==pre` 全部过滤；`tag_name` 非法返回 null；字符串 body 也认 |
| 资产挑选 | **`PenCG.amr` 存在但不在第一位时必须仍选中它**（发布时源码包排在前面）；没有时回退 `assets[0]` |
| `isMirrorable` | 只认 `github.com` / `*.github.com` / `githubusercontent.com` / `*.githubusercontent.com`；`evilgithub.com`、`github.com.evil.io`、非 http 全部拒绝 |
| `downloadUrl` | 镜像前缀只加在可镜像的地址上，GitCode 地址永不套镜像 |
| `bodyLines` | 去 Markdown（标题/列表/粗体/行内代码/链接）、连续空行压成一个、首尾空行去掉、首行裸版本号删除、`maxLines` 上限 |
| `dayKey` | `YYYY-MM-DD` 且用本地时区 |
| `shouldAutoPrompt` | 跳过该版本 / 今天内不提示 / 不比它新 —— 三条都拦住 |
| `extractFilePath` | `{result:"/p"}`、`{result:{filePath}}`、`{filePath}` 三种返回形态 |
| 发布硬约束 | `ASSET_NAME === 'PenCG.amr'`、`SAVE_DIR` 固定 |

## 变异测试（确认断言有牙）

把 `updater.js` 里这几处分别改坏，断言必须失败：

| 变异 | 失败的断言数 |
|---|---|
| `compareVersion` 改成字符串比较 | 1 |
| 忽略 prerelease 标记 | 1 |
| 不过滤 draft | 1 |
| **资产挑选改成 `assets[0]`** | **3** |
| 镜像不校验域名 | 4 |
| **连续空行不压缩** | **2** |
| 不删首行裸版本号 | 1 |
| `quietDay` 判定失效 | 1 |

> 「资产挑选」与「空行压缩」这两条最初是被变异测试抓出来的 —— 原来的断言
> 只测了「没有 PenCG.amr 时回退到第一个」，没测「PenCG.amr 在第二位时仍被
> 选中」，而这个场景恰恰是真实发布时的形态（源码包永远排在最前面）。
