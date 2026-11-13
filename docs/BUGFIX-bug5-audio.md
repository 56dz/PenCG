# BUGFIX-bug5-audio —— 系统音量 0 无效 + 退出后其他应用也有声

> 排查对象：PenCG（有道词典笔 X6 Pro 网易云游戏客户端）
> 真机：X6 Pro / RK3562 / 设备 `9D91500007505171`
> 日期：2026-11-12
> 基准读数全文：[audio-baseline.txt](audio-baseline.txt)
> 涉及文件：`PenCG/native/csrc/cgmedia/cm_gst.c`、`cm_session.h`、`cm_cmd.c`

---

## 0. 用户报的现象

1. 下拉系统控制栏把音量调到 **0**，进入游戏后**还是有声音**。
2. 退出游戏后，使用其他软件（包括**有道查词**）**也会出现声音**。

两条现象是同一个 bug 的两半：我们把用户设成 0 的功放音量抬到了 200（现象 1），
退出时又不还原（现象 2）。

---

## 1. 根因

### 结论速览

| 假设 | 结论 | 一句话 |
|---|---|---|
| **A** 功放 `0,0 → 200,200` 违反用户意图 | ✅ **确认** | 系统音量 0 就是 card1 `numid=54` = `0,0`，旧代码把它写成 `200,200` |
| **B** 改了全局 mixer 却从不还原 | ✅ **确认** | `Playback Path` 拨到 SPK 不还原、功放抬到 200 不还原，四条收尾路径一条都没还 |
| **C** fdsink 的 fd 没关 → aplay 不退出独占声卡 | ⚠️ **部分否定** | fd 确实关了、SIGCHLD=SIG_IGN 下不会有僵尸；但 SIGTERM 紧跟 SIGKILL（无间隔）导致 aplay 走不到正常收尾，并且**没有任何「aplay 真的死了」的自证**，卡死时现场无证据 |

---

### A. 功放音量：`values=0,0 → cset 200,200` —— **确认**

#### 出问题的代码（修复前，`cm_gst.c:291-311`）

```c
static void audio_amp_volume_ensure(void) {
    char line[256];
    int zero = 0;
    FILE *fp = popen(AUDIO_AMIXER_PATH " -c 1 cget numid=54 2>/dev/null", "r");
    if (!fp) return;
    while (fgets(line, sizeof(line), fp)) {
        char *v = strstr(line, ": values=");
        if (v) { zero = (strncmp(v, ": values=0,0", 12) == 0); break; }
    }
    pclose(fp);
    if (zero) {
        CM_LOGI("audio amp volume 0 -> 200");
        system(AUDIO_AMIXER_PATH " -c 1 cset numid=54 200,200 >/dev/null 2>&1");
    }
}
```

调用点在 `audio_route_ensure()` 的 OFF 分支（修复前 `cm_gst.c:314-335`）：

```c
static void audio_route_ensure(void) {
    ...
    if (strcmp(cur, "OFF") != 0) { CM_LOGI("Playback Path already %s, leave it", cur); return; }
    audio_mixer_set_path("SPK");
    audio_amp_volume_ensure();   // ← 就是这一行
}
```

#### 真机证据：系统音量 0 到底落在哪个控件

三条互相独立的证据，全部来自 adb 只读实测（细节见 [audio-baseline.txt](audio-baseline.txt) 第 4 节）：

**证据 1 — `/etc/asound.conf` 的软音量绑在 card1**

```
pcm.!default   = asym, playback.pcm = "speaker"
pcm.speaker    = plug -> spk_softvol
pcm.spk_softvol = softvol {
      control { name "Master Playback Volume" card 1 device 0 }
      min_dB -46.08  max_dB -0.18  resolution 256
      slave.pcm "resample_spk"  ->  hw:1,0, 48000, 2ch
}
```

`/bin/aplay` 不带 `-D` 时走 `!default`，**音量环节就是 card1 的 softvol**，
最终直接写 `hw:1,0`。声音根本不经过 rk817（card0）的模拟输出级。

**证据 2 — `hal-audio` 配置写死了 numid**

```
$ /bin/hal-audio -l
speaker_card_id=1
speaker_subdevice_id=0
speaker_vol_ctl_numid=54     <-- 系统音量控件 = card1 numid 54
mic_card_id=0
analog_*_hs_vol_ctl_numid=51
digital_hs_card_id=2
```

**证据 3 — 改系统音量只动这一个控件（diff 实验）**

```
$ adb shell 'hal-audio -s 50'
（前后各抓一次 amixer -c 0 contents + amixer -c 1 contents，全量 diff）
=> 唯一变化：card1 numid=54   values=0,0 -> values=176,176
   card0 的所有控件（含 Playback Path / Master / Speaker Switch）一个都没变
```

映射实测（非线性）：

