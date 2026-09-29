# 迈从（MCHOSE）鼠标 HID 协议规格

目标设备：**MCHOSE A7 Pro**，`USB\VID_5253&PID_1021`（2.4G 接收器），固件 `5.4.7.4`。

本文所有结论都标注了来源等级：

- **【实测】** 在本机 A7 Pro 上用本项目 `tools/` 中的工具读/写真实硬件得到的结果
- **【源码】** 从官方 MCHOSE HUB 的公开 CDN 前端包中读出的定义
- **【社区】** 开源社区对**同方案其它机型**的公开记录（仅作交叉印证，不作为唯一依据）

---

## 1. 证据来源与获取路径

| 来源 | 内容 | 获取方式 |
| :--- | :--- | :--- |
| 本机设备 | HID 拓扑、报告描述符能力 | `tools/hid_probe.exe 5253 --desc` 【实测】 |
| 官方软件前端 | 命令表、字段 parser、读写决策 | `https://cdn.mchose.com.cn/customPage/14/assets/main-*.js` 【源码】 |
| 官方软件本地状态 | 电量/固件/连接模式的独立记录 | `%APPDATA%\MCHOSEHUB\files\config\mc_main_store_key.json` 【实测】 |
| 社区 | 同方案 L7 Pro / K7 Ultra 的推送格式 | 见文末参考 |

> 官方软件为 Electron 应用：主进程编译为 V8 字节码（`out/main/index.jsc`），
> 但**渲染层由 CDN 远程下发、是未混淆的明文 JS**，鼠标的全部设备逻辑都在其中。
> `customPage/<N>/` 是按产品线发布的页面包，A7 系列在 **14** 号包。

本仓库不包含上述任何官方前端代码副本。

---

## 2. 设备与通道拓扑【实测】

`VID_5253 / PID_1021` 是 USB 复合设备，共 3 个接口：

| 接口 | 集合 | UsagePage/Usage | 报文长度 | 用途 |
| :--- | :--- | :--- | :--- | :--- |
| `MI_00` | 鼠标 | `0x0001` / `0x0002` | In 8 | 标准鼠标，被 `mouhid` 独占 |
| `MI_01` | COL01 键盘 | `0x0001` / `0x0006` | In 11 / Out 2 | 键盘仿真 |
| `MI_01` | COL02 消费者控制 | `0x000C` / `0x0001` | In 3 | 多媒体键 |
| **`MI_02`** | **COL02（控制通道）** | **`0xFF01` / `0x0001`** | **In/Out/Feature 均 65** | **本协议使用** |
| `MI_02` | COL01 | `0xFF0B` / `0x0104` | In/Out/Feature 均 61 | 另一条厂商通道（本协议未用） |

控制通道的 Report ID 与负载长度：

| Report ID | 负载字节 | 用途 |
| :--- | :--- | :--- |
| `0x11` | 20 | 短命令（信息读、回报率、休眠、休眠、DPI） |
| `0x12` | 64 | 长数据（全量设置读、DPI X/Y 写） |
| `0x13` | 20 | **设备→主机主动推送** |
| `0x14` | 20 | 备用 |

---

## 3. 报文帧格式【源码 + 实测】

### 3.1 请求

```
sendFeatureReport(reportId, payload)      // WebHID 语义
等价 Win32： HidD_SetFeature(h, buf, 1 + payloadLen)
             buf[0]        = reportId
             buf[1 .. n]   = 命令表字节[i] XOR 0xFF
             buf[n+1 ..]   = 0xFF（即命令表的 0x00 补齐位取反）
```

命令表本身形如 `"11 03 00 00 ... 00"`（首个 token 是 Report ID，第二个是命令字节，其余为参数）。

### 3.2 响应

```
receiveFeatureReport(reportId)            // 返回 [reportId, cmd^0xFF, payload^0xFF ...]
等价 Win32： HidD_GetFeature(h, buf, 1 + payloadLen)
             buf[0]  = reportId
             buf[1]  = 命令字节 XOR 0xFF   ← 用于校验「指令混发」
             buf[2..] = 负载，逐字节 XOR 0xFF 后按 little-endian 解析
```

应答**没有 CRC、没有序号、没有长度回显**，唯一校验就是 `buf[1]` 的命令回显。

### 3.3 实测样本

