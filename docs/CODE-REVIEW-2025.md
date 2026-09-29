# mchose-tray 代码审查报告（只读审查）与处理结论

> **处理结论（后续追加，按此为准）**
>
> 本报告由独立的只读审查子代理产出。逐条核对后处理如下：
>
> | 编号 | 问题 | 处理 |
> | :--- | :--- | :--- |
> | H1 | 菜单 ID 区间重叠（`IDM_SLEEP` 用分钟数编码 + 分支写成 `1601+200`，吞掉全部 DPI 档位） | **已修**。改为按下标编码、区间收窄，并新增 `static_assert` 与 `--selftest`（32 项检查全通过） |
> | H2 | `GuiLog` 把 `snprintf` 返回值当写入长度 | **已修**。夹紧为缓冲区容量 |
> | H3 | `off += snprintf(out+off, sizeof(out)-off, ...)` 下溢 | **已修**。新增 `AppendFmt()` 显式夹紧剩余空间，诊断输出全部改用它 |
> | M1 | `CreateCompatibleDC` 失败未判空（UB + 位图泄漏） | **已修**（3 处） |
> | M2 | 旧图标在 `NIM_MODIFY` 之前被 `DestroyIcon` | **已修**。先 modify 再 destroy |
> | M4 | `TaskbarCreated` 后 `NIM_ADD` 未重组 `g_nid` | 未改（低影响，已在报告中记录） |
> | M5 | `RequestRefresh` 被消费后丢弃 | **已修**。改为清零周期计时器、立刻重读 |
> | M6 | `Stop()` 超时后仍关闭句柄 | **已修**。超时保留句柄与事件，不清回调 |
> | M8 | 诊断分支在互斥体之前、`--dump` 与 GUI 并发抢设备 | **部分已修**：日志删除移到互斥体之后。诊断与 GUI 并发仍需人工避免（见 README） |
> | M9 | 选集合用 `>=` 且不校验帧长 | **已修**。要求 `FeatureReportByteLength >= 65`，并改为 `>` 保证确定性 |
> | M10 | 「解码后全零即无效」可能误判合法全零状态 | **已复核并驳回**。实测该判据必须严格：放宽后会把滞后缓冲当有效数据，显示成"电量 0%、回报率 125 Hz"的垃圾状态。且该判据正是**区分"鼠标休眠"与"已连接"**的关键（见下） |
> | UB1 | OSD 淡出末帧传负值给 `(BYTE)` | **驳回（不成立）**。代码在 `a <= 0` 时直接隐藏，不会传负值 |
>
> **审查未覆盖、由本次修复新发现并处理的问题：**
>
> 1. **推送句柄读失败后永不重开** —— 被动通道一次错误即永久死亡。
> 2. **每 9 秒无条件发布 `CHANGE_SETTINGS`** —— UI 会周期性弹 OSD。已改为仅在设置真变化时发布，并新增 `--watch` 回归检查。
> 3. **`ReadPush` 放弃在途 IRP**（真 UB）—— `CancelIo` 后未收割，迟到完成写入失效栈帧；且陈旧事件信号会让下次读误判失败并关掉推送句柄。已改为常驻 `OVERLAPPED` + `CancelIo` 后 `GetOverlappedResult(bWait=TRUE)` 收割。
> 4. **不能让输入读长期挂起** —— 实测把待决输入读长期留在队列会堵死本设备的命令通道，特性读全部失败。故采用"每次发起→超时即取消并收割"。
> 5. **未区分"接收器在线"与"鼠标已连接"** —— 鼠标休眠时 `11 04/06`、`12 67` 只返回全零，旧实现会显示"已连接，电量 0%"。现已用 `11 03` 的 `connect` 字段区分，休眠时显示「鼠标休眠中（接收器正常）」，`--dump` 以退出码 2 区分。
> 6. **写后立即回读会读到写入前的旧设置** —— 导致写校验假失败。已加稳定期 + 双读（`ReadAllSettingsSettled`），并用**真正会变的档位**验证通过（500Hz→档1、1000Hz→档2）。
> 7. **`failStreak` 双计数** —— 两条周期读各 +1，阈值 3 实际只等于 1~2 轮；且成功推送不清零。已改为每轮最多 +1、任何成功（含推送）清零，并加入离线指数退避。
>
> 报告正文如下（保留原样，作为审查记录）。
# mchose-tray 代码审查报告（只读审查，未修改任何被审文件）

审查范围：`src/main.cpp`、`src/tray_ui.h`、`src/tray_ui.cpp`、`src/mchose_protocol.h`、`src/mchose_protocol.cpp`
交叉核对：`src/device_manager.h`、`src/device_manager.cpp`、`docs/PROTOCOL.md`、`build.bat`
构建方式：MinGW-w64 g++，`-O2 -Wall -municode -mwindows -static`（`build.bat:15`）

结论速览：

| 级别 | 数量 | 编号 |
| :--- | :--- | :--- |
| 高 | 3 | H1（菜单 ID 区间重叠）、H2（`snprintf` 返回值当写入长度）、H3（自适应缓冲区 `+off` 下溢） |
| 中 | 10 | M1–M8、UB1、UB3 |
| 低 | 21 | L1–L21（其中 UB3 计入中级；UB5/UB6/UB8 计入低级，UB2/UB4 为已核对无缺陷项） |

---

## 1. 菜单命令 ID 冲突（重点，完整数值推演）

### 1.1 ID 宏的定义（`main.cpp:22-31`）

```
22: #define IDM_OSD      1300
23: #define IDM_STYLE    1301
24: #define IDM_AUTORUN  1302
25: #define IDM_EXIT     1303
26: #define IDM_REFRESH  1304
27: #define IDF_RATE     1400
28: #define IDM_RATE(i)  (1500 + (i))
29: #define IDM_SLEEP_OFF 1600
30: #define IDM_SLEEP(n) (1601 + (n))
31: #define IDM_DPI(s)   (1700 + (s))
```

注意：`IDF_RATE`（1400）被定义但**从未在构造菜单时使用**（`main.cpp:116` 用的是 `IDM_RATE(i)`），属于死宏，也说明这里已经有过一次 ID 方案变更——正是这次变更留下了区间重叠。

### 1.2 每个菜单项实际产生的 ID（按代码逐项算）

**A. 固定项**（`main.cpp:104,149,150,151,152-153,155`）

| 菜单项 | 行号 | 表达式 | ID |
| :--- | :--- | :--- | :--- |
| 状态标题（`MF_DISABLED`） | 104 | `0` | 0 |
| 显示状态悬浮窗 | 149 | `IDM_OSD` | **1300** |
| 立即刷新 | 150 | `IDM_REFRESH` | **1304** |
| 切换图标样式 | 151 | `IDM_STYLE` | **1301** |
| 开机自启 | 152-153 | `IDM_AUTORUN` | **1302** |
| 退出 | 155 | `IDM_EXIT` | **1303** |

固定项区间：`[1300, 1304]`，两两互不重叠。

**B. 回报率项**（`main.cpp:110-117`，`AppendMenuW(hRate, flags, IDM_RATE(i), label)`）

`i = 0..min(rateCount,6)-1`，`rateCount` 来自 `st.rateCount`（默认 6，无线 6 / 有线 3，见 `device_manager.cpp:416`）。
ID 序列（i 最大 5）：

```
i       : 0    1    2    3    4    5
ID      : 1500 1501 1502 1503 1504 1505
```

回报率项区间：`[1500, 1505]`（最宽情形）。

**C. 休眠项**（`main.cpp:123` 与 `main.cpp:124-130`）

```
123: AppendMenuW(hSleep, flags, IDM_SLEEP_OFF, L"关闭休眠");           → 1600
129: AppendMenuW(hSleep, f, IDM_SLEEP(kSleepChoices[i]), label);
     kSleepChoices[] = { 1, 3, 5, 10, 30, 60 }   (main.cpp:55)
```

逐项展开 `IDM_SLEEP(n) = 1601 + n`：

| i | `kSleepChoices[i]` | 标签 | ID |
| :--- | :--- | :--- | :--- |
| — | — | 关闭休眠 | **1600** |
| 0 | 1 | 1 分钟 | **1602** |
| 1 | 3 | 3 分钟 | **1604** |
| 2 | 5 | 5 分钟 | **1606** |
| 3 | 10 | 10 分钟 | **1611** |
| 4 | 30 | 30 分钟 | **1631** |
| 5 | 60 | 60 分钟 | **1661** |

