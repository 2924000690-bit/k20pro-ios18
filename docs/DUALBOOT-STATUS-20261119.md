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

## Wi-Fi on the installed iOS18 (2026-11-20 清晨)

- 固件已放进根卷 `/usr/local/share/firmware/wifi`（modem.mdt/b00..b31、wlanmdsp.mbn、firmware-5.bin、
  bd_f11cn.bin、0-rmtfs-ram、四项 NV），`kern.sweet_wifi_step = 255`
- `wifi_lx=2 wifi_scan=1` 实测：`THE SCAN HEARD 14 NETWORKS`（12×2.4G + 2×5G），射频/驱动/站点正常
- `wifid` 的 LaunchDaemon 有 `_LimitLoadToDeviceTree = ["wlan","marconi-wifi"]`；ADT 里追加一个 `/wlan`
  节点（字节级追加到根节点末尾，保留其余字节）后，launchd 开机自动加载 wifid，无需手动 bootstrap
  - 注意：用 mkadt.py 重新生成 ADT 会因 random-seed 之外的结构差异触发 DarwinBoot `EFI_COMPROMISED_DATA`；
    可用「解析原 ADT → 在末尾追加节点 → 更新父节点 children 计数」的方式保持其它字节不变
  - 节点放在 `/arm-io` 下与放在根下，wifid 都能启动，但 Settings 的 Wi-Fi 面板仍为灰色
- wifid 与驱动已通信：`net: en0 is asked 0xc02c6938 by wifid`、`apple80211: the station says: power, 1`
- 剩余卡点：iOS 18 的 Settings/MobileWiFi 能力判断（不是 SIOCSA80211 这条 ioctl 链）；下一步查
  CoreWiFi/MobileWiFi 期望的 IO80211Interface / 能力位（系统框架在 dyld shared cache，不在 .framework 目录）
- 亮度：熄屏再亮时 `blSet(true)` 恢复的是面板上次 WLED 字节（`fBlRaw`），不是 iOS 亮度等级 → 需在
  `SweetFramebuffer.cpp` 记录 iOS 设置的等级并在唤醒时重放
- 开机图：`logo` 与 `splash` 分区是 Android 与 iOS 共用的 ABL 开机图（已恢复官方 `logo.img`）；
  UniXNU 阶段的绿色是它自带的菜单 UI，白苹果需要在 UEFI/DarwinBoot 或内核早期 FB 里画

## 结论：Settings 灰的最后一环是 IO80211Interface（2026-11-20 01:2x）

- 手动跑 `/usr/sbin/wifid` 抓到它自己的日志：`PPM Error: No matching driver found`
- wifid 二进制里引用的 IOKit 类名：`IO80211Interface`、`IO80211APIUserClient`、`AppleWiFi`、
  `IO80211InterfaceRole/ProductType/RadioMODE/...`
- 说明：wifi 的 ioctl 链（SIOCSA80211/SIOCGA80211、SIOCGIFMEDIA=802.11）已经能用——wifid 会问
  `0xc02c6938`（SIOCGIFMEDIA）并得到 802.11 media；但 wifid 还要在 IOKit 里 match 到一个
  `IO80211Interface` 实例才认为“有 Wi-Fi 驱动”，否则 Settings/MobileWiFi 判为无 Wi-Fi 能力
- 下一步（未做）：在内核里发布一个最小的 `IO80211Interface` IOService（C++，挂到现有 IOKit 树，
  带 IO80211InterfaceRole/ProductType 等属性），先验证 wifid 是否不再报 No matching driver、Settings
  是否恢复；如果它还要 user client，再补 IO80211APIUserClient 的方法表

## IO80211Interface stub: published, but wifid still says "No matching driver"（2026-11-20 02:0x）

- 内核加入 `raphael_io80211_stub.cpp`（`wifi_iokit=1` 才启用），真机日志出现
  `HAKIOS IO80211: IO80211Interface published (wifi_iokit=1)`
- 但手动跑 `/usr/sbin/wifid` 仍然打印 `PPM Error:No matching driver found`，
  `com.apple.wifi.manager` 在 launchd 里仍是 `U`（未注册），Settings 面板依旧灰
- 说明：仅发布一个类名为 `IO80211Interface` 的 IOService 不够；wifid 匹配的“driver”要么带属性过滤
  （IO80211InterfaceRole 等），要么是另一个类（AppleWiFi/IO80211Controller），要么要求 IOServiceOpen
  的 user client。下一步需要从 wifid/IO80211.framework 里确认它的 matching dictionary，
  再决定 stub 挂哪个类、暴露什么属性/用户客户端
- 构建：run 44579835862 成功；镜像 `wifi-test/boot-ios18-iokit.img`

## 更正：`PPM Error: No matching driver` 是 ApplePPM 的，不是 Wi-Fi（2026-11-20 02:2x）

- 在 dyld shared cache 里定位到该字符串来自 `/usr/lib/libPPM.dylib`（ApplePPM，基带/Wi‑Fi 发射功率与
  thermal admission 的客户端），它要 match 的是 PPM 的 IOKit 用户客户端，和 Wi‑Fi 是否可用无关
- 所以 `IO80211Interface`/`AppleWiFi`/`IO80211Controller` 三个 stub 都已发布（真机日志确认
  `HAKIOS IO80211: IO80211Interface/AppleWiFi/IO80211Controller published (wifi_iokit=1)`），
  wifid 仍打印那行 PPM 错误是**预期**的，不能拿它当 Wi‑Fi 判断依据