```
请求 11 06 → 发送 11 F9 FF FF ... (21 字节)
响应 11 F9 AC AD EF FF FA FB F8 FB F6 B8 FF B8 FF ...
解码负载 53 52 10 00 05 04 07 04 09 47 00 47 00 ...
         └vid=0x5253 ┘ └pid=0x0010┘ └fw ┘ └mt┘ └电量 0x47=71%┘
```

固件字段 `05 04 07 04` 逐字节对应版本串 `5.4.7.4`，与官方记录完全一致，
这是判定协议还原正确的关键证据。

---

## 4. 读命令【源码定义 + 实测验证】

| 命令表 | RID | 命令 | 解析字段（XOR 解码后，little-endian） |
| :--- | :--- | :--- | :--- |
| `11 03` + 19×`00` | 0x11 | 0x03 | `bond(u8) vid(u16) pid(u16) connect(u8) game(u8)` |
| `11 04` + 19×`00` | 0x11 | 0x04 | `versionLength(u8) version[len]`（ASCII，实测 `"5.4.7.4"`） |
| `11 06` + 19×`00` | 0x11 | 0x06 | `vid(u16) pid(u16) fwVersion(u32) [bit3 连接模式][bit1 连接状态][bit4 保留] batteryLevel(u8) chargeStatus(u8)` |
| `12 67` + 63×`00` | 0x12 | 0x67 | 见下 |
| `11 1b` + 19×`00` | 0x11 | 0x1b | 灯光：`enable id useIndependent brightness lightSetting closeTime speed openRandomColor r g b r2 g2 b2 changeDirect` |
| `12 65 01 00/01` | 0x12 | 0x65 | 宏数据 |
| `12 63 <idx>` | 0x12 | 0x63 | 按键名 |
| `12 68 00/01/02` | 0x12 | 0x68 | 三组板载配置 |

### `12 67` 全量设置布局【实测】

```
偏移  0        profileIndex(u8)
      1        低=gDpiIndex(bit4)  高=gRateIndex(bit4)     ← 活动回报率档位
      2        低=usbDpiIndex(bit4) 高=usbRateIndex(bit4)  ← 0x41 写入字段
      3        reserved
      4..15    dpi0..dpi5  (u16 ×6, little-endian)
      16       dpiSum
      17       sensor
      18       keyDebounce
      19       sleep
      20..     按键映射区（bit4 类型 + bit4 索引 + 3 字节值）×N
```

A7 Pro 实测负载：

```
00 21 01 01 20 03 40 06 80 0C 00 19 90 65 90 65 02 80 08 03 00 ...
│  │  │  │  └──────┬──────┘ └─┬─┘ │  │  │  │
│  │  │  │      dpi0..5      sum sensor db  sleep
│  │  │  └ reserved=1
│  │  └ byte2=0x01 → 低 usbDpiIndex=1，高 usbRateIndex=0
│  └ byte1=0x21 → 低 gDpiIndex=1，高 gRateIndex=2（=1000 Hz）
└ profileIndex=0

dpi0..5 = 0x0320, 0x0640, 0x0C80, 0x1900, 0x6590, 0x6590
        = 800, 1600, 3200, 6400, 26000, 26000
```

---

## 5. 写命令【源码定义 + 实测验证】

写路径：`setCommand(key, obj)` → 按该命令的 parser 把 obj 序列化成负载
→ 逐字节 XOR 0xFF → 不足部分用 0xFF 补齐到 20（RID 0x11）或 64（RID 0x12）
→ `sendFeatureReport`。

| 命令表 | RID | 负载布局 |
| :--- | :--- | :--- |
| `11 02` | 0x11 | `command game` |
| `11 40` | 0x11 | `command usbDpiIndex gDpiIndex reserved dpi0..5(u16×6) sum` |
| `12 40` | 0x12 | 上式 + `diff` + `dpiVal0..5(u16×6)`（X/Y 独立式） |
| `11 41` | 0x11 | `command usbRate freeRate` |
| `11 0A` | 0x11 | `command sleepStatus sleep` |
| `11 43` | 0x11 | `command time`（去抖） |
| `11 42` | 0x11 | `command lod ripple line motionSync rsv rsv2 gameMode rotateOpen rotateVal` |
| `11 58` | 0x11 | `command profileIndex`（切换板载配置） |
| `12 57` | 0x12 | 全量设置整包写入 |
| `12 52` / `12 55` | 0x12 | 按键 / 长宏 |
| `11 2B` / `12 2D` | 0x11 / 0x12 | 灯光 / 独立灯光 |