休眠项区间（**含空洞**）：`[1600, 1661]`；被真正占用的 ID 是
`{1600, 1602, 1604, 1606, 1611, 1631, 1661}`。（`1601` 这个 ID 被宏表达式刻意跳过，因为 `n ≥ 1`，见 `main.cpp:30` 的 `+1`。）

**D. DPI 项**（`main.cpp:135-145`，`AppendMenuW(hDpi, flags, IDM_DPI(i), label)`）

`i = 0..5`，`IDM_DPI(s) = 1700 + s`：

```
i       : 0    1    2    3    4    5
ID      : 1700 1701 1702 1703 1704 1705
```

DPI 项区间：`[1700, 1705]`。

### 1.3 分发端实际使用的区间（`main.cpp:165-207`）

| 顺序 | 行号 | 条件 | 语义 |
| :--- | :--- | :--- | :--- |
| 1 | 165 | `cmd == 0` | 返回 |
| 2 | 167 | `cmd == 1303` | 退出 |
| 3 | 171 | `cmd == 1300` | 显示 OSD |
| 4 | 175 | `cmd == 1301` | 切换样式 |
| 5 | 180 | `cmd == 1302` | 自启开关 |
| 6 | 184 | `cmd == 1304` | 立即刷新 |
| 7 | 189 | `1500 ≤ cmd < 1600` | 回报率 |
| 8 | 195 | `cmd == 1600` | 关闭休眠 |
| 9 | **199** | **`1601 ≤ cmd < 1801`** | **休眠分钟数** |
| 10 | 204 | `1700 ≤ cmd < 1706` | DPI 档位 |

### 1.4 重叠判定（关键结论）

```
分支 9（休眠）声明覆盖：[1601, 1801)   = 1601 … 1800
分支 10（DPI）声明覆盖：[1700, 1706)   = 1700 … 1705

交集 = [1700, 1705]  ←→  DPI 的 6 个菜单项 ID 全集
```

**交集非空，且等于 DPI 项全集。** 由于分支 9（第 199 行）在分支 10（第 204 行）之前，且两个分支内部都是 `return`，DPI 分支**永远不可达**。

具体触发推演（用户操作 → 返回 ID → 落到的分支 → 实际执行）：

| 用户点击 | 返回 cmd | 命中分支 | 传给设备的值 | 正确应为 |
| :--- | :--- | :--- | :--- | :--- |
| DPI 第 1 档 | 1700 | 分支 9（行 201） | `RequestSetSleep(true, 99)` → 休眠 **99 分钟** | `RequestSwitchDpiStage(0)` |
| DPI 第 2 档 | 1701 | 分支 9 | 休眠 **100 分钟** | `RequestSwitchDpiStage(1)` |
| DPI 第 3 档 | 1702 | 分支 9 | 休眠 **101 分钟** | `RequestSwitchDpiStage(2)` |
| DPI 第 4 档 | 1703 | 分支 9 | 休眠 **102 分钟** | `RequestSwitchDpiStage(3)` |
| DPI 第 5 档 | 1704 | 分支 9 | 休眠 **103 分钟** | `RequestSwitchDpiStage(4)` |
| DPI 第 6 档 | 1705 | 分支 9 | 休眠 **104 分钟** | `RequestSwitchDpiStage(5)` |

（`minutes = cmd - 1601`，见 `main.cpp:200`。）

**后果不止"点错功能"，而是会写坏设备 EEPROM**：`SetSleep`（`mchose_protocol.cpp:356-367`）的 `minutes` 只做 `0..255` 截断，99–104 全部合法下发；`docs/PROTOCOL.md:278` 明确"写命令都可能改 EEPROM"。也就是每次点 DPI 档位，都会把休眠时间改成 99–104 分钟，且写完还会 `Publish(CHANGE_SETTINGS)`（`device_manager.cpp:167`），刷新界面把新值读回来显示——用户会在菜单里看到"休眠 60 分钟"的勾选消失。

**DPI 功能实际完全不可用**；同时注意 `ShowOsd` 里 `dpiIndexRaw` 仍会变化（若设备后来因休眠写入等触发了刷新），UI 与实际不符。

### 1.5 次要问题（同一处代码的连带缺陷）

1. **分支 9 上界是魔数，与"分钟数"耦合错误**：`1601 + 200` 恰好等于 1801，纯属块大小选择，正好越过 1700。若把 DPI 基准从 1700 改成 1706（或把 `IDM_RATE` 扩到 8 档），重叠形态会改变但依然存在。
2. **ID 被当作数据**：`IDM_SLEEP(n) = 1601 + n` 让 ID 等于"分钟数+1601"，因此"任意落在区间内的 ID"都会被当成合法分钟数下发，没有白名单校验（`kSleepChoices` 未被复用）。对比 DPI 分支有 `cmd - 1700` 后由 `RequestSwitchDpiStage` 做了一次 0–5 校验（`device_manager.cpp:457`），休眠分支没有任何校验。
3. **`IDM_DPI` 分支的 `cmd < 1706` 上界写死为 6**：一旦把 DPI 项从 6 个改成 7 个（`main.cpp:135` 的 `i < 6` 改成 `i < 7`），最后一个 ID 会静默失效。建议用 `IDM_DPI_MAX`。
4. **`IDF_RATE`（1400）为死宏**（`main.cpp:27`）：易误导后续维护者以为 1400 段是回报率 ID 段。
5. **Windows 保留 ID 段**：0–15 由系统用于 `WM_SYSCOMMAND`（SC_CLOSE 等）。当前最小 ID 为 1300，安全；但没有任何注释或静态断言保护这个下界。

### 1.6 建议修法

最小改动（保持现有结构）：

```c
/* 重新划分：每段留足空间且上界显式 */
#define IDM_RATE_BASE   1500
#define IDM_RATE(i)     (IDM_RATE_BASE + (i))          /* 1500..1507 */
#define IDM_RATE_MAX    8
#define IDM_SLEEP_OFF   1600
#define IDM_SLEEP(n)    (1600 + (n))                   /* n ∈ 1..60 → 1601..1660，去掉 +1 偏移 */
#define IDM_SLEEP_MAX   60
#define IDM_DPI_BASE    1700
#define IDM_DPI(s)      (IDM_DPI_BASE + (s))           /* 1700..1799 */
#define IDM_DPI_MAX     6
```

分发端改为 `switch` + 精确边界，并把 DPI 判在休眠之前（防御式，即便将来再改宏也不会错投）：

```c
if (cmd >= IDM_DPI_BASE && cmd < IDM_DPI_BASE + IDM_DPI_MAX) {
    Device::RequestSwitchDpiStage(cmd - IDM_DPI_BASE);
    return;
}
if (cmd == IDM_SLEEP_OFF) { Device::RequestSetSleep(false, 0); return; }
if (cmd > IDM_SLEEP_OFF && cmd <= IDM_SLEEP_OFF + IDM_SLEEP_MAX) {
    int minutes = cmd - IDM_SLEEP_OFF;
    /* 白名单：只在 kSleepChoices 中允许的值才下发 */
    for (int i = 0; i < kSleepCount; i++)
        if (kSleepChoices[i] == minutes) { Device::RequestSetSleep(true, minutes); return; }
    return;
}
```

更彻底的做法：给每个子菜单用独立的命令区间并加静态断言
`static_assert(IDM_RATE_BASE + IDM_RATE_MAX <= IDM_SLEEP_OFF, "");`
`static_assert(IDM_SLEEP_OFF + IDM_SLEEP_MAX < IDM_DPI_BASE, "");`
这样重叠会在编译期被拦住。

---

## 2. GDI / 资源泄漏与使用顺序

### H1（高）`snprintf` 自适应偏移量在下溢时变成超大长度 —— `main.cpp:303-390`

见第 3 节 H2（同一条问题在字符串与资源两个维度都成立，此处不重复计数，统一在 3.1 描述）。

### M1（中）`CreateCompatibleDC` 失败未检查 —— `tray_ui.cpp:122-123`、`tray_ui.cpp:185-186`

```cpp
122: HDC hdc = CreateCompatibleDC(NULL);
123: HGDIOBJ old = SelectObject(hdc, hbm);       // hdc 可能为 NULL
```