| `hal-audio -s` | card1 `numid=54` |
|---|---|
| 0 | `0,0` |
| 30 | `128,128` |
| 50 | `176,176` |
| 100 | `255,255` |

> ⚠️ `hal-audio -g` 是**坏接口**：mixer 是 176,176 时它返回 0，mixer 是 0,0 时也返回 0。
> 判音量只能 `amixer -c 1 cget numid=54`。`-s` 本身可靠。

#### 决定性证据：声学 2×2 矩阵

方法：`aplay -q -t raw -f S16_LE -r 48000 -c 2 - < /dev/urandom` 播放 2 秒，
同时 `arecord -q -D hw:0,0 -t raw -f S16_LE -r 48000 -c 2 -d 2` 录音，
用 od + awk 算 16bit 采样的绝对值均值 / 峰值。
（录音前 `amixer -c 0 cset name="Capture MIC Path" "Main Mic"`，录完还原 `MIC OFF`。）

| 条件 | 均值 | 峰值 | 判读 |
|---|---|---|---|
| 完全不放音 | 83 | 240 | 本底噪声 |
| Path=OFF, amp=`0,0` | 90 | 553 | **静音** |
| Path=OFF, amp=`200,200` | 9339 | 32768 | **大声** ← 旧代码在音量 0 时干的事 |
| Path=SPK, amp=`0,0` | 102 | 605 | **静音** |
| Path=SPK, amp=`200,200` | 9366 | 32768 | **大声** |

两个结论：

1. **`OFF` 与 `SPK` 对「是否出声」没有任何差别**（90 vs 102，都在本底噪声量级）。
2. **唯一决定响度的是 card1 `numid=54`**：`0,0` 静音，`200,200` 满响度。

所以旧代码在用户把音量拖到 0 时做的「`values=0,0` → `cset 200,200`」，
就是把矩阵第 2 行变成第 3 行 —— **这正是用户听到的那声不该有的声音**。

#### 为什么这条路径一定被触发

开机空闲态实测：`Playback Path` = `values=0`（**OFF**），card1 `numid=54` = `0,0`。
`audio_route_ensure()` 一旦读到 OFF 就走 OFF 分支，于是**每次起播都会**：

```
Playback Path OFF -> SPK   （这一改无害，但也没必要）
audio amp volume 0 -> 200  （这一改就是 bug）
```

---

### B. 改了全局 mixer 却从不还原 —— **确认**

#### 我们改了两样全局状态，一样都没还

| 全局状态 | 改它的代码 | 还原它？ |
|---|---|---|
| card0 `Playback Path` OFF → SPK | 修复前 `cm_gst.c:327`（`audio_mixer_set_path("SPK")`） | ❌ 无 |
| card1 `numid=54` 0,0 → 200,200 | 修复前 `cm_gst.c:306`（`system("... cset numid=54 200,200")`），调用点 `cm_gst.c:334` | ❌ 无 |

`cm_box_destroy()`（修复前 `cm_gst.c:877-898`）只做 GStreamer 拆除 + `aplay_stop()`，
**没有任何 mixer 还原**。

#### 为什么「从不还原」能一直留到用户回到系统

真机确认**没有任何系统守护进程替我们管这个控件**：

```
$ strings <各二进制> | grep -c "Playback Path"
/bin/hal-audio                            -> 0
/usr/bin/miniapp                          -> 0
/usr/bin/weston                           -> 0
/usr/bin/runSoundPlayer                   -> 0
/oem/YoudaoDictPen/output/SoundPlayer     -> 0
/oem/YoudaoDictPen/output/SoundRecord     -> 0
/data/local/bluealsa_custom/bluealsa_orig -> 0
```

反向也验证过：打开默认 PCM 播放**不会**自动改 Playback Path，
也**不会**改 card1 `numid=54`（176,176 播放前后不变）。

再加上 `/var/lib/alsa` 为空（没有 alsactl 常驻保存），
**我们改的 mixer 只有我们自己还原；我们不还，它就一直是 200,200。**

于是退出游戏后，用户拿着一个「系统音量 0、功放 200」的机器回到桌面：
有道查词的提示音、SoundPlayer（`/oem/YoudaoDictPen/output/SoundPlayer`）的发音
全部按 200/255 的音量放出来 —— 现象 2）

#### 四条收尾路径，一条都没还原

| 路径 | 位置 | 修复前 |
|---|---|---|
| 看门狗超时 / 正常退出 | `cm_media.c:338-339` | 只 destroy，无还原 |
| 会话释放 | `cgmedia.c:156-157` | 只 destroy，无还原 |
| JS 侧 stop | `cgmedia.c:322-324` | 只 destroy，无还原 |
| `setAudio {on:0}` | `cm_cmd.c:129-135` | 只 destroy，无还原 |

四条最终都走 `cm_box_destroy()` —— 所以还原只要挂在 `cm_box_destroy` 的音频分支上，
一处实现就覆盖全部路径（本次修复正是这么做的）。