官方软件的调用点（决定 `command` 取值与实际字段）：

```js
Qr("0x11 0x41", { command: 65, ...fr });   // 回报率，fr = {usbRate:idx, freeRate:0}
                                           //        或 {usbRate:0, freeRate:idx}
Qr("0x11 0x0A", { command: 10, sleepStatus: on?1:0, sleep: minutes });
Qr("0x11 0x43", { command: 67, time: n });
Qr("0x11 0x42", { command: 66, lod: n });
Mr.command = 64; setCommand("0x12 0x40", Mr);   // DPI
```

回报率档位索引 → Hz（官方 `rateMap`，A7 Pro 无线为 6 档）：

| 索引 | 0 | 1 | 2 | 3 | 4 | 5 |
| :--- | :--- | :--- | :--- | :--- | :--- | :--- |
| Hz | 125 | 500 | 1000 | 2000 | 4000 | 8000 |

---

## 6. 设备主动推送 `report 0x13`【实测】

设备会主动上报 20 字节推送，**负载同样逐字节 XOR 0xFF**，Report ID 本身不参与异或。

A7 Pro 实测：

```
原始 : 13 1D FE FE FF B8 FE FF FD D9 BE C8 DF AF 8D 90 FF FF FF FF FF
解码 : E2 01 01 00 47 01 02 02 26 41 37 20 50 72 6F 00 00 00 00 00
       │  │  │  │  │  │  │  │  └───────────┬──────────┘
       │  │  │  │  │  │  │  │        "A7 Pro" (ASCII)
       │  │  │  │  │  │  │  └ 0x26 型号代码
       │  │  │  │  │  └──┴ 未知
       │  │  │  │  └ 0x47 = 71% 电量   ← 与特性读 11 06 结果一致
       │  │  │  └ 充电状态
       │  └──┴ 协议版本 01 01
       └ 子类型 0xE2
```

要点：

- **子类型不要写死。** L7 Pro 实测为 `0x1D`，K7 Ultra 与**本机 A7 Pro 为 `0xE2`**。
  实现上两者都接受（`payload[4] <= 100` 作为合法性兜底）。
- 发送 `11 06` 读设备信息（即 `[0x11, 0xF9] + [0xFF]×19`）会 **nudge** 设备，
  约 2 秒后到达一条 `0xE2` 推送。这是社区所称的 "nudge" 报文的真实身份。
- 设备也会回显主机发出的命令（子类型 `0xE5`），实现时应过滤。

---

## 7. 字段映射实测（本文最关键的一节）

官方 parser 只用 `bit4` 声明字段，**没有写死半字节先后**，因此"哪个半字节是哪个字段"
无法从源码静态确定，必须实测。以下三组实验在真机上完成，全部可逆。

### 实验 A：回报率写入落点

`0x41` 负载为 `[command, byte1, byte2]`。分别写入不同值后回读 `12 67`：

| 写入 `byte1, byte2` | 回读 `byte1` | 回读 `byte2` | 结论 |
| :--- | :--- | :--- | :--- |
| `2, 5` | `0x21`（不变） | `0x**5**1` | 跟随 **byte2** |
| `5, 2` | `0x21`（不变） | `0x**2**1` | 跟随 **byte2** |
| `0, 0` | `0x21`（不变） | `0x**0**1` | 原始值 |

**结论：`0x41` 的 byte2 → 回读 `byte2` 的高半字节；byte1 在 `12 67` 中无可观测影响。**

### 实验 B：DPI 写入落点

`0x40` 负载为 `[command, i1, i2, reserved, dpi0..5, sum]`：

| 写入 | 回读 `byte1` | 回读 `byte2` |
| :--- | :--- | :--- |
| `i1=4, i2=4` | `0x21` → `0x2**4**`（低半字节） | `0x01` → `0x0**4**`（低半字节） |
| 自校验还原 | 精确回到 `0x21` | 精确回到 `0x01` |

**结论：两个 DPI 索引字段落在回读 `byte1`/`byte2` 的**低**半字节。**

### 实验 C：由 A、B 推出的 nibble 顺序

A 与 B 合起来给出唯一自洽解释：**先声明的 `bit4` 字段落在低半字节**。
于是 `12 67` 的映射为：

```
byte1: 低 = gDpiIndex      高 = gRateIndex   （活动回报率）
byte2: 低 = usbDpiIndex    高 = usbRateIndex （0x41 写入字段）
```