- **触发条件**：GDI 句柄耗尽 / 极低内存（`CreateCompatibleDC` 返回 NULL）。
- **后果**：向 `SelectObject` 传入 NULL HDC 属未定义行为；更确定的是 `DrawCapsule:118` 已经创建成功的 `hbm` 在此路径**永远泄漏**（`hdc == NULL` 时没有分支去 `DeleteObject(hbm)`，函数仍会跑到 161-166 去 `DeleteDC(hdc)`）。`DrawNumber:181-186` 同样。
- **严重级别**：中（当前环境难复现，但属于"每个 CreateXXX 都有对应 Delete"的唯一破口）。
- **建议**：`if (hdc == NULL) { DeleteObject(hbm); return NULL; }`，并对 `DrawNumber` 里 `CreateFontW` 返回 NULL 也照此处理（`tray_ui.cpp:209-212` 现在无判空，NULL 会被选进 DC 并 `DeleteObject(NULL)`）。

### M2（中）托盘图标在 `NIM_MODIFY` 之前就被 `DestroyIcon` —— `main.cpp:62-72`

```cpp
62: HICON icon = TrayUi::CreateBatteryIcon(...);
63: if (icon != NULL) {
64:     if (g_hIcon) DestroyIcon(g_hIcon);        // ← 旧图标先销毁
65:     g_hIcon = icon;
66:     g_nid.hIcon = g_hIcon;
67:     g_nid.uFlags = NIF_ICON | NIF_TIP | NIF_MESSAGE;
68:     Shell_NotifyIconW(NIM_MODIFY, &g_nid);    // ← 之后才通知
69: }
70:
71: TrayUi::UpdateTooltip(g_nid, st);
72: Shell_NotifyIconW(NIM_MODIFY, &g_nid);
```

- **触发条件**：任意一次状态刷新（`WM_APP_STATE`，每 3 秒至少 1 次）；若此时 explorer 恰好用旧 `hIcon` 重绘托盘（主题切换、DPI 变更、通知区域刷新），或第 68 行的 `NIM_MODIFY` 尚未被 explorer 处理。
- **后果**：explorer 侧持有已释放的 HICON，表现为图标空白/闪烁，极端情况是 explorer 的 GDI 引用失效。
- **严重级别**：中（有竞态窗口，实际表现通常是短暂空白或"图标消失直到下次刷新"）。
- **建议**：先 `NIM_MODIFY` 换上新图标，再销毁旧的；并把"图标"从"提示文本"更新中拆开（现在 68 与 72 是两次连续 `NIM_MODIFY`，第 72 行重复通知是多余的）：

```cpp
HICON old = g_hIcon;
HICON icon = TrayUi::CreateBatteryIcon(...);
if (icon != NULL) {
    g_hIcon = icon;
    g_nid.hIcon = icon;
}
g_nid.uFlags = NIF_ICON | NIF_TIP | NIF_MESSAGE;
TrayUi::UpdateTooltip(g_nid, st);
Shell_NotifyIconW(NIM_MODIFY, &g_nid);        /* 一次即可 */
if (old && old != g_hIcon) DestroyIcon(old);
```

### M3（中）只有"设置变化"时不重绘图标，状态与图标不一致 —— `main.cpp:58-76` + `213-218`

```cpp
213: case WM_APP_STATE: {
214:     DWORD mask = (DWORD)wp;
215:     bool newConn = (mask & Device::CHANGE_CONNECTED) != 0;
216:     RefreshTray((mask & Device::CHANGE_SETTINGS) != 0, newConn);
217:     return 0;
```

`RefreshTray` 的 `showOsdIfNeeded` 实际控制的是"是否弹 OSD"（第 74 行），**图标与提示文本无论如何都会重建**——所以这里其实没有图标漏刷新问题。真正的缺陷是：`CHANGE_SETTINGS`-only 的刷新也会 `ShowOsd`，而 `CHANGE_BATTERY`-only 只更新图标+提示、不弹 OSD。

- **触发条件**：回报率/DPI/休眠写入成功后 `Publish(CHANGE_SETTINGS)`（`device_manager.cpp:288, 359`）。
- **后果**：用户仅改设置也会看到 OSD 弹出（这与 `g_osdOnConnect` 的"仅在连接/必要时弹"设计意图相悖，视觉噪音）。反之电量变化不弹 OSD 是合理的。
- **严重级别**：中低（行为不一致，非崩溃）。
- **建议**：把参数语义拆成两个布尔量 `bool iconDirty` / `bool osdWanted`，由调用点明确表达；`ShowOsd` 的触发条件改为 `newConn || (mask & CHANGE_VERSION) || 显式用户动作`。

### L1（低）`OsdPaint` 每次重绘都新建位图 —— `tray_ui.cpp:234-263`

```cpp
234: HDC mem = CreateCompatibleDC(hdc);
235: HBITMAP bmp = CreateCompatibleBitmap(hdc, g_osdW, g_osdH);
```

- 释放顺序本身**正确**（259 `BitBlt` → 261 `SelectObject(mem, old)` → 262 `DeleteObject(bmp)` → 263 `DeleteDC(mem)`），`SelectObject` 也恢复到了 `old`，没有"删除仍被选中的对象"问题。
- 但淡出阶段每 16 ms 一次 `WM_PAINT`（`tray_ui.cpp:293` + `279 InvalidateRect`），每次都重建 300×92 位图与 DC，产生可观的 GDI 抖动。建议缓存 `mem/bmp`，尺寸变化时才重建。
- `BeginPaint/EndPaint` 配对正确（232/264）。

### L2（低）`CreateCompatibleBitmap` / `CreateSolidBrush` 失败未处理 —— `tray_ui.cpp:235, 239` 等

`CreateCompatibleBitmap` 失败返回 NULL 时 `SelectObject(mem, NULL)`，随后 `BitBlt` 失败但不崩溃；建议判空并跳过绘制。

### L3（低）DIB alpha 后处理本身是安全的，但"非黑即不透明"会撕裂抗锯齿 —— `tray_ui.cpp:76-103`

```cpp
82: bmi.bmiHeader.biHeight = -size;      /* 自上而下 */
89: for (int i = 0; i < size * size; i++) {
90:     unsigned char b = px[i * 4 + 0], g = px[i * 4 + 1], r = px[i * 4 + 2];
91:     px[i * 4 + 3] = (r | g | b) ? 255 : 0;
92: }
```

- **越界核查（结论：不越界）**：32bpp DIB 的行距是 4 字节对齐，而 `width * 4` 天然是 4 的倍数，所以 `biWidth = size` 时 stride 恰为 `size * 4`，第 i 个像素的 4 字节落在 `[i*4, i*4+3]`，最大下标 `(size*size-1)*4+3 = size*size*4-1`，与 `CreateDIBSection` 分配的 `size*size*4` 完全吻合（`tray_ui.cpp:117`/`181` 的 `memset(bits, 0, (size_t)size*size*4)` 也说明作者知道总大小）。
- **`size` 的有效范围**：唯一入口 `CreateBatteryIcon`（`tray_ui.cpp:367-374`）把 `size` 夹在 `[16, 64]`，所以 `size*size*4` 最大 16384，`int` 不会溢出。但 `BuildIconFromDib`/`DrawCapsule`/`DrawNumber` 都是 `namespace {}` 内的私有函数、参数未校验；`size * size` 是 `int` 运算，若将来从别处传入 >46341 的值会整型溢出并导致越界写。建议在 `BuildIconFromDib` 内加 `if (size <= 0 || size > 256) return NULL;`，并把循环条件写成 `(size_t)size * size`。
- **质量问题**：`(r|g|b) ? 255 : 0` 把所有非黑像素置为完全不透明，`DrawTextW` 的抗锯齿灰边要么全不透明要么全透明，文字边缘会出现硬锯齿；同时纯黑描边会被判为透明。这是"能用但粗糙"的实现，如果追求观感建议改用 `CreateIconIndirect` 前先画到固定背景色并按颜色键抠图，或用 32bpp 描边矢量重画。
- **另外 `hMask` 是"全 0"的单色位图**（`tray_ui.cpp:94`，`CreateBitmap(size,size,1,1,NULL)`），在 Windows 2000+ 上 32bpp 图标的可见性由颜色位图的 alpha 决定，掩码只作兼容，当前写法可行；但若要兼容老 API（如 `DrawIconEx` 的 `DI_MASK`），需要真正填掩码。

### L4（低）`Init` 未检查窗口/类创建失败，`g_hFont1/2` 可能为 NULL 被选入 DC —— `tray_ui.cpp:341-357`