- 现在仍要回答的问题是：Settings 的 Wi‑Fi 面板为什么还是灰的。已知：
  - wifid 在跑（`launchctl list` 有），会向驱动发 SIOCGA80211（power）和 SIOCGIFMEDIA（802.11 media）
  - `com.apple.wifi.manager` 在 launchd 里是 `U`（未注册），`com.apple.private.corewifi.internal-xpc` 是 `M`
  - 下一步：确定 Settings/MobileWiFi 的 Wi‑Fi 能力判断来自哪里（MobileGestalt 的 wifi 能力键、还是
    CoreWiFi 对 wifid 的 XPC 查询结果），再决定补哪一层

## 为什么 iOS18 待机发烫（2026-11-20 07:4x 实测）

- 内核/GPU 不是热源：GPU 日志显示 99% 时间在最低档（level 1, 257 MHz），busy 0%
- 真正原因在用户态：
  - `xpcproxy`/`fileproviderd`/`bluetoothd`/`wifip2pd` 等守护进程反复崩溃重启（一次会话里 xpcproxy 5,646 次、
    fileproviderd 1,460、wifip2pd 1,457、bluetoothd 1,451）
  - `audiomxd` 空转，把 `launchd` 拉到 ~88-90% CPU（还有 21% 自己），整机因此持续发热
- 处理（`launchctl disable`，写在根卷 `/var/db/com.apple.xpc.launchd/disabled.plist`，重启后仍生效）：
  `com.apple.audiomxd`、`com.apple.corespeechd`、`com.apple.siriactionsd`、
  `com.apple.speechmodeltrainingd`、`com.apple.bluetoothd`、`com.apple.bluetoothuserd`、
  `com.apple.BluetoothUIService`、`com.apple.siri.acousticsignature`、`com.apple.wifip2pd`、
  `com.apple.FileProvider`、`com.apple.spaceattributiond`、`com.apple.WebBookmarks.webbookmarksd`、
  `com.apple.MTLAssetUpgraderD`、`com.apple.modelmanagerd`、`com.apple.mobile.NRDUpdated`
- 效果：exec 速率 ~34/30s → ~12/60s；launchd 从 88% 降到 ~1%；整机空闲 CPU 只有几个百分点
- Wi‑Fi 不受影响：`wifid` 在跑、`en0` 仍是 802.11、射频保持 up
- 这些服务本来就是崩溃状态（音频没有驱动、蓝牙没有传输、P2P 无对端），关掉不损失现有可用功能；
  以后要恢复某个服务：`launchctl enable system/<label>`（或 `user/501/<label>`）后重启即可

## 找到 wifid 真正 match 的类：IO80211SkywalkInterface（2026-11-20 08:0x，离线分析）

- 用 7z 从 `ios18-root-quiet/root.img` 解出 dyld shared cache（68 个文件，4.0 GB），写了缓存解析脚本按
  `imagesText` 表定位镜像，抽出三个框架的 Mach-O 段：
  - `CoreWiFi`（1,558,752 B __TEXT）、`MobileWiFi`（231,296）、`IO80211`（363,200）
- `MobileWiFi` 里有 `com.apple.wifi.manager.available`、`__WiFiManagerClientAvailableCallback`：
  Settings 的 Wi‑Fi 面板依赖 manager 的 availability 通知
- `wifid` 的字符串里有一条关键日志：
  `IOServiceAddMatchingNotification IO80211SkywalkInterface failed`
  —— iOS 18 的 Wi‑Fi 栈要 match 的是 **IO80211SkywalkInterface**，不是我们之前发布的老类
  `IO80211Interface/AppleWiFi/IO80211Controller`（那三个是 iOS 16 及更早的路径，PPM 报错则是 ApplePPM 的，已排除）
- 已在内核 stub 里补上 `IO80211SkywalkInterface`（带 `IOInterfaceName=en0`、Role/ProductType/RadioMODE 属性），
  仍由 `wifi_iokit=1` 控制
- 等手机回来验证：看 wifid 是否不再报该 match 失败、`com.apple.wifi.manager` 是否从 `U` 变 `M`、Settings 是否恢复

## 两个构建已就绪（2026-11-20 08:1x）

- 内核（Skywalk stub）：run 44642394617 成功
  - 镜像：`wifi-test/boot-ios18-skywalk.img`（`052c67c4…`，53,846,016 B）
  - 内含 `IO80211SkywalkInterface` + `IO80211InterfaceRole=Infrastructure` + `AppleWLANDriver` + `AppleSunriseWLAN`
  - 用法：`fastboot boot boot-ios18-skywalk.img`，或刷进 `recovery`
- 固件（白苹果）：run 44642855675 成功
  - 产物：`wifi-test/fw-logo/boot-raphael-full-RELEASE.img/boot-raphael.img`（70,627,328 B，含 UniXNU + 带 Apple logo 的 DarwinBoot）
  - 这块只在 iOS 入口画 Apple，Android 入口仍显示官方小米 logo
- 手机回来后验证顺序：
  1. `fastboot boot boot-ios18-skywalk.img` → 看 wifid 是否仍报 Skywalk match 失败、`com.apple.wifi.manager` 是否变 `M`
  2. 刷 `recovery` 成 skywalk 镜像（或先用 RAM boot）
  3. `fastboot boot boot-raphael.img`（或刷 boot）验证白苹果