---

### C. fdsink fd 未被关闭 → aplay 不退出、独占声卡 —— **部分否定**

#### 否定部分：fd 确实关了，也没有僵尸

修复前的 `aplay_stop()`（`cm_gst.c:703-713`）已经先 `close(b->aplay_fd)` 再发信号：

```c
static void aplay_stop(cm_box *b) {
    if (!b) return;
    if (b->aplay_fd >= 0) { close(b->aplay_fd); b->aplay_fd = -1; }      // ← fd 关了
    if (b->aplay_pid > 0) {
        kill(b->aplay_pid, SIGTERM);
        kill(b->aplay_pid, SIGKILL);   // ← 两行连发
        b->aplay_pid = 0;
    }
}
```

`gst-inspect-1.0 fdsink`（GStreamer 1.22.0）属性全集：
`async / blocksize / enable-last-sample / fd / last-sample / max-bitrate / max-lateness /
name / parent / processing-deadline / qos / render-delay / stats / sync / throttle-time /
ts-offset` —— **没有 close-fd**，所以 fdsink 不会替我们关，我们自己关是对的。

僵尸也不会有：`/proc/968/status` 显示 `SigIgn: 0000000000011006`（bit16 = SIGCHLD 被忽略），
内核会自动回收子进程。`waitpid` 在这些条件下返回 `ECHILD`，**不是**能用的收尾手段。

真机空闲态 `pidof aplay` 为空、`card0/card1` pcm status 都是 `closed`，
说明「aplay 卡住永久独占声卡」不是常态。

#### 确认部分：收尾质量不够，且卡死时没有证据

三个真实缺陷：

1. **`SIGTERM` 紧跟着 `SIGKILL`，中间没有任何等待**。
   SIGKILL 几乎总是先到，aplay 根本没机会走正常的 `snd_pcm_stop/drain` 收尾 ——
   这就是「退出瞬间有残响 / 爆音」的来源。
2. **先发信号再关 fd 的顺序在语境上更容易让 aplay 直接被砍**，
   而正确的顺序恰好是反的：先关写端让 `read()` 返回 0（EOF），aplay 自己 flush 退出。
3. **没有任何「aplay 是否真的死了」的自证**。
   一旦哪天真卡在 `TASK_UNINTERRUPTIBLE`（ALSA 驱动里），日志里就完全看不出来，
   这正是 Bug5 的 C 假设想问的事情。修复后这条假设可以被日志直接判定。

#### 顺带发现：`cg_aplay.log` 里的 XRUN

```
/userdisk/miniapp/data/cg_aplay.log（19350 B，376 行）
bluealsa-pcm.c:895: Getting BlueALSA PCM: PLAYBACK B0:A3:F2:AD:7E:FA a2dp
S16_LE / 48000 / Stereo / SBC，HW buffer 5 x 5760 = 28800 B
underrun!!! (at least 232650.770 ms long)
underrun!!! (at least 14376.547 ms long)
underrun!!! (at least 7408.337 ms long)
underrun!!! (at least 3300.751 ms long)
state : XRUN
```

这是**蓝牙分支**的缓冲/时钟问题（`-B 150000 -F 30000` + `fdsink sync=false`），
和 Bug5 的「静音失效」是两件独立的事，但共用同一段 `aplay_stop()` 收尾逻辑。
本次不扩大范围处理 XRUN，只在文档里留档。

---

## 2. 修复方案

### 设计原则：**尊重系统音量 + 完整还原**

| 原则 | 做法 |
|---|---|
| 不替用户调音量 | **彻底删掉** `audio_amp_volume_ensure()`。card1 `numid=54` 从此只读，一个字节都不写 |
| 改过的才还原 | 开播前快照；还原时**只写回我们自己写过、且当前值仍是我们的值**的项（用户中途插耳机/连蓝牙就 leave it） |
| 幂等 | 快照标记 `audio_snap_done` 先置位再干活，重入直接返回；四条收尾路径都调也只还原一次 |
| 不留死角 | 建盒失败的两条早退路径也还原（否则路由永久留在 SPK） |
| aplay 收尾可自证 | 关 fd → 等 EOF 自退 → SIGTERM 等待 → SIGKILL 等待 → 还活着就 ERROR 落日志 |

### 关键判断依据（写清依据，不是拍脑袋）

**1. 什么时候才改 Playback Path？**

只在 `strcmp(cur, "OFF") == 0` 时改。依据：
- 空闲态真机实测 `values=0`（OFF）是出厂默认（`/etc/asound.state.melon` 里 DAC Playback Path 也是 OFF）；
- 而 we 需要一个「功放通路已接通」的态才能出声；
- 已经是 `HP` / `BT` / `SPK` 等其他值时说明路由已经就绪或是用户的选择，**绝不覆盖**。