`RegisterClassExW`（341）与 `CreateWindowExW`（346）返回值均被忽略；若窗口创建失败，`g_hOsd == NULL`，`ShowOsd` 会在 401 行安全返回（这一点作者处理对了）。但 `CreateFontW`（350/353）失败时 `OsdPaint:245/249` 会把 NULL 选入 DC。建议加判空。

### L5（低）`Cleanup` 后窗口类未注销、定时器未显式 `KillTimer` —— `tray_ui.cpp:360-365`

`DestroyWindow(g_hOsd)` 会随窗口销毁其定时器，行为上没问题；`RegisterClassExW` 注册的 `McHoseOsdWindow`（341）与 `main.cpp:440` 的 `McHoseTrayMessageWnd` 都未 `UnregisterClassW`。进程退出时由系统回收，属洁癖级问题（若将来做成 DLL/可重入模块会真泄漏类原子）。

### L6（低）`hMutex` 资源在已存在实例的早退路径未关闭，且托盘图标已创建 —— `main.cpp:415-419` 与 `443-475`

```cpp
415: HANDLE hMutex = CreateMutexW(NULL, TRUE, L"Local\\mchose-tray-single");
416: if (hMutex != NULL && GetLastError() == ERROR_ALREADY_EXISTS) {
417:     GuiLog("[X] 已有实例在运行，退出");
418:     return 0;                     // hMutex 未 CloseHandle（进程退出兜底）
419: }
```

更值得注意的顺序问题：`mchose-tray-gui.log` 的 `DeleteFileW`（413）与 `GuiLog("[1]…")`（414）发生在互斥体检查**之前**，所以第二个实例**会先把第一个实例的日志删掉**——`--dump` 排错时会把 GUI 实例的日志毁掉。此外若 `hMutex == NULL`（系统资源耗尽），第 416 行的短路使检查被整体跳过，程序会继续跑成多实例（`CreateMutexW` 失败时应当拒绝启动或至少记日志）。

### L7（低）`ShowMenu` 未检查 `CreatePopupMenu` 返回值 —— `main.cpp:90-93, 118, 132, 146`

子菜单创建失败返回 NULL 时，`AppendMenuW(hMenu, MF_POPUP|MF_STRING, (UINT_PTR)NULL, L"回报率")` 会静默失败（该子菜单整段消失），且 163 行的 `DestroyMenu(NULL)` 在 Win2000+ 上是安全的 no-op。建议判空后 `DestroyMenu(hMenu); return;`。

---

## 3. 字符串安全

### H2（高）`off += snprintf(out + off, sizeof(out) - off, …)` 的自适应偏移量在溢出后下溢为天量长度 —— `main.cpp:301-390`，同型 `main.cpp:353-390`

```cpp
301:             char out[1024];
302:             int off = 0;
303:             off += snprintf(out + off, sizeof(out) - off,
304:                             "=== 写命令验证（--set-rate %d）===\n", setRateHz);
...
318:                 DWORD w = 0; WriteFile(hOut, out, (DWORD)strlen(out), &w, NULL);
```

- **机理**：`off` 是 `int`，`sizeof(out)` 是 `size_t`（无符号 64 位）。一旦某次输出被截断，`snprintf` 返回的是**"本应写入的长度"**，`off` 可以超过 `sizeof(out)`。此后 `sizeof(out) - off` 触发整型提升：`int off` 先转成 `size_t`，相减得一个接近 `2^64` 的巨大值。于是下一次 `snprintf` 拿到"我还有 1.8e19 字节可写"的信息，会**照常写满栈**；同时 `out + off` 本身已是数组尾之后一格以上的指针（UB）。
- **触发条件（当前不可达，但只差一步）**：1024 字节缓冲的实际最大输出约 350–450 字节（最长项是 `st.modelName` 的 63 宽字符经 UTF-8 放大到约 190 字节），确实有余量；`--dump` 分支的 4096 字节（`main.cpp:353`）余量更大。但注意 `off` 累加链有 20 多次调用，任何一次新增字段、`modelName` 变长、或未来把 `%s` 换成用户可控字符串，都会立刻变成栈溢出。这是典型的"延迟引爆"缺陷。
- **严重级别**：高（`-O2` 下可能被优化成不可预测的栈破坏，且写入的是 `WriteFile` 长度参数，同时伴随 `strlen(out)` 对未终止缓冲区的越界读）。
- **建议**：统一改成显式剩余量检查，不要依赖 `snprintf` 返回值累加：

```cpp
static void AppendFmt(char *buf, int cap, int &off, const char *fmt, ...)
{
    if (off < 0 || off >= cap) return;
    va_list ap; va_start(ap, fmt);
    int n = vsnprintf(buf + off, (size_t)(cap - off), fmt, ap);
    va_end(ap);
    if (n < 0) return;
    if (n >= cap - off) off = cap;      /* 已截断：标记满 */
    else                off += n;
}
```

调用处一律 `AppendFmt(out, (int)sizeof(out), off, "...")`。另建议把 `out` 从栈上挪到静态缓冲或 `malloc`，并在 `WriteFile` 前用 `off`（而不是 `strlen(out)`）作为长度。

### H3（高）`GuiLog` 把 `snprintf` 的返回值直接当写入长度 —— `main.cpp:36-46`

```cpp
41:     char line[256];
42:     int n = snprintf(line, sizeof(line), "%s\n", msg);
43:     DWORD w = 0;
44:     WriteFile(h, line, (DWORD)n, &w, NULL);
```

- **机理**：`snprintf` 在截断时返回"本应写入的长度"（可 > 255）。`WriteFile` 于是从 256 字节的栈数组读出 `n` 个字节——**栈越界读**并把它写进日志文件；若 `n` 极大（`msg` 异常长）还可能越过栈页导致崩溃。`snprintf` 失败时返回负值，`(DWORD)n` 变成 `0xFFFFFFFF`（4 GB 写入请求）。
- **触发条件**：任何长度 ≥ 256 的 `msg`。当前所有调用点的字面量都很短，唯一动态内容是 `main.cpp:462-463` 的 `dbg` 与 `main.cpp:471` 的 `dbg`（≤200 字节），所以现在不会触发；但这是"函数自身不安全、靠调用方自律"，属于必须修的类型。
- **严重级别**：高（越界读 + 日志写入任意长度；`GuiLog` 是排错路径，恰恰在最需要它的时候可能是被攻击/异常状态）。
- **建议**：`if (n < 0) return; size_t len = ((size_t)n < sizeof(line)) ? (size_t)n : strlen(line); WriteFile(h, line, (DWORD)len, &w, NULL);`；或直接 `WriteFile(h, line, (DWORD)strlen(line), ...)`（`snprintf` 必定终止）。

### L8（低）`UpdateTooltip` 的 `wcsncpy(nid.szTip, buf, 127)` 依赖结构体字段宽度 —— `tray_ui.cpp:378-397`

```cpp
395:     wcsncpy(nid.szTip, buf, 127);
396:     nid.szTip[127] = L'\0';
```

- **核查结论：正确**。`NOTIFYICONDATAW::szTip` 在 `_WIN32_IE >= 0x0500`（Windows 2000+ / 现代 MinGW-w64 SDK，本工程 `-municode -mwindows` 必用该头）下为 `WCHAR[128]`，`wcsncpy` 最多写 127 个 + 显式终止符 = 128，恰好填满不越界。
- **残留风险**：若将来编译到 `_WIN32_IE < 0x0500`，`szTip` 会退化为 `WCHAR[64]`，这两行立刻变成越界写。建议改成 `static_assert(sizeof(nid.szTip) == 128, ...)` 或改用 `wcsncpy_s(nid.szTip, buf, _TRUNCATE)`。
- **内容最坏长度核算（结论：`buf[256]` 够用）**：格式串为 `L"%s\n电量 %d%%%s · %s\n回报率 %s · DPI 档 %d"`（`tray_ui.cpp:387-393`）。最坏情形 `modelName` = 63 字符（`Device::State::modelName[64]`，`device_manager.h:35`）+ 固定文本约 30 字符 + `rate` ≤ 8 + 数字 ≤ 12 ≈ 115 字符，远小于 256。

### L9（低）`ShowOsd` 的 `g_osdLine[i]` 长度充足，但 `DT_NOCLIP` 会让长文本溢出窗口 —— `tray_ui.cpp:407-427` + `247/252/256`

