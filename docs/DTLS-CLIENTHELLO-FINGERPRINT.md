# 设备端 DTLS ClientHello 指纹（2026-11-14 05:55 原神局，gw-lt-hbtj-05）

完整 hex（203 字节 = DTLS HS 头 12B + body 191B，单分片）：

```
010000bf00000000000000bffefdc667a214b2605c7acac3917f9aa91ae3c7787d0faa4db18ff1058f4c91faa9aa00000038c02cc030009fcca9cca8ccaac02bc02f009ec024c028006bc023c0270067c00ac0140039c009c0130033009d009c003d003c0035002f00ff0100005d000b000403000102000a000c000a001d0017001e00190018000e000500020001000016000000170000000d0030002e040305030603080708080809080a080b080408050806040105010601030302030301020103020202040205020602
```

## 解析结果

| 字段 | 值 |
|---|---|
| hs type / len | 1 (ClientHello) / 191，msg_seq=0，frag_off=0，frag_len=191（不分片） |
| legacy_version | `fefd`（DTLS 1.2 老写法，无 supported_versions 扩展） |
| random | `c667a214…f9aa9aa`（32B，正常熵） |
| session_id | 空 |
| cookie | 空（初始 ClientHello，正常） |
| cipher_suites (28 个) | `c02c c030 009f cca9 cca8 ccaa c02b c02f 009e c024 c028 006b c023 c027 0067 c00a c014 0039 c009 c013 0033 009d 009c 003d 003c 0035 002f 00ff` |
| extensions (93B) | `0x000b ec_point_formats` / `0x000a supported_groups(x25519 secp256r1 secp384r1 secp521r1 secp256k1)` / `0x000e use_srtp(SRTP_AES128_CM_SHA1_80, mki=空)` / `0x0016 encrypt_then_mac` / `0x0017 extended_master_secret` / `0x000d signature_algorithms(15 个)` |

## 与 Chrome 的指纹差异（风控假说的证据面）

1. **28 个密码套件，OpenSSL 默认序**，含静态 RSA / 非 PFS 套件（`c00a c014 0039 0033 0035 002f 00ff`）。Chrome 的 ClientHello 只发 ~15 个现代套件且顺序不同。
2. **无 ALPN (0x0010)** —— Chrome 对 wss 一律带 ALPN。
3. **无 supported_versions / key_share / psk_key_exchange_modes**（纯 TLS1.2 栈）。
4. `use_srtp` 只有单一 profile `SRTP_AES128_CM_SHA1_80`；Chrome 是 `SRTP_AEAD_AES_128_GCM:SRTP_AES128_CM_SHA1_80`（GCM 在前，双 profile）。
5. legacy_version 用 `fefd`（DTLS1.2 代码值）；Chrome 在 DTLS 场景走 `feff/fefd` + supported_versions 组合，数值不同。

**结论**：我们与 Chrome 在 cipher 顺序、suite 集合、extension 集合（缺 ALPN）、use_srtp profile 列表四层全部不同。若网关按 JA3/JA3S 类指纹风控（只放行官方 SDK/Chrome 指纹，其它限速或 250s 掐断），入口证据成立。

## A/B 实验结果（2026-11-14 06:08 对照局）：指纹假说 ❌ 被否定

- **基准局**（05:55，默认 28 套件 OpenSSL 序，原神 `yssy`，gw-lt-hbtj-05）：sess=250s 停推 → 8s 检出 → L2 → 5s 恢复。
- **对照局**（06:08，`DEV_CHROME_FP=true`，14 套件 Chrome 序，ClientHello 从 203B 缩到 **177B**，日志 `chrome_fp cipher list applied (14 suites, Chrome order)`，终末地 `mrfzzmd`）：**同样在 sess≈250s 停推**（06:13:10 detected rx=26133 → L2 → 06:13:18 recovered 停滞 5s）。
- **结论：cipher 序/集合改变不改变停推时刻 ⇒ 网关的 250s 掐断不基于 DTLS ClientHello 指纹**（至少不基于 cipher 这一层）。extension 层（ALPN 等）仍未验证，但优先级下降。
- 结合第二会话同样 250s（05:55 局的 L2 新会话也在 ~248s 被掐）：**停推是「按媒体会话的固定时长定时踢」，与指纹无关、与 会话序号 无关** —— 更像服务端对非官方 SDK 会话的统一 TTL，或对所有人统一的会话时长上限（网页端 481s 未冻结那一次仍待解释）。
- 下一步候选（按性价比）：①用官方 web 包同参数起一局确认网页端现在是否也会 250s 被掐（若也会 → 是平台策略变化，不是我们的问题）；②抓 op=close 的 code/reason 分布（已有日志埋点，被动收集）。

## 对应改动

`cm_dtls.c` 的 `msg_cb()`：handshake type=1 时把第一分片前 512B dump 成 hex（`CM_LOGI("#%d DTLS ClientHello first-frag ...")`），每 DTLS 上下文只 dump 一次。取数命令：

```bash
L=$(ls -t /userdata/applog/DictPen_*.log | head -1)
grep -a "DTLS ClientHello first-frag" $L
```

若要改指纹以伪装 Chrome：`SSL_CTX_set_ciphersuites`/`SSL_CTX_set_cipher_list` 控 suite 序、`SSL_CTX_set_alpn_select_cb` 加 ALPN、`SSL_CTX_set_tlsext_use_srtp` 换双 profile —— 但**先验证网关真的按指纹风控**（可先用 `SSL_CTX_set_cipher_list` 把 suite 顺序调成 Chrome 序做 A/B，看 250s 停推是否消失）。