**2. 为什么 `Playback Path` 还要拨、但 `numid=54` 绝不能动？**

声学矩阵已经证明 OFF 和 SPK 对响度**没有差别**（90 vs 102），
所以严格说拨 `Playback Path` 对出声不是必需的 —— 但它是既有行为、且在某些
codec 状态下可能影响通路使能，保留「OFF→SPK」可以避免行为回退；
而 card1 `numid=54` 是**用户音量本体**，动它等于替用户拖音量条，必须彻底禁止。

**3. 怎么判断「系统音量是 0」？**

不去判断。这是本次修复最重要的取舍 —— **旧的判断逻辑本身就是 bug 的来源**。
正确做法是：**我们根本不需要知道用户音量是多少**，因为我们不再碰音量。
判音量只能 `amixer -c 1 cget numid=54`（`hal-audio -g` 恒返回 0，不可信），
但既然不写，就只在退出时回读一次做日志自证。

### 快照 / 还原的数据结构

放在 `cm_sess` 而不是 `cm_box`（`cm_session.h:200-219`）：

```c
    /* ---- 音频路由快照 / 还原（Bug5）---- */
    int  audio_snap_done;              /* 快照已做（0=没改过，还原时直接返回） */
    char audio_path_snap[32];          /* 进游戏前 Playback Path 的原文，如 "OFF" */
    char audio_path_wrote[32];         /* 我们实际写下去并回读确认的值；空=没改 */
    char audio_amp_snap[32];           /* 进游戏前 card1#54，如 "0,0"（只读） */
    char audio_amp_after[32];          /* 退出时 card1#54，用于日志自证（只读） */
```

为什么在会话上：盒子可能创建失败，也可能先于会话结束被销毁
（`setAudio(0)` / `setVideoVisible` 重建），而还原必须在任何一条拆管线路径上幂等跑一次。

### aplay 收尾：四阶段 + 自证

```
① close(fd)                         关写端 → aplay read() 返回 0 (EOF) → 自己 flush 退出
   └ 轮询 20 × 10ms (200ms)          成功：LOG "exited on EOF after N ms"
② kill(SIGTERM)                     让它正常收尾
   └ 轮询 30 × 10ms (300ms)          成功：LOG "exited on SIGTERM after N ms"
③ kill(SIGKILL)                     兜底
   └ 轮询 50 × 10ms (500ms)          成功：LOGW "killed after N ms (audio may clip)"
④ 还活着                             LOGE "UNKILLABLE after N ms (stuck in driver?)"
```

存活探针用 `kill(pid, 0)` 而不是 `waitpid`：

```c
static int aplay_alive(pid_t pid) {
    if (pid <= 0) return 0;
    if (kill(pid, 0) == 0) return 1;
    return errno == EPERM;   /* EPERM = 存在但没权限，保守当成活着 */
}
```

理由：宿主 `SIGCHLD=SIG_IGN`（真机 `SigIgn: 0000000000011006` 已证实），
`waitpid` 返回 `ECHILD` 拿不到状态；但内核仍会在子进程退出时释放 pid，
所以 `kill(pid, 0)` 是可靠的存活探针。

---

## 3. 最小补丁

### 3.1 `PenCG/native/csrc/cgmedia/cm_gst.c`

| 位置（修复后行号） | 改动 |
|---|---|
| `cm_gst.c:18` | 新增 `#include <errno.h>`（`aplay_alive` 判 ESRCH 用） |
| `cm_gst.c:151` | `struct cm_box` 新增 `cm_sess *sess;`（销毁时按会话快照还原；不改 `cm_box_destroy` 签名，因为它被禁改的 `cm_media.c`/`cgmedia.c` 调用） |
| `cm_gst.c:260-266` | 音频宏重写，新增 `AUDIO_AMP_CARD "-c 1"` / `AUDIO_AMP_CTL "numid=54"`（**只读**） |
| `cm_gst.c:345-374` | 新增 `audio_amp_read()`：只读 card1#54，匹配 `": values="`（**不能匹配 `values=`**，否则把 `; type=INTEGER,...,values=2` 的声道数 2 当音量） |
| `cm_gst.c:379-397` | 新增 `audio_snapshot()`：幂等快照 Playback Path + card1#54 |
| `cm_gst.c:412-446` | 新增 `cm_audio_route_restore()`（导出）：条件还原 + 自证日志 |
| `cm_gst.c:450-476` | `audio_route_ensure(void)` → `audio_route_ensure(cm_sess *s)`：先快照，再按 OFF 分支拨 SPK，回读确认后记 `audio_path_wrote`；**删掉 `audio_amp_volume_ensure()` 调用** |
| ~~`cm_gst.c:291-311`（原）~~ | **整段删除 `audio_amp_volume_ensure()`**（`values=0,0 → cset 200,200` 就在这里） |
| `cm_gst.c:849-865` | 新增 `aplay_alive(pid_t)` 存活探针 |
| `cm_gst.c:867-923` | 重写 `aplay_stop()`：EOF → SIGTERM → SIGKILL 四阶段 + 自证日志 |
| `cm_gst.c:995` | `cm_box_create()` 里 `b->sess = s;` |
| `cm_gst.c:1034` | 调用点改 `audio_route_ensure(s);` |
| `cm_gst.c:1042` | `aplay_spawn` 失败分支补 `cm_audio_route_restore(s);`（盒子没交出去，`cm_box_destroy` 不会被调用） |
| `cm_gst.c:1051-1058` | `launch` 失败分支从 `aplay_stop(b); free(b);` 改成 `cm_box_destroy(b);`（收尾逻辑统一） |
| `cm_gst.c:1117` | `cm_box_destroy()` 末尾加 `if (!b->is_video) cm_audio_route_restore(b->sess);` |

