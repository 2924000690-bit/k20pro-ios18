# 双系统现场状态（2026-11-19 02:0x）

一键包已装好并在真机验证。以下都是实测值，不是计划值。

## 分区（GPT，113.09 GiB，LUN0）

| 分区 | 大小 | 内容 / sha256 |
| --- | --- | --- |
| `boot` | 128 MiB | 安卓 boot（`/vol1/1000/文件/TWRP_boot_45c2b4ec_20261030/boot.emmc.win`），sha256 `6bf2b6c5…f4232c` |
| `recovery` | 64 MiB | iOS 启动入口 `ios-recovery.img`（xnuloader v3 + 内嵌 Linux 看护内核），sha256 `15082641…4d66` |
| `darwin` | 48 GiB | iOS16 System（0…2.78 GB）+ 启动资源（3 GiB 起，157,548,544 B，含本机 modemst1/2、fsg、fsc）+ iOS 数据卷（块 2042368 起，40.21 GiB） |
| `userdata` | 63.15 GiB | f2fs（一键包按 vendor fstab 重建），Android 侧当前数据 |

- Android：GSI Android 17（`CP2A.260605.016`）+ vendor MIUI `V12.5.6.0.RFKCNXM`，内核 `4.14.357-MiuiCX-HyperOS`
- 启动方式：正常开机 → Android；关机后电源+音量上 → iOS16；TWRP 用 `fastboot boot images/twrp-probe.img`
- 安装器备份：`双系统一键包-蜂窝/…/session-private/45c2b4ec/20261118-170629-151953/`（boot/recovery/dtbo/vbmeta/misc/devinfo/persist/metadata + state.json）

## 今晚做过的事

1. Linux 版一键包（`/tmp/oneclick-linux.py`，adapter 只把 adb/fastboot 换成 `/usr/bin`，逻辑未改）跑完首次安装：GPT、格式化 userdata、写 System、写资源、初始化数据卷、写 recovery，全部读回校验通过。
2. 安装前发现 `boot` 分区里是 TWRP（不是 Android boot），所以“正常开机进 Android”本来是坏的就；已按用户提供的 TWRP 备份刷回 Android boot，验证可正常开机、adb、Wi-Fi。
3. 没有格式化 Android 用户数据（用户明确不同意），userdata 保持安装器重建后的 f2fs。

## WPA3（Android 侧实测，作 iOS 对照）

- `kira 5G`：`KeyMgmt: SAE`、`RequirePmf: false`、5745 MHz、11ac 866 Mbps，IP 192.168.1.112
- 驱动上报 `WIFI_FEATURE_WPA3_SAE`、`WIFI_FEATURE_SAE_H2E`
- 连接序列 `ASSOCIATING → ASSOCIATED → FOUR_WAY_HANDSHAKE → GROUP_HANDSHAKE → COMPLETED`，约 249 ms
- 包里的 iOS 内核资源里没有任何 `sae` 路径 → 它只能 WPA2；WPA3 必须回到本仓库的 iOS18 内核（`wifi_sae=1`）

## iOS18 进 darwin 的硬限制

- darwin 布局由 `ios-recovery.img` 里的 loader 决定：System 在 0，启动资源在 3 GiB（`0xc0000000`），数据卷从块 2042368（8.36 GiB）起
- 本仓库 iOS18 HFS 根卷：20 GiB 声明的卷、实用约 16 GiB（`ios18-root/*/root.img`，HFSX）
- 结论：不解决 3 GiB 资源偏移，iOS18 根卷放不进 darwin；三条可行路线
  1. 改/换 loader，把资源偏移挪到 ≥16 GiB（需反汇编 `ios-recovery.img` 的看护内核，或找作者新版 loader）
  2. 从 userdata 再切一个 `iosroot`（约 20 GiB）给 iOS18；`--wipe` 要格式化 userdata（用户不同意），原地缩 f2fs 需要 Android root（当前没有）
  3. 把 iOS18 根卷裁到 3 GiB 以内（当前 16 GiB，不现实）

## 恢复点

- `boot` 想回 TWRP：`fastboot flash boot images/twrp-original.img`（sha256 `4161f9f5…84b8bf`）
- recovery 想回 TWRP：安装器选项 5，或 `fastboot flash recovery` 用备份
- iOS16 数据想重来：`session-private` 里只有 GPT/recovery 备份，iOS 数据需从包内镜像重装（选项 7 重装 System，保留数据卷）

## 白苹果开机图（splash）

- UEFI 里 `EnableSecurityHoleForSplashPartition = 0x1`，XBL 会从 `splash` 分区读开机图
- 官方 `logo.img`（V12.5.6 线刷包）格式：0x4000 处 `LOGO!!!!` 头 + 4 帧 BMP（3×24bpp + 1×32bpp，1080×2340）
- 已生成 `wifi-test/logo-apple.img`：同尺寸同格式，黑底白苹果（苹果图取自 iOS18 根卷 `ProgressUI.framework/apple-logo@2x~iphone.png`，CgBI 解码）
- 预览：`wifi-test/splash-preview.png`
- 刷写：`fastboot flash splash logo-apple.img`；还原用官方包里的 `logo.img`

## 实机观察：iOS18 从 darwin 启动（2026-11-19 22:5x）

- 用户确认：刷入 darwin 后 **GPU 恢复、流畅度大增**（`sweetgpu=8` 生效，日志 `SWEET GPU: pm: on level 1`）
- **黑块复现**：与 `docs/FORK-SCAN-AND-BUGS.md`、`docs/HANDOVER-GPU-SCANOUT.md` 记录的现象一致（compositor scanout 早于 GPU fence / 直出路径）
- 处理顺序：先把 recovery 持久化 + Wi-Fi 跑通，再按 HANDOVER-GPU-SCANOUT 的 scanout 顺序修黑块

## 事故记录：recovery 启动 mount(3) panic（2026-11-19 23:3x）

- 已把我们的 51.35 MiB 镜像刷进 `recovery`（AVB 无签名）并刷了白苹果 `splash`
- `adb reboot recovery` 后：UNIXNU 0.1.0 正常起来，SWEET EARLY 1–20 全过，随后
  `SWEET EARLY PANIC: mount(3) exited ... (signal 0, exit status 1)` —— 根卷挂载失败
- 同一镜像用 `fastboot boot` 当时能正常挂 `darwin` 并进 iOS18；所以先怀疑根卷在
  中断的 106 MiB 固件推送（ssh broken pipe）后处于脏/损坏状态，而不是 recovery 模式本身
- 已生成 AVB 封装版 `wifi-test/boot-ios18-darwin-recovery.img`（64 MiB，AVBf 尾），待根卷干净后再验证 recovery 启动
- 待办：重刷 `darwin` → RAM 启动确认 → 用单文件 tar + sha256 校验的方式重推 Wi-Fi 固件 → 再验证 recovery 持久入口