校验：原始 `byte1=0x21` → `gDpiIndex=1, gRateIndex=2`；`byte2=0x01` → `usbDpiIndex=1, usbRateIndex=0`。
实验 B 写入 `i2=4`（= `gDpiIndex`）应改 `byte1` 低半字节 ✔；写入 `i1=4`（= `usbDpiIndex`）应改 `byte2` 低半字节 ✔。
实验 A 写入 `byte2` 应改 `byte2` 高半字节（= `usbRateIndex`）✔。三者完全自洽。

### 活动回报率的独立印证

`gRateIndex = 2` → **1000 Hz**。该值同时被设备自己的推送报文印证：
推送 `payload[6] = 2`（社区对同方案机型实测该字段为回报率索引，0↦125 … 5↦8000），
与 `gRateIndex` 一致。两条独立来源同值，故界面显示采用 `gRateIndex`。

### 遗留的语义细节

`0x41` 写入的是 `usbRateIndex`（`byte2` 高半字节），而界面显示的是 `gRateIndex`（`byte1` 高半字节）。
两者在写入后不会立刻同步。官方软件注释提到**设置先写 EEPROM，需要重载命令才同步回 RAM**
（同方案 K7 Ultra 上有 `0x0B 0xAA` 重载命令）。这可以解释该现象。
本项目因此不做"乐观覆盖"，界面只显示实读值，写命令一律做**写后回读校验**，
校验目标是写入字段本身。

---

## 8. 实现必须注意的坑【实测】

1. **特性读会返回滞后缓冲。** 打开设备后的第一次 `12 67` 常返回**解码后全 0** 的响应
   （命令回显却是对的）。必须把"解码后负载全零"视为无效并重试。
   本实现重试 4 次，实测均在第 1~2 次拿到真数据。
   注意：合法性判断要看**解码后**的字节，不能看原始字节——全零负载的原始字节是全 `0xFF`。
   也**不要**为了"兼容设备真的全零"而放宽这条判据：实测放宽后会把滞后缓冲当有效数据，
   显示成"电量 0%、回报率 125 Hz"这类垃圾状态（见第 10 节）。
2. **写命令之后立刻读，会读到写入前的旧设置。** 写校验因此可能假失败。
   正确做法：写后给 150ms 稳定期 → 丢弃一次读数（冲掉滞后缓冲）→ 再读第二次作为判据。
   实测：写 500 Hz 后写入字段档 `0→1`、写 1000 Hz 后 `1→2`，双读方案稳定通过；
   单读方案会读到写前的值并误报失败。
3. **命令回显校验是唯一的防错手段。** `buf[1]` 必须等于 `命令字节 ^ 0xFF`，否则丢弃重试。
   畸形的请求会被设备原样回显（实测把 Report ID 也异或进负载时，响应就是那条畸形命令本身）。
4. **XOR 0xFF 的范围**：只异或负载，**Report ID 不参与**。
5. **写命令补齐用 `0xFF`**（等价于命令表的 `0x00` 补齐位取反），不是补 0。
6. **`HidD_SetFeature`/`HidD_GetFeature` 的长度用 `1 + 该 Report ID 的负载长度`**
   （`0x11` → 21 字节，`0x12` → 65 字节），虽然集合的 `FeatureReportByteLength` 报的是 65。
7. **不要用 `0x41` 去"读"回报率**：它只写不读，写 `[0x41,0,0]` 会把档位设成索引 0。
   回报率必须从 `12 67` 或推送报文读。
8. **写命令都可能改 EEPROM。** 本项目所有写路径都遵循"先读全量 → 只改目标字段 → 整包回写 → 写后回读"，
   并用可逆实验验证过。
9. **不要让输入读长期挂起。** 实测：若把待决的重叠输入读一直留在队列里，
   本设备的命令通道会被堵死，`11 06` / `12 67` 全部拿不到数据。
   正确做法是"每次发起一次读 → 超时就 `CancelIo` **并用 `GetOverlappedResult(bWait=TRUE)` 收割**"。
   注意两者都要做：只取消不收割，迟到的完成会写到已失效的 `OVERLAPPED`（UB），
   并留下陈旧事件信号导致下一次读被误判失败。

---

## 9. 链路状态：接收器在线 ≠ 鼠标已连接【实测】

这是集成时必须处理的真实状态，否则会显示误导信息。

