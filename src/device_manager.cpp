/*
 * device_manager.cpp — 后台 HID 工作线程与状态机实现
 */

#include "device_manager.h"

#include <stdio.h>
#include <string.h>

namespace Device {

namespace {

CRITICAL_SECTION    g_cs;
bool                g_csReady = false;
State               g_state;
StateCallback       g_cb = NULL;
HANDLE              g_hStop = NULL;
HANDLE              g_hThread = NULL;

/* 挂起命令（由 UI 线程写入，工作线程消费） */
bool g_pendRate = false;   int  g_pendRateHz = 0;
bool g_pendSleep = false;  bool g_pendSleepEnable = false; int g_pendSleepMin = 0;
bool g_pendDpi = false;    int  g_pendDpiStage = -1;
bool g_pendRefresh = false;
int  g_lastCmdResult = CMD_RESULT_NONE;

void Lock()   { EnterCriticalSection(&g_cs); }
void Unlock() { LeaveCriticalSection(&g_cs); }

/* 临界区必须早于任何 GetState()/Request* 调用可用，因此用静态对象在
 * 进程启动时（wWinMain 之前）就初始化，而不是等到 Start()。 */
struct CriticalSectionInit {
    CriticalSectionInit()
    {
        InitializeCriticalSection(&g_cs);
        g_csReady = true;
    }
};
CriticalSectionInit g_csInit;

/* 更新快照并通知（在锁外回调，避免回调里再取状态造成死锁） */
void Publish(DWORD mask)
{
    State snap;
    StateCallback cb = NULL;
    Lock();
    if (mask != 0) {
        g_state.totalPublishes++;
        if ((mask & CHANGE_SETTINGS) != 0) g_state.settingsPublishes++;
    }
    snap = g_state;
    cb = g_cb;                 /* 回调指针也在锁内读取，避免与 Stop() 竞争 */
    Unlock();

    if (mask != 0 && cb != NULL) cb(snap, mask);
}

/* 解析机型库并把有效参数写进状态。
 *
 * 优先级由 McHose::ResolveModel 决定：设备自报型号名 > VID/PID 精确匹配 > VID 通配。
 * 之所以这样排：PID 会跨机型复用（0x1020 在 A7 Pro 是有线、在 L7 Pro 是无线），
 * 只看 PID 会把机型认错，进而选错回报率档数与 DPI 上限。
 */
static void ApplyModelCaps(State &st, unsigned short vid, unsigned short pid)
{
    const McHose::ModelSpec *spec = McHose::ResolveModel(st.modelName, vid, pid);
    McHose::ModelCaps caps = McHose::CapsFor(spec, st.modelName);

    st.modelDpiMax     = caps.dpiMax;
    st.modelDpiStages  = caps.dpiStages;
    st.modelKnown      = caps.known;
    st.modelSource     = caps.source;
    if (spec != NULL && spec->name != NULL) {
        wcsncpy(st.modelProfile, spec->name, 47);
        st.modelProfile[47] = L'\0';
    } else {
        st.modelProfile[0] = L'\0';
    }
    st.rateCount = McHose::RateCountForMode(st.connectMode, caps);
}
/* 解析设备主动推送的 report 0x13。
 * 实测（A7 Pro）：payload[0]=0xE2，[3]=充电，[4]=电量，[9..]=型号名。
 * 参考社区对同方案 L7 Pro 的实测，其子类型为 0x1D —— 两者都接受。 */
bool ParsePush(const unsigned char *buf, int len, State &st, bool &nameChanged)
{
    if (len < 12) return false;
    if (buf[0] != 0x13) return false;

    unsigned char p[64];
    int n = len - 1;
    if (n > (int)sizeof(p)) n = (int)sizeof(p);
    for (int i = 0; i < n; i++) p[i] = (unsigned char)(buf[1 + i] ^ 0xFF);

    if (p[0] != 0xE2 && p[0] != 0x1D) return false;
    /* 电量合理性：>100 直接拒；0% 且未充电也拒（关机时的"看似真实"的 0%） */
    if (!McHose::IsPlausibleBattery(p[4], p[3])) return false;

    st.pushCount++;                        /* 统计实际收到的有效推送条数 */

    bool changed = false;
    if (!st.batteryValid || st.battery != (int)p[4]) { st.battery = p[4]; changed = true; }
    st.batteryValid = true;
    st.lastBatteryTick = GetTickCount();     /* 记录电量新鲜度 */

    bool charging = (p[3] != 0);
    if (st.charging != charging) { st.charging = charging; changed = true; }

    /* 型号名（ASCII，以 0 结束） */
    nameChanged = false;
    if (p[9] >= 32 && p[9] < 127) {
        wchar_t name[64];
        int k = 0;
        for (int i = 9; i < n && k < (int)(sizeof(name) / sizeof(name[0])) - 1; i++) {
            unsigned char c = p[i];
            if (c == 0) break;
            name[k++] = (wchar_t)((c >= 32 && c < 127) ? c : L'?');
        }
        name[k] = L'\0';
        if (k > 0 && wcscmp(name, st.modelName) != 0) {
            /* 型号名变了要重新解析机型：名字是比 PID 更可靠的判据 */
            ApplyModelCaps(st, st.ifaceVid, st.ifacePid);
            wcsncpy(st.modelName, name, (sizeof(st.modelName) / sizeof(wchar_t)) - 1);
            st.modelName[(sizeof(st.modelName) / sizeof(wchar_t)) - 1] = L'\0';
            nameChanged = true;
        }
    }
    return changed;
}

/*
 * ---------------------------------------------------------------- 推送读取
 *
 * 这里有两个必须同时满足的约束，历史上分别踩过：
 *
 * (1) 不能放弃已发出的 IRP。旧实现超时后 CancelIo 就直接返回，
 *     迟到的完成会把 IOSB 写进已经失效的**栈上** OVERLAPPED（UB），
 *     并留下陈旧的事件信号，使下一次等待被提前唤醒、
 *     GetOverlappedResult(bWait=FALSE) 因 ERROR_IO_INCOMPLETE 失败，
 *     于是把好好的推送句柄关掉。
 *      → 修复：OVERLAPPED 常驻，CancelIo 后用 GetOverlappedResult(bWait=TRUE) 收割。
 *
 * (2) 不能让一个输入读**长期挂起**。实测教训：若把待决的输入读一直留在队列里，
 *     本设备的命令通道会被堵死，特性读（11 06 / 12 67）全部拿不到数据。
 *      → 修复：每次 Poll 都是"发起一次读，等到超时；超时就取消并收割"。
 */
struct PushReader {
    HANDLE        dev = INVALID_HANDLE_VALUE;
    HANDLE        ev  = NULL;
    OVERLAPPED    ov;
    unsigned char buf[128];
    int           lastLen = 0;