### 3.2 `PenCG/native/csrc/cgmedia/cm_session.h`

| 位置 | 改动 |
|---|---|
| `cm_session.h:200-219` | `struct cm_sess` 在 `pthread_mutex_t gst_mu;` 之后新增 5 个音频快照字段 + 说明注释 |
| `cm_session.h:280` | 声明区新增 `void cm_audio_route_restore(cm_sess *s);` |

### 3.3 `PenCG/native/csrc/cgmedia/cm_cmd.c`

| 位置 | 改动 |
|---|---|
| `cm_cmd.c:137` | `setAudio {on:0}` 分支在 `cm_box_destroy` + `s->abox = NULL` 之后补一次 `cm_audio_route_restore(s);`（幂等兜底，覆盖「盒子早就没了但快照标记还挂着」的边角情况） |

### 3.4 核心代码

```c
/* 把我们在 audio_route_ensure 里改过的全局 mixer 状态还原。**幂等**。 */
void cm_audio_route_restore(cm_sess *s) {
    char cur[64];
    char after[64];
    if (!s || !s->audio_snap_done) return;   /* 没快照过 = 没改过 = 无事可做 */
    s->audio_snap_done = 0;                  /* 先置位，保证重入时直接返回 */

    if (s->audio_path_wrote[0]) {
        if (audio_mixer_get_path(cur, sizeof(cur)) == 0 &&
            strcmp(cur, s->audio_path_wrote) == 0) {
            (void)audio_mixer_set_path(s->audio_path_snap);
            ...
        } else {
            /* 当前值已经不是我们写的那个：用户自己改过路由，尊重他。 */
            CM_LOGI("#%d audio restore: Playback Path now %s (not ours %s), leave it", ...);
        }
        s->audio_path_wrote[0] = 0;
    }

    /* 自证：把退出时的系统音量打进日志，和开播前快照对照。 */
    if (s->audio_amp_snap[0] &&
        audio_amp_read(s->audio_amp_after, (int)sizeof(s->audio_amp_after)) == 0) {
        if (strcmp(s->audio_amp_snap, s->audio_amp_after) == 0)
            CM_LOGI("#%d audio restore: amp(card1#54) %s unchanged (we never write it)", ...);
        else
            CM_LOGI("#%d audio restore: amp(card1#54) %s -> %s (changed by system, not us)", ...);
    }
}
```

---

## 4. 真机验证步骤（adb 命令级）

### 步骤 -1 — 取证通道（**先读这个**，否则下面所有 grep 都是空的）

X6 Pro 的 Falcon 运行时里，JS 的 `console` **只有 `console.error` 会写盘**；
`console.warn` / `console.log` 完全不落盘（全盘 `grep -a -c 'W][JS]'` 计数为 0）。
落盘文件是 **`/userdata/applog/loli_s6_dev.txt`**（不是 `DictPen_*.log`）。

```bash
ADB="./env/adb/adb.exe -s 9D91500007505171"
$ADB shell "grep -a -n 'pencg' /userdata/applog/loli_s6_dev.txt | tail -50"
```

行形态固定：

```
[2026-11-12 22:15:24.947][E][JS] [pencg][I] [media] SDP video pt=96 ...
└──────────┬────────────┘ └┬┘ └┬┘ └────┬────┘
        落盘时间戳        级别 来源   PenCG 自己打的级别前缀
```

- **落盘级别恒为 `[E]`**（底层通道是 `console.error`），跟严重程度无关。
- **真正的级别只看行内 `[pencg][I|W|E]`**（这条只对 JS 侧成立）。

**⚠ 但本文档 §4 里绝大多数日志行不是 JS 侧的**：`audio snapshot` / `Playback Path ... -> SPK` /
`aplay pid=... exited` / `audio restore` 全部由 **native 侧 `cm_log()`** 打出
（`PenCG/native/csrc/cgmedia/cm_util.c:19-29`，`fprintf(stdout, "[cgmedia] ")` + `fflush`），
它是**与 JS console 完全独立的第二条通道**，前缀是 `[cgmedia]` 而不是 `[pencg]`。
所以本文下方所有「期望日志」示例都已按 `[cgmedia]` 书写（早期版本误写成 `[pencg]`，已修正）：

