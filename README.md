# F32 高级后台工具包

给中兴 MF791D（F32）随身 WiFi 补上一套实用能力的增强包：**短信自动转发到钉钉、流量配额到量断网、空闲烧流量、设备端测速、手机端远程运维**，并把原厂那套老旧的 Web 后台重做成液态玻璃风格。

一次性通过 ADB 刷入，不改固件、不刷机、可回滚。

## 适配设备

| 项目 | 说明 |
|---|---|
| 硬件 | 中兴 MF791D（F32）；同平台 F30Bpro 理论可用，未实测 |
| 软件版本 | SZXK-Panguin_P049U-20250601 |
| 系统 | ARM Linux + busybox，rootfs 为只读 squashfs |
| 前提 | 已开启 ADB 调试，电脑上 `adb devices` 能识别到设备 |

> 平台为中兴微 SZXK-Panguin，不是高通方案。任何针对高通 410/EC20/EC25 的固件、工具、刷机包都不适用，强刷会变砖。

## 功能

| 模块 | 说明 |
|---|---|
| 短信转发 | 设备收到短信后自动推到钉钉群机器人，支持加签、关键词过滤、转发后删卡上短信 |
| 流量配额断网 | 统计 WAN 累计用量，达到套餐总额自动断网；首页黄卡直接填总额，已用自动生成 |
| 空闲烧流量 | 网页自动带出 ICCID 前段，手动填后 4 位校验一致才授权；局域网无终端时下载消耗流量，有终端接入立即停 |
| 设备端测速 | 首页按钮触发，设备侧 8 路并发实测下行，返回 Mbps 与 RTT |
| 手机远程运维 | 手机浏览器打开 `/m.html`，即可写文件、执行命令、看服务状态，无需电脑 |
| 高级后台 | `atweb` 监听 9090，可直接跑 AT 指令 |
| 界面改造 | 首页卡片液态玻璃化、橙色流量卡配额输入、设备信息页重排 |

## 目录结构

```
.
├─ 一键刷入双高级后台.bat     一键部署（11 步）
├─ web/                      完整 /etc_ro/web 内容（替换原厂后台）
├─ 短信转发组件/
│   ├─ smsapi                控制服务二进制（8080）
│   ├─ smsapi.c              源码
│   ├─ smsfwd                转发程序
│   ├─ smsfwd_ca.pem         HTTPS 证书
│   └─ smsfwd.conf           空白配置模板
├─ 配额断网组件/
│   ├─ quota                 配额守护二进制
│   ├─ quota2.c              源码
│   ├─ quota.conf            默认配置（总额 0 = 不限量）
│   ├─ fwguard.sh            防火墙接管
│   └─ netfix.sh             开机 DNS 下发
├─ 空闲烧流量/
│   ├─ burnctl              ARM 静态守护
│   ├─ burnctl.c            源码
│   ├─ 一键部署.bat         单独部署组件
│   └─ 说明.md              行为与接口说明
├─ 手机远程运维/
│   ├─ m.html                手机运维页
│   ├─ 一键开启手机运维.bat
│   └─ 说明.md
├─ atweb                     高级后台（9090）
├─ adb.exe / AdbWinApi.dll / AdbWinUsbApi.dll
├─ 短信转发-使用说明.md
└─ README.md
```

## 部署

1. 电脑装好 ADB 驱动，设备打开 USB 调试并连上
2. 双击 `一键刷入双高级后台.bat`，按提示确认
3. 脚本会自动：挂载 rw → 替换 web → 推 atweb/smsapi/smsfwd/quota/burnctl → 写 `/etc/rc` 自启 → 建软链 → 启服务 → 校验文件大小 → `sync` 落盘
4. 刷完到网页「短信 → 转发设置」填钉钉 Webhook（包内已清空，不含任何 token）
5. 到首页黄卡填套餐总额（留 0 表示不限量）
6. 到「空闲烧流量」页，页面自动带出 ICCID 前段，只需填写后 4 位并授权；重启后授权自动清空，需要重填

重启设备后约 60 秒全部服务自动拉起（脚本里 sleep 5/10/15/60 错峰启动）。

## 端口

| 端口 | 服务 | 说明 |
|---|---|---|
| 80 | 原厂 Web | 替换后的后台首页 |
| 8080 | smsapi | 状态查询、测速、配额保存、烧流授权、文件/命令运维接口 |
| 9090 | atweb | AT 指令高级后台 |