    /* 换设备：先收割在途 IRP，再记账 */
    void Reset(HANDLE device)
    {
        if (dev != INVALID_HANDLE_VALUE && dev != NULL) {
            /* 无条件收割：即使本次调用没有在途 IRP，这一步也无害 */
            CancelIo(dev);
            DWORD dummy = 0;
            GetOverlappedResult(dev, &ov, &dummy, TRUE);
        }
        dev = device;
        lastLen = 0;
    }

    /* 关闭设备句柄：必须先收割 IRP 再 CloseHandle，否则迟到的完成会写到已失效对象 */
    void CloseDevice()
    {
        if (dev != INVALID_HANDLE_VALUE && dev != NULL) {
            Reset(INVALID_HANDLE_VALUE);
            CloseHandle(dev);
        }
        dev = INVALID_HANDLE_VALUE;
        lastLen = 0;
    }

    /* 返回：>0 收到字节数；0 本次无数据；-1 出错，调用方应重开。
     * 每次调用都是"发起一次读 → 等到超时 → 取消并收割"，绝不留下挂起的 IRP。 */
    int Poll(DWORD timeoutMs)
    {
        if (dev == INVALID_HANDLE_VALUE || dev == NULL || ev == NULL) return -1;

        memset(&ov, 0, sizeof(ov));
        ov.hEvent = ev;
        ResetEvent(ev);

        DWORD got = 0;
        BOOL ok = ReadFile(dev, buf, sizeof(buf), &got, &ov);
        if (!ok) {
            if (GetLastError() != ERROR_IO_PENDING) return -1;

            DWORD w = WaitForSingleObject(ev, timeoutMs);
            if (w == WAIT_TIMEOUT) {
                CancelIo(dev);
                /* bWait = TRUE：阻塞到 IRP 真正结束才返回，收割干净。
                 * 这正是旧实现缺失的一步。 */
                DWORD dummy = 0;
                GetOverlappedResult(dev, &ov, &dummy, TRUE);
                return 0;
            }
            if (w != WAIT_OBJECT_0) return -1;
            if (!GetOverlappedResult(dev, &ov, &got, FALSE)) return -1;
        }

        lastLen = (int)got;
        return lastLen;
    }