```
[cgmedia] #3 audio snapshot: Playback Path=OFF amp(card1#54)=0,0
[cgmedia] aplay pid=2412 exited on EOF after 10 ms
```

- native 输出是否也落进 `loli_s6_dev.txt`、还是只进 `stdout`（取决于 Falcon 怎么接 `miniapp` 的 stdout）
  **待现场确认**。现场若只看到 JS 侧 `[pencg]` 行、一条 `[cgmedia]` 都没有，
  先按「native 走了另一路输出」排查，**不要直接判定修复没生效**。
- 建议的抓取方式（两条通道一起看）：`grep -a -e '\[pencg\]' -e '\[cgmedia\]'`。

> 前置（假设已在 `haas-x6pro/` 目录、`ADB=./env/adb/adb.exe`、设备号 `9D91500007505171`）：

```bash
ADB="./env/adb/adb.exe -s 9D91500007505171"
```

### 步骤 0 — 抓修复前的基准（确认设备干净）

```bash
$ADB shell "amixer -c 1 cget numid=54 | grep -o 'values=[0-9,]*' | tail -1"   # 期望 values=0,0
$ADB shell "amixer -c 0 cget name='Playback Path' | grep -o 'values=[0-9,]*' | tail -1"  # 期望 values=0 (OFF)
$ADB shell pidof aplay                                                        # 期望空
$ADB shell 'cat /proc/asound/card1/pcm0p/sub0/status'                         # 期望 closed
```

> ⚠️ 读 `cget` **必须 `grep -o "values=[0-9,]*"`**。直接 `tail -1` 会拿到 `| dBscale-min=...` 那一行。

### 步骤 1 — 系统音量 0，进游戏应当**完全静音**（Bug5 现象 1）

```bash
# 1) 系统音量拉到 0
$ADB shell "hal-audio -s 0"
$ADB shell "amixer -c 1 cget numid=54 | grep -o 'values=[0-9,]*' | tail -1"    # 确认 values=0,0

# 2) 进游戏，起播后立刻查（在 PenCG 里进一局云游戏）
$ADB shell "amixer -c 1 cget numid=54 | grep -o 'values=[0-9,]*' | tail -1"
#    期望：仍然 values=0,0     ← 修复前这里会变成 200,200
$ADB shell "amixer -c 0 cget name='Playback Path' | grep -o 'values=[0-9,]*' | tail -1"
#    期望：values=2 (SPK)

# 3) 用麦克风旁录确认真的没声（可选，但要"应静音"的硬证据就做）
$ADB shell "amixer -c 0 cset name='Capture MIC Path' 'Main Mic'"
$ADB shell "arecord -q -D hw:0,0 -t raw -f S16_LE -r 48000 -c 2 -d 3 /data/local/tmp/t.raw"
$ADB shell "od -An -v -td2 /data/local/tmp/t.raw | awk '{for(i=1;i<=NF;i++){v=\$i<0?-\$i:\$i; s+=v; n++; if(v>m)m=v}} END{print \"avg=\" s/n, \"max=\" m}'"
$ADB shell "amixer -c 0 cset name='Capture MIC Path' 'MIC OFF'"
#    判读：avg 在 100 上下、max < 1500  => 静音（对照矩阵第 2/4 行）
#          avg 9000+、max 32768         => 还在响（对照矩阵第 3/5 行）
```

**通过标准**：游戏在播（`Playback Path=SPK`）、系统音量仍是 `0,0`、旁录 avg ≈ 100。
日志里应有一条 `#<id> audio snapshot: Playback Path=OFF amp(card1#54)=0,0`，
且**不应出现** `audio amp volume 0 -> 200`（这行已经被删除，出现即说明装的是旧包）。

### 步骤 2 — 系统音量 50，进游戏有声，退出后 `pidof aplay` 为空（Bug5 现象 2）

```bash
$ADB shell "hal-audio -s 50"
$ADB shell "amixer -c 1 cget numid=54 | grep -o 'values=[0-9,]*' | tail -1"    # 期望 values=176,176

# 进游戏有声（主观确认），然后退出游戏
$ADB shell pidof aplay                                                         # 期望：空
$ADB shell 'cat /proc/asound/card1/pcm0p/sub0/status'                          # 期望：closed
$ADB shell "amixer -c 1 cget numid=54 | grep -o 'values=[0-9,]*' | tail -1"    # 期望：仍 176,176（我们没动）
$ADB shell "amixer -c 0 cget name='Playback Path' | grep -o 'values=[0-9,]*' | tail -1"
#    期望：回到 values=0 (OFF)  ← 修复前会一直留在 2 (SPK)
```