最坏情形 `L"%s   %s"`：`modelName`(63) + 3 空格 + `ModeText`(≤4) = 70 < 160，安全。
`L"电量 %d%%%s · %s · 固件 %s"`：63(firmware 上限 `wchar_t[32]` 实为 31) + 数字等 ≈ 60 < 160，安全。
**问题在绘制**：`DrawTextW`（247/252/256）带 `DT_NOCLIP`，当文本实际宽度超过 `r1/r2/r3`（右边界 `g_osdW - 14 = 286`）时会画到窗口外并被窗口裁剪。建议去掉 `DT_NOCLIP` 或加 `DT_END_ELLIPSIS`。

### L10（低）`ToggleAutoRun` 的路径截断与未终止风险 —— `tray_ui.cpp:469-472`

```cpp
469:         wchar_t path[MAX_PATH] = {0};
470:         GetModuleFileNameW(NULL, path, MAX_PATH);
471:         wchar_t quoted[MAX_PATH + 8];
472:         swprintf(quoted, MAX_PATH + 8, L"\"%s\"", path);
```

- **当前安全**：`quoted` 容量 268，最坏输入 `path` 为 259 字符 + 1（`swprintf` 终止）= 263 < 268。
- **风险点**：`GetModuleFileNameW(NULL, path, MAX_PATH)` 在路径长度 ≥ 260 时返回 260 且在旧行为下不保证终止；此后 `swprintf` 若因超长返回 -1，**缓冲区内容未定义**，紧接着 `wcslen(quoted)`（474）就是对未终止缓冲区的越界读，注册表里可能写进垃圾。
- **建议**：`SetLastError(0); DWORD n = GetModuleFileNameW(NULL, path, MAX_PATH); if (n == 0 || n >= MAX_PATH) return;` 且 `if (swprintf(...) < 0) return;`；长路径支持（`\\?\`）则改用 `GetModuleFileNameW(NULL, NULL, 0)` 动态分配。

### L11（低）`GetProductString` 静默截断 —— `mchose_protocol.cpp:326-338`

```cpp
331:     wchar_t buf[256];
333:     if (!HidD_GetProductString(h, buf, sizeof(buf))) return false;
335:     wcsncpy(out, buf, outChars - 1);
336:     out[outChars - 1] = L'\0';
```

缓冲区/终止处理正确（`device_manager.cpp:240` 传入 `prod[128]`，`outChars - 1` 与显式终止符配合无误）。`HidD_GetProductString` 的第 4 参数声明为 `ULONG`，`sizeof(buf)` 是 `size_t`（64 位下 8 字节），隐式收窄为 512，数值上正确但建议显式 `(ULONG)sizeof(buf)` 以免将来改 `buf` 大小时触发 `-Wconversion` 或截断（若 `buf` 超过 4 GB 才会真出问题，属理论）。若产品串长于输出缓冲则静默截断且**返回 true**，调用方无法区分"读到短名"与"截断了"，建议在截断时返回 false 或提供长度出参。

### 补充：`--dump` 的 `WideCharToMultiByte` 用法正确 —— `main.cpp:347-351`

```cpp
349:     WideCharToMultiByte(CP_UTF8, 0, st.modelName, -1, name, sizeof(name), NULL, NULL);
```

`st.modelName` = `wchar_t[64]`，UTF-8 最坏 3 字节/字符 → 需 189 + 1 字节，`name[128]` **可能被截断**（`WideCharToMultiByte` 会返回 0 并在 `ERROR_INSUFFICIENT_BUFFER` 下保持目标未终止——但这里是 `{0}` 初始化，所以最坏只是型号名显示不全，不会越界）。建议 `name[256]` 或按需要放大。

### 附带（源码编码，可能影响字面量）—— 所有 .cpp 均无 BOM

`Get-Content -Encoding Byte` 显示 `main.cpp / tray_ui.cpp / mchose_protocol.cpp / device_manager.cpp` 的前四字节均为 `47,42,10,32`（即 `/*\n `），**UTF-8 无 BOM**。GCC 在无 BOM 时按 `-finput-charset`（默认 UTF-8）解释源文件，因此 `L"⚡充电中"`、`L"未连接"` 等宽字面量应按 UTF-8 正确转码；`build.bat:15` 未显式指定 `-finput-charset=UTF-8 -fexec-charset=UTF-8`，一旦有人用 GBK 编码另存文件，字面量会静默变乱码。建议在 `build.bat` 中显式加 `-finput-charset=UTF-8`，或给源文件加 BOM。

---

## 4. 消息与生命周期

### 4.1 `g_wmTaskbarCreated` 的处理位置（结论：正确）

```cpp
432:     g_wmTaskbarCreated = RegisterWindowMessageW(L"TaskbarCreated");
...
254:     if (msg == g_wmTaskbarCreated) {
255:         /* explorer 重启后重新挂载托盘图标 */
256:         Shell_NotifyIconW(NIM_ADD, &g_nid);
257:         RefreshTray(false, false);
258:         return 0;
259:     }
261:     return DefWindowProcW(hWnd, msg, wp, lp);
```

- `RegisterWindowMessageW`（432）在 `CreateWindowExW`（443）之前调用，窗口创建时消息已能正确匹配；
- 检查放在 `switch` 的 `default` 之后（254），不会被任何具体 `case` 抢先，放置位置正确；
- 注册消息号一定 ≥ 0xC000，不会与 `WM_APP_STATE`（`WM_APP+2` = 0x8002）冲突。

**缺陷 M4（中）**：`NIM_ADD` 分支没有重置 `g_nid.uFlags` / `g_nid.szTip`。

`g_nid.uFlags` 在 `RefreshTray:67` 被赋值为 `NIF_ICON | NIF_TIP | NIF_MESSAGE`，看起来够用；但 `g_nid.hIcon` 依赖 `RefreshTray` 是否成功创建过图标（`CreateBatteryIcon` 返回 NULL 时 `g_nid.hIcon` 保持上一次的值或 NULL，见 M2 的判空分支）。explorer 重启后若 `hIcon` 为 NULL，会添加一个无图标项。建议在 254 分支里重新组装一遍 `g_nid`（`uFlags`、`uCallbackMessage`、`uID`、`hIcon`、`szTip`）再 `NIM_ADD`，并检查返回值后记日志。

**缺陷 L12（低）**：`TaskbarCreated` 只在收到广播时重挂，而 `NIM_ADD` 可能因 explorer 尚未就绪而失败（`Shell_NotifyIconW` 返回 FALSE），当前没有重试。建议失败时 `SetTimer` 500 ms 后重试（有上限）。

### 4.2 `WM_APP_STATE` 的递归/重入（结论：无递归，但存在状态覆盖）

```cpp
213:     case WM_APP_STATE: {
...
216:         RefreshTray((mask & Device::CHANGE_SETTINGS) != 0, newConn);
```

- `RefreshTray` 内的 `ShowOsd`（`main.cpp:75`）只做 `SetTimer` / `SetWindowPos` / `InvalidateRect`，都是异步的，不会同步派发 `WM_APP_STATE`，**不存在递归**。
- 真正的重入源是 `ShowMenu`（`main.cpp:160` 的 `TrackPopupMenu`）：它建立模态循环并留在 `MainWndProc` 栈帧内，期间到达的 `WM_APP_STATE` 会被**排队延迟**处理，因此右键菜单打开时托盘图标不会刷新（最长可挂到用户关闭菜单）。这对菜单里的"电量%""勾选项"意味着**菜单内容可能是几分钟前的快照**（`Device::GetState()` 在 88 行只取一次）。建议在 `ShowMenu` 返回后补一次 `RefreshTray(false, false)`，或在菜单打开期间用 `TrackPopupMenuEx` 之前先 `Device::RequestRefresh()` 并给设备一点时间。
- 另一处竞态：`OnDeviceStateChanged`（`main.cpp:79-84`）在工作线程调用 `PostMessageW`，而 `g_hMain` 的读写没有同步（`main.cpp:82` 判空、`main.cpp:443` 赋值）。`Device::Start`（477）在 `g_hMain` 赋值之后，正常路径无问题；但 `Device::Stop`（488）之后工作线程理论上仍可能投递（见 M7），属于低概率。

### M5（中）`RequestRefresh` 被消费后直接丢弃，"立即刷新"菜单项无效 —— `device_manager.cpp:186` + `main.cpp:184-188`

```cpp
/* device_manager.cpp */
186:     if (doRefresh) handled = true;
```

```cpp
/* main.cpp */
184:     if (cmd == IDM_REFRESH) {
185:         Device::RequestRefresh();
186:         TrayUi::ShowOsd(Device::GetState(), NULL);     // 弹的是旧快照
187:         return;
188:     }
```

- `RequestRefresh`（`device_manager.cpp:466-471`）只置 `g_pendRefresh = true`；工作线程摘取后（135-139）**除了把 `handled` 置真之外不做任何事**——既不重置 `lastInfoTick`/`lastSetTick`，也不读设备。
- **后果**：菜单"立即刷新"和单击托盘（`main.cpp:222-223`）都不会触发主动读取，用户看到的是最长 9 秒前的缓存（`lastSetTick` 周期 9000 ms，`device_manager.cpp:345`）。因为 `handled == true`，还会走 `Publish(CHANGE_SETTINGS)`（288），弹一次 OSD 显示陈旧的 DPI/回报率。
- **建议**：在 `RunPendingCommands` 里把 `doRefresh` 翻译成"给 `lastInfoTick`/`lastSetTick` 立刻过期"（改为出参或返回标志），或在摘取时直接置 `lastInfoTick = lastSetTick = 0`。

### M6（中）`Device::Stop` 4 秒后强制关句柄，工作线程可能仍在运行 —— `device_manager.cpp:410-423`

```cpp
413:     if (g_hThread != NULL) {
414:         WaitForSingleObject(g_hThread, 4000);
415:         CloseHandle(g_hThread);
416:         g_hThread = NULL;
417:     }
```

- `WaitForSingleObject` 返回 `WAIT_TIMEOUT` 时没有检查，直接 `CloseHandle(g_hThread)` 并把全局置 NULL。若工作线程正卡在某个 HID 调用（`HidD_SetFeature` 在设备拔出后可能长时间不返回；`OpenControlDevice` 的枚举也耗时），线程会继续运行并访问 `g_state`/`g_cb`，而 `Stop` 之后 `wWinMain` 立即 `TrayUi::Cleanup()` + 返回——**进程退出时线程可能仍在触碰已析构的状态，或被 TerminateThread 式的强杀截断**。
- **建议**：判断返回值，超时则记录并（a）不要 `CloseHandle` 泄漏一个句柄而不是制造悬空引用，或（b）用 `CancelIoEx` 打断挂起的 IO 后二次等待；至少在超时时不要继续后续清理。

### M7（中）命令入队不检查线程/连接状态，"成功"是假象 —— `device_manager.cpp:434-464` + `main.cpp:195-207`

`RequestSetSleep` / `RequestSetPollingRate` / `RequestSwitchDpiStage` 在锁内置标志后**无条件返回 true**。若 `Device::Stop()` 已调用（或设备未连接），命令永久滞留在队列里，UI 无从得知。菜单在未连接时会把项置灰（`main.cpp:122/127/143`），所以正常路径挡住了；但 `IDM_SLEEP_OFF`（123 行）用的是 `MF_STRING | (st.connected ? 0 : MF_GRAYED)`，同样置灰，安全。真正的暴露点是 `mchose_protocol` 侧：`SetSleep`/`SetDpiStage` 的失败只反映在 `g_lastCmdResult`，而 GUI 模式**从不读取 `TakeLastCommandResult`**（只有 `--set-rate` 用，`main.cpp:296`），所以用户在 GUI 里按下任何写命令都**得不到成功/失败反馈**。建议 GUI 也在 `WM_APP_STATE` 里消费一次结果并提示。

### M8（中）`--dump` / `--set-rate` 分支绕过单实例检查，且早于日志清理 —— `main.cpp:269-410` 与 `412-419`

- 诊断分支在互斥体（415）**之前**执行，因此 GUI 正在运行时再跑 `--dump` 会**同时打开同一组 HID 设备**（两套 `OpenControlDevice` + `OpenControlDeviceEx`，各自 `ReadCommand` 并写 EEPROM 型命令），可能互相抢响应（`ReadCommand` 的"回显校验 + 全零重试"会把对方的命令响应当成自己的滞后缓冲，导致诊断结果不可信）。
- 另外第 413 行的 `DeleteFileW(L"mchose-tray-gui.log")` 也在互斥体检查之前，**第二个实例会删掉正在运行的第一个实例的日志**（L6 已提，此处强调其与诊断模式叠加的后果）。
- **建议**：把互斥体获取移到 `wWinMain` 最前（诊断分支之前）；诊断模式下改用带后缀的互斥体名（如 `Local\mchose-tray-diag`）并立即失败退出（返回 3 表示"GUI 正在运行"），或对诊断模式也复用 `Local\mchose-tray-single` 但只做"检测不占用"（`OpenMutexW`）。

### L13（低）`--dump`/`--set-rate` 的输出文件路径是相对 CWD —— `main.cpp:320-321, 393-394`

`CreateFileW(L"mchose-tray-dump.txt", ...)` 与 `GuiLog` 的 `mchose-tray-gui.log`（`main.cpp:38`）都用相对路径。自启动（HKCU Run，`tray_ui.cpp:463-475`）时 CWD 通常是 `%SystemRoot%\System32` 或用户目录，写入可能失败或"文件找不到"。建议用 `GetModuleFileNameW` 取程序目录拼绝对路径。

### L14（低）`--set-rate` 的入参边界 —— `main.cpp:276-279, 283`

`_wtoi` 失败返回 0，`setRateHz > 0` 为假时**静默退回 `--dump` 语义**（`dump` 已被置真，277），用户以为在设置回报率，实际只是打印状态。建议参数非法时打印用法并返回非 0。

### L15（低）退出时未 `UnregisterClassW` / 未 `DestroyMenu` 残留 —— `main.cpp:440, 163`

进程级资源，退出即回收；列出以满足"每个 Create 是否都有对应 Destroy"的检查清单。

---

## 5. 协议层

### 5.1 `OpenControlDeviceEx` 枚举循环（`mchose_protocol.cpp:56-135`）

**正确性核查（逐条回答提问）**：

| 检查项 | 结论 | 依据 |
| :--- | :--- | :--- |
| 是否漏关句柄 | **基本不漏**。`matched && found == h` 时不关（保留为返回值，正确）；`matched && found != h`（被更优者替换）时在第 117 行先关掉旧 `found`，循环尾第 129 行的 `!matched \|\| found != h` 为真于是关掉当前 `h`（正确）；`!matched` 时第 129 行关掉 `h`（正确）。`detail` 在所有分支（80-83、88、94-99、130）都 `free` 了 | 116-131 |
| 是否可能返回已关闭句柄 | **不可能**。`found` 仅在 118 行被赋值，且赋值前（117 行）只关闭"被替换的旧值"；被替换时 `found` 立刻指向新 `h`，而新 `h` 的关闭条件是第 129 行的 `found != h`，此时为假，不会关 | 116-129 |
| 是否可能选中错误集合 | **是，这是本函数的主要缺陷**（见 M9） | 116-124 |

**M9（中）"最优集合"的选择标准不充分，且同分时后到者胜**

```cpp
115:                 /* 同页可能有多个集合，优先 Feature 报文最长的一个 */
116:                 if ((int)caps.FeatureReportByteLength >= bestFeatureLen) {
117:                     if (found != INVALID_HANDLE_VALUE) CloseHandle(found);
118:                     found = h;
119:                     bestFeatureLen = (int)caps.FeatureReportByteLength;
```

1. **`>=` 而非 `>`**：`bestFeatureLen` 初值为 0（68 行）。第一个匹配项必然入选（`len >= 0`）；此后**帧长相同的匹配项会顶掉前一个**，把已经打开且已验证的句柄关掉再换新的。当前拓扑（`docs/PROTOCOL.md:39-40`）里 `0xFF01/0x0001` 只有一个集合，结果稳定；但在有线模式或其他机型上若同名集合出现两次，选中结果就依赖总线枚举顺序（不稳定）。
2. **只比帧长，不校验帧长是否符合本协议**：本协议硬性要求 `0x11 → 21 字节`、`0x12 → 65 字节`（`docs/PROTOCOL.md:274-275`）。若某接口的 `0xFF01/0x0001` 集合帧长为 61（另一条 `0xFF0B` 通道的形态，见 `docs/PROTOCOL.md:40`）而它恰好更长/更晚出现，会被选中，随后 `ReadCommand` 按 21/65 收发就会持续失败。建议把选择条件改为**"帧长必须包含预期的 report 长度"**并加白名单：
   ```cpp
   if (caps.FeatureReportByteLength != kReport12Payload + 1) continue;   /* 必须 65 */
   ```
   或至少在一次试探性读（`ReadCommand(h, kReportControl, kCmdDeviceInfo, ...)`）成功后才认定为 `found`。
3. **`HidD_GetAttributes` 里没有按接口/集合区分（`MI_02`）**：只靠 VID/PID + UsagePage/Usage，同型号的鼠标接口与被仿真键盘接口若共享厂商页就会误判（当前 `MI_01` 是 `0x0001/0x0006`，安全）。
4. **`if (!matched || found != h) CloseHandle(h);` 的隐藏前提**：这段代码依赖"`found == h` ⟹ 不能关"的推理；一旦将来有人在 116-124 里加 `CloseHandle(h); ... continue;` 之类的早退，`found` 就变成悬空。建议改成显式所有权变量（`HANDLE owned = INVALID_HANDLE_VALUE; bool keep = false; ... if (!keep) CloseHandle(h);`）。

**L16（低）接口路径粗筛用 `strstr(path, "vid_5253")`，大小写与截断都敏感**

```cpp
86:         char path[1024];
87:         w2a(detail->DevicePath, path, sizeof(path));
88:         if (strstr(path, "vid_5253") == NULL) { free(detail); continue; }
```

- HID 接口路径里 VID 通常是小写 `vid_5253`（实测可用），但 `strstr` 区分大小写，若某个驱动/系统返回大写 `VID_5253` 就会**漏过设备**（表现为"未连接"，与 M9 的误判是相反的失败方向）。
- `w2a`（37-41）在 UTF-8 转换失败或被截断时不会报错，若路径超 1024 字节，`vid_5253` 段（在路径尾部）会被截掉而误判为不匹配。
- **建议**：改用 `wcsstr(detail->DevicePath, L"vid_5253")` 直接对宽字符做**大小写不敏感**比较，并跳过粗筛（反正后面还有 `HidD_GetAttributes` 的精确校验），粗筛只用于提前避免 `CreateFile`——可用 `HidD_GetAttributes` 前置不了，那就把路径缓冲改成动态分配（`needed` 已知）。

**L17（低）`pp` 泄漏路径**：`HidD_GetPreparsedData` 成功但 `HidP_GetCaps` 失败时，`HidD_FreePreparsedData(pp)`（126）其实仍在 `if (HidD_GetAttributes && ... && HidD_GetPreparsedData)` 块内，**能被执行到**，所以不泄漏——此处是正确的，但依赖 `HidP_GetCaps` 失败后 `pp` 仍有效的约定；建议把 `HidD_FreePreparsedData` 移到独立的 `if (pp) HidD_FreePreparsedData(pp);` 以保证任何早退路径都会释放。（同理 `attr.Size = sizeof(attr)`（102）正确。）

### 5.2 `ReadCommand` 的重试与校验 —— `mchose_protocol.cpp:144-211`

**M10（中）"解码后全零即无效"会把合法的全零状态判成读失败**

```cpp
189:         /* 解码后全零视为无效响应（该芯片首次读常返回上一命令的滞后缓冲，
190:          * 其形态正是「解码后全 0」），必须重试。 */
194:         bool anyNonZero = false;
195:         for (int i = 0; i < n; i++) {
196:             unsigned char v = (unsigned char)(res[2 + i] ^ 0xFF);
197:             if (v != 0x00) { anyNonZero = true; break; }
198:         }
199:         if (!anyNonZero) {
200:             if (verbose) printf("    [try %d] 解码后负载全零，重试\n", attempt + 1);
201:             continue;
202:         }
```

- **机理**：判据是"整个负载全零"，而**合法的设备状态本身可能就是全零**。最典型的是 `12 67` 的第 19 字节 sleep：关闭休眠时该字段就是 0（`ReadAllSettings:322`），但负载还有 DPI/回报率等非零字段，所以 `12 67` 安全。真正的风险在**其它读命令**：`11 04`（版本）在空固件串下、`11 03`（绑定信息）在未绑定状态下，负载有可能接近全零。
- **"永久失败"评估**：重试上限 4 次（164），每次最多约 40 ms（`Sleep(40)`，171）+ 30 ms（失败时 `Sleep(30)`，167），最坏 ≈ 280 ms 后返回 -1。上层 `RunPendingCommands`（`device_manager.cpp:148/174/178`）与周期读（321/348）会把 -1 记为 `readErrors++` 并在 `failStreak >= 3` 时**判定设备离线**（`device_manager.cpp:367`）。因此若设备处于某种合法但全零的状态，会**每 9 秒累积一次失败、最终误报"未连接"**——这是"永久失败"的真实形态。
- **严重级别**：中。
- **建议**：不要用"全零"作为唯一无效判据。更稳的判据是"命令回显正确 + 本次响应与上一次响应的原始字节不同"或"响应不是全 `0xFF`（即解码前全零）"。如果必须保留全零判据，至少对特定命令（如 `11 04`/`11 03`）放宽，并保证重试次数与 `failStreak` 阈值配合（例如全零判据不计入 `failStreak`）。

**核查（无越界的部分）**：

```cpp
191:         int n = payloadLen - 1;
192:         if (n > cap) n = cap;
...
196:             unsigned char v = (unsigned char)(res[2 + i] ^ 0xFF);
206:         for (int i = 0; i < n; i++) decoded[i] = (unsigned char)(res[2 + i] ^ 0xFF);
```

- `res`/`req` 都是 `unsigned char[1 + 64]`（158、173），合法下标 0..64。
- `payloadLen` ∈ {20, 64}（149-150）⟹ `n ≤ 63` ⟹ `res[2+i]` 最大 `res[64]`，**恰好在界内**；`HidD_GetFeature(h, res, payloadLen + 1)` 最大写 65 字节，也恰好填满。**无越界**。
- `decoded[i]` 受 `n ≤ cap` 保护，`ReadDeviceInfo` 等调用方传 `sizeof(d) = 64`，`n ≤ 63`，`d[0..62]` 在界内。**无越界**。
- 注意 `d[64]` 中 `d[63]` 永不写入，但所有读取都 ≤ `d[19]`（`ReadAllSettings`），无未初始化读取。

**L18（低）`payloadLen` 只由 `reportId` 决定，未知 reportId 一律按 64 处理**

```cpp
149:     const int payloadLen = (reportId == kReportControl) ? kReport11Payload
150:                                                          : kReport12Payload;
```

所有现有调用点只传 `0x11`/`0x12`（`mchose_protocol.cpp:247/265/282/299`），无问题；但若将来传入 `0x13`/`0x14`（`docs/PROTOCOL.md:48-49` 记载存在）会按 64 字节发送畸长报文。建议 `switch` + `default: return -1;`，同时用 `caps` 校验（见 M9 建议）。

### 5.3 `WriteCommand` 的长度校验与补齐 —— `mchose_protocol.cpp:213-240`

```cpp
217:     if (payload == NULL || payloadLen <= 0) return false;
219:     const int reportLen = (reportId == kReportControl) ? kReport11Payload : kReport12Payload;
221:     if (payloadLen > reportLen) return false;
224:     unsigned char order[64];
225:     memset(order, 0, sizeof(order));
226:     memcpy(order, payload, payloadLen);
```

- **核查结论：长度校验和拷贝边界都正确**。`payloadLen ≤ reportLen ≤ 64 = sizeof(order)`，`memcpy` 不越界；`req[1 + 64]` 与循环 `i < reportLen ≤ 64` 匹配，`req[64]` 是最后一个合法下标，`HidD_SetFeature(h, req, reportLen + 1)` 最大 65 字节恰好填满。**无越界**。
- **补齐语义正确**：`order` 先全 0，未写入部分保持 0，发送时整体 `^ 0xFF` 变成 `0xFF`——与 `docs/PROTOCOL.md:62` 的"补齐位 = 命令表 0x00 取反 = 0xFF"完全一致。这一点作者做对了，且 `ReadCommand:154-160` 用同一套补齐，读写对称。
- **缺陷（低）**：与 `ReadCommand` 同样只按 `reportId` 判定长度，缺 `default` 分支；且**没有校验单个 report 的实际容量**（`caps.FeatureReportByteLength` 在 `OpenControlDeviceEx` 里算出来后被丢弃了）。协议文档也承认这是"经验值"（`docs/PROTOCOL.md:274-275` 明确 `HidD_GetFeature` 用 21 字节而集合报 65）。建议把 `caps` 的 `FeatureReportByteLength` 从 `OpenControlDeviceEx` 通过出参带出并做一次一致性断言。

### 5.4 其它协议层数据校验

**核查（结论：安全，但属"信任设备"）**：

- `ReadVersion:268-275`：`len = d[0]` 后同时被 `n - 1` 和 `outLen - 1` 双向夹紧，`out[len] = '\0'` 不会越界。**正确**。
- `ReadConnInfo:248` / `ReadDeviceInfo:283` / `ReadAllSettings:300` 的最小长度检查与后续读取下标一致（最小 7 / 11 / 20，最大访问下标 6 / 10 / 19）。**正确且必要**。
- `rdU32`（31-35）：`(unsigned)p[3] << 24`——`unsigned` 在 Windows 上是 32 位，左移到最高位不产生 UB（值被回绕）。若移植到 `unsigned` 为 16 位的平台会 UB，但目标平台固定，**可接受**。
- `RateIndexToHz`（392-398）/ `HzToRateIndex`（400-410）：越界返回 0 / -1，语义清晰；`SetPollingRate:344-345` 对 `idx < 0` 做了拒绝。**正确**。
- **L19（低）** `SetDpiStage:379-380` 把 `usbDpiIndex` 与 `gDpiIndex` 同时写成 `stageIndex`：
  ```cpp
  379:     p[1] = (unsigned char)stageIndex;   /* usbDpiIndex */
  380:     p[2] = (unsigned char)stageIndex;   /* gDpiIndex（同时更新无线档位索引） */
  ```
  这与 `docs/PROTOCOL.md:229-232` 的实验 B（i1→byte2 低、i2→byte1 低）一致，但**回写时把"另一条通道的档位索引"也一并改了**。若设备在 2.4G 与有线两条通道下分别维护档位，一次"切档"会同时改动两边。建议把 `cur.usbDpiIndex` / `cur.gDpiIndex` 原样带回，只改目标那一个（是否安全取决于设备行为，属需要实测确认的设计选择）。
- **L20（低）** `SetDpiStage` 回写的 `p[16] = cur.dpiSum`（386）直接沿用回读的校验和。如果档值未变这是对的；一旦设备对 `dpiSum` 有新算法，会写入不一致的校验和。建议在本地重算或明确注释"仅当档值未变时成立"。
- **L21（低）** `ReadAllSettings` 的 `d[3]` → `reserved`（316）与 `d[16..19]`（319-322）都只受 `n >= 20` 保护；`d[16]/d[17]/d[18]/d[19]` 恰好卡在边界，未来若把 `n` 阈值改成 16 就会越界读。建议用 `static_assert` 或注释固化"偏移 19 需要长度 ≥ 20"。

---

## 6. 其它未定义行为 / 越界 / 整数截断

| 编号 | 位置 | 问题 | 触发条件 | 级别 | 建议 |
| :--- | :--- | :--- | :--- | :--- | :--- |
| UB1 | `tray_ui.cpp:295-301` | `int a = g_osdAlpha - 18;` 当 238 递减时最后一次为 `238 - 18*14 = -14`（负），进入 `else` 分支调用 `OsdSetAlpha(-14)`，其中 `SetLayeredWindowAttributes(..., (BYTE)alpha, ...)`（275）把 -14 截断为 **242**（接近全不透明），于是"淡出"最后一帧突然变亮 | 每次 OSD 淡出（238 不是 18 的整数倍） | 中 | 先夹紧再转换：`OsdSetAlpha(a < 0 ? 0 : a)`，或让步长整除初值 |
| UB2 | `tray_ui.cpp:267-280` | `OsdSetAlpha(0)` 走 `alpha <= 0` 早退分支，**不更新 `g_osdAlpha`**（270 行在 return 之前？——不，270 在 return 之前，此处正确）。但 `OsdProc` 的 `a <= 0` 分支先 `KillTimer` 再 `OsdSetAlpha(0)`，于是 `g_osdAlpha` 保持 0，`ShowOsd`（433）重新置 238。**无缺陷**，列出以确认已核对 | — | — | — |
| UB3 | `main.cpp:283-284` | `Device::Start(NULL, NULL)` 之后立刻进入等待循环，但 `Start` 失败时（返回 false，477 行有处理，此处 284 行**未检查返回值**）会空等 10 秒读不到数据 | `CreateThread`/`CreateEventW` 失败 | 低 | 检查返回值，失败时直接返回非 0 |
| UB4 | `main.cpp:290-291` | `--set-rate` 路径把 `RateIndexToHz` 之外的整型 `setRateHz` 直接透传；若传入非 `kRates` 值，`HzToRateIndex` 返回 -1，`SetPollingRate` 返回 false，`RunPendingCommands` 记 `ok=false` → 结果 1（正确失败），**无 UB** | — | — | — |
| UB5 | `main.cpp:160-161` | `int cmd = (int)TrackPopupMenu(...)`：返回值是 `UINT`（`TPM_RETURNCMD`），转 `int` 后与 1300+ 比较。ID 都很小，**无截断问题**；但 `(int)` 转换在 ID ≥ 0x80000000 时会是负值，建议改为 `UINT cmd` 并对齐比较类型 | — | 低 | `UINT cmd` + `cmd == IDM_EXIT` 等按 `UINT` 比较 |
| UB6 | `main.cpp:96-100` | `swprintf(header, 192, ...)` 的"192"是硬编码字面量而非 `sizeof(header)/sizeof(wchar_t)`，与 `header[192]` 分散在两处；`--dump` 分支的 `snprintf(out + off, sizeof(out) - off, ...)` 则是混合风格。建议统一用 `_countof` / `ARRAYSIZE` | — | 低 | 统一用 `ARRAYSIZE(header)` |
| UB7 | `mchose_protocol.cpp:26-35` | `rdU16`/`rdU32` 假定指针至少 2/4 字节有效，调用方均保证（`d` 为 64 字节数组）。**无缺陷**，已核对 | — | — | — |
| UB8 | `mchose_protocol.cpp:87` | `w2a(detail->DevicePath, path, sizeof(path))` 的 `size` 参数是 `int`，`sizeof(path)` 为 `size_t` → 隐式收窄（值 1024，实际安全）。建议显式 `(int)sizeof(path)` | — | 低 | 显式转换 |

---

## 7. 修复优先级建议

1. **立刻修（会写坏设备/越界）**
   - H1 菜单 ID 区间重叠：`main.cpp:199`（连同 31 行的宏分区）——**每次点 DPI 都在把休眠时间改成 99–104 分钟并写入 EEPROM**。
   - H2 `sizeof(out) - off` 下溢：`main.cpp:303-390`。
   - H3 `GuiLog` 用 `snprintf` 返回值为写入长度：`main.cpp:42-44`。
2. **下一轮修（竞态/功能失效）**
   - M1（`CreateCompatibleDC` 判空 + `hbm` 泄漏）、M2（图标销毁顺序）、M4（`TaskbarCreated` 重挂）、M5（"立即刷新"无效）、M6（`Stop` 超时处理）、M9（集合选择标准）、M10（全零判据导致误判离线）、UB1（淡出末帧变亮）。
3. **清理型**：M3/M7/M8/L1–L21。

### 已核对但**确认没有问题**的项（避免重复排查）

- `BuildIconFromDib` 的 alpha 后处理循环**不越界**（32bpp 行距 = `size*4`，`size ∈ [16,64]`，见 L3）。
- `ReadCommand` 的 `res[2+i]` / `decoded[i]` 在 20/64 两种长度下**均在界内**（5.2）。
- `WriteCommand` 的长度校验与 `memcpy` **无越界**，补齐语义与协议文档一致（5.3）。
- `OpenControlDeviceEx` **不会返回已关闭的句柄，也不漏关句柄**（5.1 表格）。
- `ReadVersion` / `ReadConnInfo` / `ReadDeviceInfo` / `ReadAllSettings` 的**长度夹紧与最小长度检查正确**。
- `UpdateTooltip` / `ShowOsd` 的 `swprintf` 尺寸参数与实际缓冲**匹配**（L8/L9）。
- `g_wmTaskbarCreated` 的**注册时机与判断位置正确**（4.1）。
- `OsdPaint` 的 `SelectObject` 恢复与删除顺序**正确**（L1）。

---

*审查方法：逐行阅读全部五个文件 + `device_manager.*` 与 `docs/PROTOCOL.md` 交叉验证；菜单 ID 推演基于 `main.cpp:22-31` 的宏定义与 `104-155` 的构造点、`165-207` 的分发点逐步求值。未执行构建，未修改任何被审文件；本报告写在 `docs/` 下作为新增文档。*