    int CopyOut(unsigned char *dst, int cap) const
    {
        int n = (lastLen < cap) ? lastLen : cap;
        if (n > 0) memcpy(dst, buf, (size_t)n);
        return n;
    }
};

/*
 * 写命令之后的"可信回读"。
 *
 * 该芯片的特性读会返回上一命令的滞后缓冲，而写命令之后立刻读尤其容易读到
 * 写入前的旧设置（实测：写完 125Hz 后马上读，读到的仍是写入前的档位）。
 * 因此这里：先给设备一点稳定时间，丢掉一次读数（冲掉滞后缓冲），再读第二次作为判据。
 */
bool ReadAllSettingsSettled(HANDLE h, McHose::AllSettings &out)
{
    Sleep(150);                        /* 等设备把设置落到 RAM */
    McHose::AllSettings scratch;
    McHose::ReadAllSettings(h, scratch, 0);   /* 丢弃：可能命中滞后缓冲 */
    return McHose::ReadAllSettings(h, out, 0);
}

/* 执行挂起的命令；返回是否处理过命令，并设置 g_lastCmdResult。
 * refreshRequested 输出"用户要求立即刷新"，由调用方把两个周期计时器清零以立刻重读。 */
bool RunPendingCommands(HANDLE hFeature, State &st, bool &refreshRequested)
{
    bool handled = false;
    refreshRequested = false;

    bool doRate = false; int rateHz = 0;
    bool doSleep = false; bool sleepEnable = false; int sleepMin = 0;
    bool doDpi = false; int dpiStage = -1;
    bool doRefresh = false;

    Lock();
    if (g_pendRate)    { doRate = true;    rateHz = g_pendRateHz; g_pendRate = false; }
    if (g_pendSleep)   { doSleep = true;   sleepEnable = g_pendSleepEnable; sleepMin = g_pendSleepMin; g_pendSleep = false; }
    if (g_pendDpi)     { doDpi = true;     dpiStage = g_pendDpiStage; g_pendDpi = false; }
    if (g_pendRefresh) { doRefresh = true; g_pendRefresh = false; }
    Unlock();

    if (doRate) {
        handled = true;
        bool ok = McHose::SetPollingRate(hFeature, rateHz, st.rateCount, 0);
        int idx = McHose::HzToRateIndex(rateHz, st.rateCount);
        if (ok) {
            /* 写后回读校验：只有 0x41 实际落点的字段与请求一致才算成功 */
            McHose::AllSettings s;
            if (ReadAllSettingsSettled(hFeature, s)) {
                ok = (s.usbRateIndex == idx);
                if (ok) {
                    st.writeRateIndex = (int)s.usbRateIndex;
                    st.settingsValid = true;
                    /* 不做乐观覆盖：活动档位字段由设备在自己同步时才更新，
                     * 界面一律显示实读值，避免显示尚未生效的数字。 */
                }
            } else {
                ok = false;
            }
        }
        Lock(); g_lastCmdResult = ok ? CMD_RESULT_OK_VERIFIED : CMD_RESULT_FAILED; Unlock();
    }

    if (doSleep) {
        handled = true;
        bool sent = McHose::SetSleep(hFeature, sleepEnable, sleepMin, 0);
        int result = CMD_RESULT_FAILED;
        if (sent) {
            result = CMD_RESULT_SENT_UNVERIFIED;
            if (sleepEnable) {
                /* 启用休眠时可以做回读校验：比对 12 67 的 sleep 分钟字段 */
                McHose::AllSettings s;
                if (ReadAllSettingsSettled(hFeature, s) && (int)s.sleep == sleepMin)
                    result = CMD_RESULT_OK_VERIFIED;
            }
            /* 关闭休眠时无法从该字段区分"关闭"与"保留上次分钟数"，故只报已下发 */
        }
        Lock(); g_lastCmdResult = result; Unlock();
        if (sent) Publish(CHANGE_SETTINGS);
    }

    if (doDpi) {
        handled = true;
        int result = CMD_RESULT_FAILED;
        McHose::AllSettings cur;
        if (McHose::ReadAllSettings(hFeature, cur, 0)) {
            if (McHose::SetDpiStage(hFeature, cur, dpiStage, 0)) {
                /*
                 * 写已下发，默认只报"已下发"。只有回读成功**且**索引确实变了才升级为"校验通过"。
                 *
                 * 原实现写成 `if (ok) { if (ReadAllSettingsSettled(...)) ok = (...) }`——
                 * 回读失败时 ok 仍为 true，于是没做任何校验却报"成功"。
                 *
                 * 判据用"任一索引字段匹配"：0x40 直接改写 usbDpiIndex，而 gDpiIndex
                 * （活动档位）实测会滞后一段时间才跟随，逐字节比对会误报失败。
                 */
                result = CMD_RESULT_SENT_UNVERIFIED;
                McHose::AllSettings after;
                if (ReadAllSettingsSettled(hFeature, after)) {
                    if ((int)after.usbDpiIndex == dpiStage || (int)after.gDpiIndex == dpiStage)
                        result = CMD_RESULT_OK_VERIFIED;
                }
            }
        }
        Lock(); g_lastCmdResult = result; Unlock();
        if (result != CMD_RESULT_FAILED) Publish(CHANGE_SETTINGS);
    }

    if (doRefresh) {
        /* 之前这里只把请求消费掉就丢弃，导致「立即刷新」与单击托盘图标
         * 都不会触发主动读，界面最长陈旧 9 秒。现在把刷新意图交回调用方。 */
        handled = true;
        refreshRequested = true;
    }
    return handled;
}

DWORD WINAPI WorkerThread(LPVOID param)
{
    /* 停止事件通过线程参数传入：线程内持有稳定副本。
     * 曾经直接读全局 g_hStop，一旦 Stop() 超时后把它置空，
     * WaitForSingleObject(NULL) 返回 WAIT_FAILED ≠ WAIT_OBJECT_0，break 永不触发，
     * 线程永久失控并继续独占 HID。 */
    HANDLE hStop = (HANDLE)param;

    HANDLE hFeature = INVALID_HANDLE_VALUE;
    PushReader push;
    push.ev = CreateEventW(NULL, TRUE, FALSE, NULL);

    DWORD lastInfoTick = 0;
    DWORD lastSetTick  = 0;
    DWORD lastPushOpenTick = 0;
    DWORD lastLinkTick = 0;        /* 11 03 链路状态读取节拍 */
    DWORD pushReopenBackoffMs = 2000;   /* 推送句柄重开退避 */
    DWORD offlineBackoffMs = 0;         /* 判离线后的退避，避免高频抖动 */
    int   failStreak   = 0;

    for (;;) {
        if (WaitForSingleObject(hStop, 0) == WAIT_OBJECT_0) break;

        /* 判离线后必须退避再重试。否则"句柄能打开但读不通"会形成
         * 打开→失败→判离线→立刻重开 的高频循环，导致 connected 反复翻转、通知风暴。 */
        if (offlineBackoffMs > 0) {
            if (WaitForSingleObject(hStop, offlineBackoffMs) == WAIT_OBJECT_0) break;
        }

        /* ---- 未打开：尝试打开 ---- */
        if (hFeature == INVALID_HANDLE_VALUE) {
            hFeature = McHose::OpenControlDevice(NULL, 0);

        /*
         * 一打开设备就把接口 VID/PID 与机型解析落到状态里。
         * 不能只在 11 06 读取成功时做——鼠标休眠时 11 06 会失败（返回全零），
         * 那样接口 ID 与机型信息就会一直是空的，诊断输出看起来像"设备不存在"。
         */
        if (hFeature != INVALID_HANDLE_VALUE) {
            unsigned short iv = 0, ip = 0;
            Lock();
            if (McHose::GetLastOpenedInterfaceIds(&iv, &ip)) {
                g_state.ifaceVid = iv;
                g_state.ifacePid = ip;
            }
            ApplyModelCaps(g_state, g_state.ifaceVid, g_state.ifacePid);
            Unlock();
        }
            if (hFeature != INVALID_HANDLE_VALUE && push.ev != NULL)
                push.Reset(McHose::OpenControlDeviceEx(true, NULL, 0));

            if (hFeature == INVALID_HANDLE_VALUE) {
                Lock();
                bool was = g_state.connected;
                g_state.connected = false;
                g_state.settingsValid = false;
                g_state.pushChannelOpen = false;
                Unlock();
                if (was) Publish(CHANGE_CONNECTED | CHANGE_BATTERY);
                if (WaitForSingleObject(hStop, 1500) == WAIT_OBJECT_0) break;
                continue;
            }

            /* 新连接：清空统计，稍后立即刷新 */
            Lock();
            g_state.connected = true;
            g_state.readErrors = 0;
            g_state.pushCount = 0;
            g_state.pushReads = 0;
            g_state.pushChannelOpen = (push.dev != INVALID_HANDLE_VALUE);
            Unlock();
            failStreak = 0;
            offlineBackoffMs = 0;
            pushReopenBackoffMs = 2000;
            lastInfoTick = 0;
            lastSetTick = 0;

            /* 型号名：直接读 HID 产品字符串（"MCHOSE A7 Pro"），去掉品牌前缀。
             * 这比依赖设备主动推送可靠得多。 */
            {
                wchar_t prod[128] = {0};
                if (McHose::GetProductString(hFeature, prod, 128)) {
                    const wchar_t *name = prod;
                    if (wcsncmp(name, L"MCHOSE ", 7) == 0) name += 7;
                    Lock();
                    wcsncpy(g_state.modelName, name,
                            (sizeof(g_state.modelName) / sizeof(wchar_t)) - 1);
                    g_state.modelName[(sizeof(g_state.modelName) / sizeof(wchar_t)) - 1] = L'\0';
                    Unlock();
                }
            }

            /* 首次读取设备信息（含电量），失败则依赖推送 */
            Sleep(60);
            McHose::DeviceInfo info;
            if (McHose::ReadDeviceInfo(hFeature, info, 0)) {
                Lock();
                g_state.battery = info.batteryLevel;
                g_state.batteryValid = true;
                g_state.charging = (info.chargeStatus != 0);
                g_state.connectMode = info.connectMode;
                g_state.deviceVid = info.vid;
                g_state.devicePid = info.pid;
                if (McHose::GetLastOpenedInterfaceIds(&g_state.ifaceVid, &g_state.ifacePid))
                    ApplyModelCaps(g_state, g_state.ifaceVid, g_state.ifacePid);
                Unlock();
            }
            char ver[32] = {0};
            if (McHose::ReadVersion(hFeature, ver, sizeof(ver), 0)) {
                Lock();
                for (int i = 0; i < 31 && (ver[i] != '\0' || i == 0); i++) {
                    g_state.firmware[i] = (wchar_t)(unsigned char)ver[i];
                    if (ver[i] == '\0') break;
                    g_state.firmware[i + 1] = L'\0';
                }
                Unlock();
            }
            Publish(CHANGE_CONNECTED | CHANGE_BATTERY | CHANGE_VERSION);
        }

        /* ---- 消费挂起命令 ---- */
        {
            State snap;
            Lock(); snap = g_state; Unlock();
            bool refresh = false;
            if (RunPendingCommands(hFeature, snap, refresh)) {
                Lock();
                g_state.rateIndex = snap.rateIndex;
                g_state.rateHz = snap.rateHz;
                g_state.writeRateIndex = snap.writeRateIndex;
                g_state.settingsValid = snap.settingsValid;
                Unlock();
                if (refresh) {
                    /* 清零周期计时器 → 本轮立刻重读设备信息与设置 */
                    lastInfoTick = 0;
                    lastSetTick = 0;
                }
                Publish(CHANGE_SETTINGS);
            }
        }

        /* ---- 被动监听推送报文（短超时，兼作循环节拍）---- */
        if (push.dev != INVALID_HANDLE_VALUE) {
            int got = push.Poll(300);
            if (got > 0) {
                unsigned char buf[128];
                int n = push.CopyOut(buf, sizeof(buf));
                bool nameChanged = false;
                Lock();
                bool changed = ParsePush(buf, n, g_state, nameChanged);
                g_state.pushReads++;
                g_state.lastPushLen = (n < 24) ? n : 24;
                if (g_state.lastPushLen > 0)
                    memcpy(g_state.lastPush, buf, (size_t)g_state.lastPushLen);
                /* 能收到推送即证明设备在线，必须清零失败计数 */
                if (changed || nameChanged || n >= 12) failStreak = 0;
                Unlock();
                if (nameChanged) {
                    /* 型号名是比 PID 更可靠的判据：拿到名字后重新解析机型并落盘 */
                    Lock();
                    ApplyModelCaps(g_state, g_state.ifaceVid, g_state.ifacePid);
                    Unlock();
                }
                if (changed || nameChanged)
                    Publish((changed ? (DWORD)CHANGE_BATTERY : 0) |
                            (nameChanged ? (DWORD)CHANGE_VERSION : 0));
            } else if (got < 0) {
                push.CloseDevice();          /* 内部先收割 IRP 再关闭 */
                Lock(); g_state.pushChannelOpen = false; Unlock();
            }
        } else {
            /* 推送句柄失效后限速重开（退避递增，避免半失效状态下每 2 秒全量枚举 SetupDi） */
            DWORD nowTick = GetTickCount();
            if (hFeature != INVALID_HANDLE_VALUE && push.ev != NULL &&
                nowTick - lastPushOpenTick >= pushReopenBackoffMs) {
                lastPushOpenTick = nowTick;
                push.Reset(McHose::OpenControlDeviceEx(true, NULL, 0));
                Lock(); g_state.pushChannelOpen = (push.dev != INVALID_HANDLE_VALUE); Unlock();
                if (push.dev == INVALID_HANDLE_VALUE) {
                    pushReopenBackoffMs = (pushReopenBackoffMs < 30000)
                                        ? pushReopenBackoffMs * 2 : 30000;
                } else {
                    pushReopenBackoffMs = 2000;
                }
            }
            if (WaitForSingleObject(hStop, 200) == WAIT_OBJECT_0) break;
        }

        /* ---- 链路状态：问接收器（11 03）----
         * 关键区分：11 03 由**接收器本机**应答，鼠标休眠/关掉时依然有效；
         * 而 11 04 / 11 06 / 12 67 需要**鼠标本体**应答，鼠标睡了只会返回全零。
         * 不区分这两者就会出现"已连接，电量 0%"这种误导显示。
         *
         * 性能注意：这一段**必须按节拍读**，不能每轮循环都读。
         * 循环节拍约 300ms，若每轮都发一次 11 03 事务（含 40ms 等待与最多 4 次重试），
         * 空闲 CPU 会从 0.01% 级被抬到 0.15% 级、且每秒往设备上打 3 次多余报文。 */
        DWORD now = GetTickCount();
        bool loopFailed = false;
        bool linked;

        Lock();
        linked = g_state.mouseLinked;      /* 默认沿用上次结论 */
        Unlock();

        if (lastLinkTick == 0 || now - lastLinkTick >= 3000) {
            lastLinkTick = now;
            McHose::ConnInfo ci;
            if (McHose::ReadConnInfo(hFeature, ci, 0)) {
                bool newLinked = (ci.connect != 0);
                DWORD mask = 0;
                Lock();
                if (g_state.mouseLinked != newLinked) {
                    g_state.mouseLinked = newLinked;
                    mask |= CHANGE_CONNECTED;
                }
                if (!newLinked) {
                    /* 鼠标不在链路：清掉会显示成 0 的过期数据 */
                    if (g_state.batteryValid) { g_state.batteryValid = false; mask |= CHANGE_BATTERY; }
                    g_state.settingsValid = false;
                }
                Unlock();
                linked = newLinked;
                if (mask) Publish(mask);
            }
        }
        /*
         * 电量新鲜度检查：鼠标关机后接收器依然报 connect=1，
         * 若不设超时，界面会继续显示最后一次读到的电量（陈旧值）。
         * 阈值取 15 秒（周期读 3 秒 + 推送通常 2 秒级，15 秒静默即确定读不到）。
         */
        {
            bool stale = false;
            Lock();
            if (g_state.batteryValid && g_state.lastBatteryTick != 0 &&
                now - g_state.lastBatteryTick > 15000) {
                g_state.batteryValid = false;
                stale = true;
            }
            Unlock();
            if (stale) Publish(CHANGE_BATTERY);
        }

        if (lastInfoTick == 0 || now - lastInfoTick >= 3000) {
            lastInfoTick = now;
            if (!linked) {
                /* 鼠标休眠：这不是"读失败"，不应累加离线判定 */
            } else {
                McHose::DeviceInfo info;
                if (McHose::ReadDeviceInfo(hFeature, info, 0) &&
                    McHose::IsPlausibleBattery(info.batteryLevel, info.chargeStatus)) {
                    DWORD mask = 0;
                    Lock();
                    if (g_state.battery != (int)info.batteryLevel || !g_state.batteryValid) {
                        g_state.battery = info.batteryLevel; mask |= CHANGE_BATTERY;
                    }
                    g_state.batteryValid = true;
                    g_state.lastBatteryTick = GetTickCount();
                    bool ch = (info.chargeStatus != 0);
                    if (g_state.charging != ch) { g_state.charging = ch; mask |= CHANGE_BATTERY; }
                    if (g_state.connectMode != info.connectMode) {
                        g_state.connectMode = info.connectMode;
                        if (McHose::GetLastOpenedInterfaceIds(&g_state.ifaceVid, &g_state.ifacePid))
                    ApplyModelCaps(g_state, g_state.ifaceVid, g_state.ifacePid);
                        mask |= CHANGE_CONNECTED;
                    }
                    g_state.deviceVid = info.vid;
                g_state.devicePid = info.pid;
                    Unlock();
                    if (mask) Publish(mask);
                } else {
                    loopFailed = true;
                    Lock(); g_state.readErrors++; Unlock();
                }
            }
        }

        if ((lastSetTick == 0 || now - lastSetTick >= 9000) && linked) {
            lastSetTick = now;
            McHose::AllSettings s;
            if (McHose::ReadAllSettings(hFeature, s, 0)) {
                /* 只在设置真的变化时发布 CHANGE_SETTINGS。
                 * 曾经无条件发布，导致 UI 每 9 秒弹一次 OSD 悬浮窗。 */
                DWORD mask = 0;
                Lock();
                int newRateIdx = (int)s.gRateIndex;
                int newRateHz  = McHose::RateIndexToHz(s.gRateIndex, g_state.rateCount);
                if (!g_state.settingsValid ||
                    g_state.rateIndex != newRateIdx ||
                    g_state.rateHz != newRateHz ||
                    g_state.writeRateIndex != (int)s.usbRateIndex ||
                    g_state.dpiIndexRaw != s.usbDpiIndex ||
                    g_state.dpiActiveIndex != s.gDpiIndex ||
                    g_state.sleepMinutes != s.sleep)
                    mask |= CHANGE_SETTINGS;
                for (int i = 0; i < 6; i++) {
                    if (g_state.dpi[i] != s.dpi[i]) { mask |= CHANGE_SETTINGS; break; }
                }

                g_state.settingsValid   = true;
                g_state.rateIndex       = newRateIdx;
                g_state.writeRateIndex  = (int)s.usbRateIndex;
                g_state.rateHz          = newRateHz;
                g_state.dpiIndexRaw     = s.usbDpiIndex;
                g_state.dpiActiveIndex  = s.gDpiIndex;
                g_state.sleepMinutes    = s.sleep;
                for (int i = 0; i < 6; i++) g_state.dpi[i] = s.dpi[i];
                Unlock();

                if (mask != 0) Publish(mask);
            } else {
                loopFailed = true;
                Lock(); g_state.readErrors++; Unlock();
            }
        }

        if (loopFailed) {
            failStreak++;
        } else {
            failStreak = 0;
            offlineBackoffMs = 0;
        }

        /* ---- 连续失败判离线 ---- */
        if (failStreak >= 3) {
            push.CloseDevice();
            CloseHandle(hFeature);
            hFeature = INVALID_HANDLE_VALUE;
            Lock();
            g_state.connected = false;
            g_state.settingsValid = false;
            g_state.pushChannelOpen = false;
            Unlock();
            Publish(CHANGE_CONNECTED | CHANGE_BATTERY);
            failStreak = 0;
            offlineBackoffMs = (offlineBackoffMs == 0) ? 2000 : offlineBackoffMs * 2;
            if (offlineBackoffMs > 30000) offlineBackoffMs = 30000;
        }
    }

    push.CloseDevice();
    if (hFeature != INVALID_HANDLE_VALUE) CloseHandle(hFeature);
    if (push.ev != NULL) CloseHandle(push.ev);
    return 0;
}

}  // namespace

bool Start(StateCallback callback)
{
    if (g_hThread != NULL) return true;

    Lock();
    g_cb = callback;
    Unlock();

    g_hStop = CreateEventW(NULL, TRUE, FALSE, NULL);
    if (g_hStop == NULL) return false;

    /* 停止事件作为线程参数传入，线程内持有稳定副本（见 WorkerThread 注释） */
    g_hThread = CreateThread(NULL, 0, WorkerThread, (LPVOID)g_hStop, 0, NULL);
    if (g_hThread == NULL) {
        CloseHandle(g_hStop);
        g_hStop = NULL;
        Lock(); g_cb = NULL; Unlock();
        return false;
    }
    return true;
}

void Stop()
{
    if (g_hStop != NULL) SetEvent(g_hStop);

    bool exited = true;
    if (g_hThread != NULL) {
        /* 工作线程有界阻塞点：推送等待 300ms、退避最长 30s（可被停止事件立即打断）、
         * 每次特性读最多 4 次重试且 HidD_SetFeature/GetFeature 无超时参数，
         * 因此设备彻底无响应时单轮可能超过 8 秒。超时后绝不能关闭线程仍在等待的
         * 事件对象（否则 WaitForSingleObject 落在已释放句柄上是 UB），
         * 也绝不能清 g_cb（线程可能还会回调）。 */
        DWORD w = WaitForSingleObject(g_hThread, 8000);
        if (w == WAIT_OBJECT_0) {
            CloseHandle(g_hThread);
            g_hThread = NULL;
        } else {
            exited = false;
        }
    }
    if (exited) {
        if (g_hStop != NULL) { CloseHandle(g_hStop); g_hStop = NULL; }
        Lock(); g_cb = NULL; Unlock();   /* 线程已退出，清回调是安全的 */
    }
}

State GetState()
{
    State s;
    Lock();
    s = g_state;
    Unlock();
    return s;
}

bool RequestSetPollingRate(int hz)
{
    Lock();
    g_pendRate = true;
    g_pendRateHz = hz;
    g_lastCmdResult = CMD_RESULT_NONE;
    Unlock();
    return true;
}

bool RequestSetSleep(bool enable, int minutes)
{
    Lock();
    g_pendSleep = true;
    g_pendSleepEnable = enable;
    g_pendSleepMin = minutes;
    g_lastCmdResult = CMD_RESULT_NONE;
    Unlock();
    return true;
}

bool RequestSwitchDpiStage(int stageIndex)
{
    if (stageIndex < 0 || stageIndex > 5) return false;
    Lock();
    g_pendDpi = true;
    g_pendDpiStage = stageIndex;
    g_lastCmdResult = CMD_RESULT_NONE;
    Unlock();
    return true;
}

void RequestRefresh()
{
    Lock();
    g_pendRefresh = true;
    Unlock();
}

int TakeLastCommandResult()
{
    Lock();
    int r = g_lastCmdResult;
    g_lastCmdResult = CMD_RESULT_NONE;
    Unlock();
    return r;
}

}  // namespace Device