日志应能看到完整的收尾链（`/userdata/applog/loli_s6_dev.txt`）。
**注意通道**：这些行来自 native `cm_log()`，前缀是 `[cgmedia]` 而不是 `[pencg]`
（见 §4 步骤 -1），所以两条通道一起抓：

```bash
$ADB shell "grep -a -h -e '\[cgmedia\]' -e '\[pencg\]' /userdata/applog/loli_s6_dev.txt | grep -E 'audio (snapshot|restore)|aplay pid=' | tail -20"
```

期望（顺序可能出现 EOF 或 SIGTERM 之一；`[cgmedia]` 前缀以真机为准，**待现场确认**）：

```
[cgmedia] #3 audio snapshot: Playback Path=OFF amp(card1#54)=176,176
[cgmedia] Playback Path OFF -> SPK (now=SPK)
[cgmedia] aplay pid=2412 exited on EOF after 10 ms          ← 或 exited on SIGTERM after 210 ms
[cgmedia] #3 audio restore: Playback Path SPK -> OFF (back to OFF)
[cgmedia] #3 audio restore: amp(card1#54) 176,176 unchanged (we never write it)
```

若一条 `[cgmedia]` 都没有：先回到 §4 步骤 -1 确认 native stdout 的落盘去向，
**不要直接判定「修复没生效」**。

**不期望**看到（出现即说明 aplay 卡死，是新的现场证据）：

```
[cgmedia] aplay pid=xxxx UNKILLABLE after 1000 ms (stuck in driver?)
```

### 步骤 3 — 退出后读数与 baseline 一致

```bash
$ADB shell "amixer -c 0 contents" > /tmp/after_c0.txt
$ADB shell "amixer -c 1 contents" > /tmp/after_c1.txt
# 与 docs/audio-baseline.txt 第 2/3 节逐项对照，关键三项：
#   card0 numid=45 Playback Path : values=0   (OFF)
#   card0 numid=51 Master        : values=225,225
#   card1 numid=54 Master        : 与进游戏前一致（用户音量，不因我们而变）
$ADB shell pidof aplay                    # 空
$ADB shell 'cat /proc/asound/card0/pcm0p/sub0/status'   # closed
$ADB shell 'cat /proc/asound/card1/pcm0p/sub0/status'   # closed
```

### 步骤 4 — 有道查词正常发声且有声音（不再异常）

```bash
# 退出 PenCG，回到桌面
$ADB shell "amixer -c 1 cget numid=54 | grep -o 'values=[0-9,]*' | tail -1"
#    A) 系统音量 0 的情况：期望 values=0,0  → 有道查词应当**没声**（用户意图）
#    B) 系统音量 50 的情况：期望 values=176,176 → 有道查词应当正常出声
#    ★ 修复前 A 情况下会是 200,200 —— 用户会听到"莫名其妙很大声的查词音"

# 用有道查词扫一个词，确认音频行为符合当前系统音量
$ADB shell "ps | grep -i SoundPlayer"      # 确认 SoundPlayer 在跑（它是查词的发音进程）
$ADB shell "grep -h '\[pencg\]' /userdata/applog/loli_s6_dev.txt | tail -5"
#    期望：PenCG 的进程已经退出，日志里没有新的 pencg 行（说明我们没有残留进程在写声卡）
```

### 步骤 5 — `setAudio {on:0}` 路径单独验证（还原兜底）

如果在游戏内通过网关下发 `setAudio {on:0}` 关音频：

```bash
# 关音频后立刻查（此时视频还在，只有音频盒被拆）
$ADB shell "amixer -c 0 cget name='Playback Path' | grep -o 'values=[0-9,]*' | tail -1"
#    期望：values=0 (OFF) —— 已经还原
$ADB shell pidof aplay     # 期望空
# 日志：audio restore 只出现一次（幂等），不会因为 cm_cmd 的兜底再还原一遍
```

### 步骤 6 — 看门狗超时路径（较慢，可选）

拔网让 RTP 静默 180 秒（`WATCHDOG_MS`），确认超时退出后 mixer 同样被还原：

```bash
$ADB shell "amixer -c 0 cget name='Playback Path' | grep -o 'values=[0-9,]*' | tail -1"   # 期望 values=0
$ADB shell pidof aplay                                                                     # 期望空
```

---

## 5. 回归风险 / 冲突标注

### 5.1 ⚠️ 与视频修复的合并冲突（**必读**）

本次**没有改动** `cm_gst.c` 里的视频管线字符串：

- `KMS_SINK_PROPS`（`cm_gst.c:180`）
- `video_pipeline_fmt()`（`cm_gst.c:184-197`）
- `video_pipeline_soft()`（`cm_gst.c:199-207`）