## 关键实现

### 测速

原厂 `/api/speedtest` 是去拉百度首页，永远测不出真实带宽。改成设备侧 8 路并发拉大文件、6 秒窗口，返回：

```json
{"ok":true,"dl_mbps":53.24,"rtt_ms":49,"bytes":40664744,"elapsed_ms":6111,"streams":8}
```

前端 `home.js` 请求 `http://设备IP:8080/api/speedtest`，超时 30 秒。

单流只有 5~6 Mbps（TCP 窗口 + 40ms RTT 限制，属正常现象），多流才能压到 50~60 Mbps。

### 两个必踩的源坑

- **USTC WAF**：`User-Agent: Mozilla/5.0` 会被判为攻击并返回 403（19287 字节拒绝页）。设备侧请求头必须用 `curl/8.0`。
- **华为云**：明文 HTTP 被 WAF 直接回 418，不可用。cloudflare 有 CORS 但境外节点只有 3~4 Mbps，不适合做测速源。

### 配额断网

`quota` 读 `/proc/net/dev` 累计 WAN 字节，与 `/mnt/userdata/etc_rw/quota.conf` 里的总额比较，到量后调 `fwguard.sh` 接管防火墙断网。首页数字由 `quota_info.txt` 软链提供，格式 `已用GB|总额GB|百分比|状态`。

### 空闲烧流量

`burnctl` 用 NETLINK_ROUTE 邻居事件判断局域网是否有终端，不轮询；有终端就立即停，终端清空且“前段自动填入 + 后 4 位手填拼成的完整 ICCID == 设备当前 ICCID”时才启动下载。授权只写 `/tmp`，换卡或重启后失效。`smsapi` 提供 `GET/POST /api/burn`，网页端在「空闲烧流量」页自动带出前段、只填后 4 位并校验授权；统计写入 `burn_info.txt`，首页和设备信息页同步显示。

完整行为、接口和编译命令见 `空闲烧流量/说明.md`。

### 短信转发

`smsfwd` 读设备短信库，按 Webhook 推到钉钉，成功后删卡上短信。`smsapi` 提供网页侧配置与「转发记录」查询。设备固件的短信 Web 接口本身是空实现，所以后台只保留「转发记录」与「转发设置」两页，看历史要去钉钉群翻。

### 首页排版

`core.min.css` 给 `.traffic_control_container` 写死 `height:280px`，新增行会溢出。`custom.css` 已覆盖为 `height:auto !important; min-height:280px; padding:0 0 18px 0; overflow:visible`。改完记得 Ctrl+F5，CSS 缓存最顽固。

## 已知限制

- **没有 VoWiFi 能力**：中兴微基带不支持 IMS，高通 QMI 那套方案与此平台无关。
- **收不到电信短信**（CS 域未附着时）：电信 4G 短信只有 SMS over SGs 或 SMS over IMS 两条路，模组 `CS: detached` 时两条都不通，属模组/运营商侧限制，换软件无解。
- **IPv6 黑洞**：br0 会宣告前缀但设备自身无 IPv6 默认路由，客户端拿到 v6 会黑洞。临时修法是停 radvd 并清 br0 v6 地址，重启后恢复，未做永久关闭。
- **空闲烧流量会真实消耗套餐流量**：只有手动 ICCID 与当前卡一致、且局域网终端全部离线时才会启动；不需要时到页面点“停止并清除”或不要授权。
- 设备 rootfs 只读，改 web 必须 `mount -o remount,rw /`，改完 `sync` 再 `remount,ro`。
- 不刷第三方固件：SZXK-Panguin 不在任何通用随身 WiFi 固件覆盖范围内。

## 回滚

刷入前自行备份 `/etc_ro/web` 与原 `/etc/rc`：

```bash
adb shell "cd /etc_ro && tar czf /tmp/web.tgz web" && adb pull /tmp/web.tgz
adb shell cat /etc/rc > rc.orig
```

出问题把备份推回去、`sync` 后重启即可。

## 免责声明

- 本包只针对中兴 MF791D（F32）测试，其他设备刷入后果自负。
- 包内 `web` 目录基于原厂固件界面二次修改，原厂素材版权归中兴所有，仅供个人研究与学习使用，请勿商用。
- `adb.exe` 来自 Android SDK Platform-Tools，遵循其原始许可。
- 刷机有风险，操作前务必备份，因使用本工具造成的任何损失作者不承担责任。