| 命令 | 由谁应答 | 鼠标休眠时 |
| :--- | :--- | :--- |
| `11 03` | **接收器本机**（bond / vid / pid / connect） | ✅ 仍正常应答，`connect = 0` |
| `11 04` `11 06` `12 67` | **需要鼠标本体** | ❌ 一律返回解码后全零 |

实测证据（同一台设备、同一份未改动的探针程序）：

- 鼠标在线：`11 03` → `connect=1`；`11 06` → 电量 70%、`12 67` → 真实 DPI 档值。
- 鼠标休眠：`11 03` → `connect=0`；`11 04/06`、`12 67` → 连续 4 次全部"解码后全零"。

因此：

1. 用 `11 03` 的 `connect` 字段判定**鼠标是否在链路**，不要用"读失败了"来推断；
2. 鼠标休眠时**要清掉**电量的 `valid` 标志，不要显示成 0%；
3. 鼠标休眠**不算"读失败"**，不参与离线判定（离线判定只针对接收器消失）；
4. 界面上应显示「鼠标休眠中（接收器正常）」，而不是「已连接」。

`mchose-tray.exe --dump` 的退出码同样区分这三种情况：`0` 完整 / `2` 鼠标休眠（链路正常）/ `1` 其它不完整。

---

## 10. 一个反例：不要把"全零"当成合法状态

在修复过程中曾一度把判据放宽为"前两次拒绝全零、第 3 次起接受"，理由是
"设备可能真的处于全零状态"。实测结果是**回归**：重试拿到真数据之前就把滞后缓冲
当成了有效数据，`--dump` 输出

```
电量 0%    回报率 0 Hz    DPI 档值 0 0 0 0 0 0    读失败 0 次
```

——全是垃圾值却报告"读取成功"。这对用户比"读不到"更糟：**静默的错误数据**。

结论：本协议用到的读命令（`11 03/04/06`、`12 67`）真实负载都不可能全零
（至少含 vid/pid、固件字符串或 DPI 档值），所以严格拒绝全零是正确取舍。
"设备真的全零"这一情形在真实硬件上未出现过，不应为它牺牲错误检测能力。

---

## 11. 复现与未决问题

### 复现步骤

```cmd
cd tools
gcc -O2 -o hid_probe.exe hid_probe.c -lhid -lsetupapi
gcc -O2 -o mchose_probe.exe mchose_probe.c -lhid -lsetupapi
gcc -O2 -o mchose_monitor.exe mchose_monitor.c -lhid -lsetupapi

hid_probe.exe 5253 --desc        :: 1) 确认通道与报文长度
mchose_probe.exe                 :: 2) 读 11 03/04/06 + 12 67 并解析
mchose_monitor.exe 20            :: 3) 验证推送通道（发 nudge 后监听）
mchose_probe.exe mapping         :: 4) 复现回报率字段映射判定（可逆，自动还原）
mchose_probe.exe dpitest         :: 5) 复现 DPI 字段映射判定（可逆，自校验还原）
```

### 未决问题

| # | 问题 | 建议验证方式 |
| :--- | :--- | :--- |
| 1 | 设置是否需要显式"重载"才同步 RAM | 找 `0x0B`/`0xAA` 类命令，写入后重载再回读 |
| 2 | 有线模式（PID 0x1020）的档位字段是否同构 | 用 USB 线直连后重跑 `mchose_probe.exe` |
| 3 | 休眠写入是否即时生效 | 写入不同分钟数后回读 `12 67` 偏移 19 |
| 4 | 同方案其它机型（L7 Pro / K7 Ultra）兼容范围 | 跑 `mchose_probe.exe`，比对 `11 03` 的 vid/pid 与推送子类型 |
| 5 | 0xFF0B 那条 61 字节厂商通道的用途 | 本协议未使用，官方前端亦未见引用 |

---

## 10. 参考

- [Iris-0109/rapoo-tray](https://github.com/Iris-0109/rapoo-tray) — 结构参考（MIT），雷柏鼠标，协议完全不同
- [Fan4Metal/mouse_tray](https://github.com/Fan4Metal/mouse_tray) — 同方案 L7 Pro 的 `0xFF01` 通道与推送格式记录
- [MCHOSE-cli (GitLab)](https://gitlab.com/unk9201422/Mchose) — K7 Ultra 协议 wiki，`0x11/0x12/0x13` + XOR 0xFF 与本文一致
- 官方 MCHOSE HUB 公开 CDN 前端包 `https://cdn.mchose.com.cn/customPage/14/` — 命令表与 parser 定义来源