另一位 teammate 的视频修复需要改这两处。**合并时注意**：
`cm_box_create()` 里视频分支（`cm_gst.c:1013-1028`）与我改的音频分支（`cm_gst.c:1029-1048`）
是 `if/else` 的两半，只要不动对方那一半就不会冲突；
但 `cm_box_destroy()` 末尾我加的 `if (!b->is_video) cm_audio_route_restore(b->sess);`
（`cm_gst.c:1117`）在函数尾部，如果视频修复也要在尾部加清理，
**注意保持 `aplay_stop(b)` → 音频还原 → `free()` 的顺序**。

另外 `cm_box` 结构体我在 `char audio_dev[128];` 之后加了 `cm_sess *sess;`（`cm_gst.c:151`），
如果视频修复要往同一个结构体加字段，加在 `sess` 之后即可，两者无耦合。

### 5.2 与 `cm_rtp.c` / `cm_media.c` 的关系

**本次完全没有改这两个文件**（另一位 teammate 在改）。
音频快照/还原全部落在 `cm_gst.c` + `cm_session.h` + `cm_cmd.c`，
通过 `cm_box_destroy` 这个既有出口触发，所以 `cm_media.c` 的看门狗退出路径
**自动**获得还原能力，不需要它改任何一行。

### 5.3 `cm_session.h` 的并发改动

另一位 teammate 已在 `cm_stats` 里加了 8 个 `vid_*` 字段（视频完整性统计）。
我加的 5 个 `audio_*` 字段在 `struct cm_sess` 的 `gst_mu` 之后，**与 `cm_stats` 不在同一结构体**，
无冲突。已跑完整 `native/build.ps1` 确认两个 .so 都能产出。

### 5.4 行为变化（用户可感知）

| 行为 | 修复前 | 修复后 |
|---|---|---|
| 系统音量 0 进游戏 | **有声**（功放被抬到 200） | **静音** ✅ |
| 系统音量 50 进游戏 | 有声（但功放被抬到 200，比用户设定更响） | 有声，**响度等于用户设定** ✅ |
| 退出游戏后 `Playback Path` | 永久留在 SPK | 还原成进游戏前的值 ✅ |
| 退出游戏后系统音量 | 永久留在 200,200 | **从未被我们改过** ✅ |
| 退出游戏后 `pidof aplay` | 靠 SIGKILL 硬杀，无自证 | 正常 EOF 退出，日志自证 ✅ |

### 5.5 收尾耗时变化（可接受，需知情）

`aplay_stop()` 从「发两个信号立即返回」变成「轮询等它真的死」，
最坏情况累计约 **200 + 300 + 500 = 1000 ms**，且只在 aplay 不响应时才走满。
正常情况（aplay 收到 EOF 就退）实测轮询第一轮（10 ms）就返回。

`cm_box_destroy()` 本身已经含 `gst_element_get_state(..., 1.5s)` 的同步等待，
所以拆管线路径本来就是「可能秒级」的；这 1 s 只在极端卡死时叠加，
且此时留着它比忽略它更危险（声卡被独占）。**不认为需要优化。**

### 5.6 未处理（有意不扩大范围）

1. **蓝牙分支 XRUN**（`cg_aplay.log` 里 `underrun!!!` / `state : XRUN`）。
   这是 `-B 150000 -F 30000` + `fdsink sync=false` 的缓冲/时钟问题，
   与本次静音 bug 无关，共用 `aplay_stop()` 收尾逻辑但不需要改它。
2. **`hal-audio -g` 坏接口**。这是系统侧的问题，我们不用它就不受影响。
   已在 `audio-baseline.txt` 第 4 节记录，供后续排查参考。

---

## 6. 编译验证

```bash
$ cd PenCG && powershell -NoProfile -ExecutionPolicy Bypass -File native/build-media.ps1
OK -> PenCG/libs/libjsapi_cgmedia.so (78.8 KB)

$ cd PenCG && powershell -NoProfile -ExecutionPolicy Bypass -File native/build.ps1
OK -> PenCG/libs/libjsapi_cgnative.so (60.7 KB)
OK -> PenCG/libs/libjsapi_cgmedia.so (78.9 KB)
```

两个 .so 都产出。编译只有一个**既有**告警（不是本次引入）：

```
cm_gst.c:517:12: warning: address of function 'gst_debug_message_get' will always evaluate to 'true'
```

（这是手写函数声明做存在性检查导致的老告警，改动前就有。）

---

## 7. 一句话总结

> 旧代码把**用户音量**（card1 `numid=54`）当成「功放没开」的信号，
> 读到 `0,0` 就抬到 `200,200`；而我们改完后从不还原。
> 于是：音量 0 进游戏必响（A），退出后整机音量钉在 200（B）。
> 修复 = **永不写用户音量 + 改过的全局路由按快照幂等还原 + aplay 收尾可自证**。
