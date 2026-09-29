/*
 * model_db.cpp — 迈从（MCHOSE）多机型适配表实现
 *
 * 数据来源分级：
 *   【实测】 本项目在真实硬件上读/写验证过
 *   【社区】 开源社区项目的公开记录（mouse_tray / mchose-cli / 论坛 lsusb 输出）
 *   【官网】 厂商或零售渠道公开的规格表（用于交叉印证 DPI 上限、按键数）
 *
 * 重要事实：**PID 会跨机型复用**。
 *   0x1020 在 A7 Pro 上是"有线 PID"，在 L7 Pro 上是"无线/接收器 PID"。
 * 因此 ResolveModel() 优先采用设备自报的型号名，VID/PID 只作候选与开设备筛选。
 */

#include "model_db.h"
#include <windows.h>
#include <wchar.h>

namespace McHose {

/*
 * 已知 VID/PID 的机型。
 * dpiStages = 0 表示未知（调用方按 6 档处理，并由设备回读的档值数组自我校正）。
 */
static const ModelSpec kPidKnown[] = {
    /* name          vid      pidWL    pidWired  dpiMax  stages  rcWL rcWired source */
    { L"A7 Pro",     0x5253,  0x1021,  0x1020,   26000,  6,      6,   3,  "实测（本机硬件验证）" },
    { L"L7 Pro",     0x5253,  0x1020,  0x00B0,   26000,  0,      6,   3,  "社区 mouse_tray（VID/PID）+ 官网规格（26000dpi）" },
    { L"A7 V2 Ultra",0x3837,  0x100B,  0x0000,   42000,  0,      6,   3,  "社区论坛 lsusb 3837:100b + 官网规格（42000dpi）" },
    { L"K7 Ultra",   0x3837,  0x1001,  0x4150,   42000,  0,      6,   3,  "社区 mchose-cli 协议规格 + 官网规格（42000dpi）" },
};

/*
 * 只知型号名与规格（VID/PID 未公开记录）的机型。
 * 设备会通过推送报文的 ASCII 字段自报型号名，因此这些机型同样能被正确识别与显示。
 */
struct NameOnlySpec {
    const wchar_t *name;
    int dpiMax;
    const char *source;
};
static const NameOnlySpec kNameOnly[] = {
    { L"A7 Ultra V2",     42000, "官网规格" },
    { L"A7 V2 Pro",       26000, "官网规格" },
    { L"A5 Ultra",            0, "官网在售" },
    { L"A5 V2 Ultra",         0, "官网在售" },
    { L"A7 V2 Ultra+",        0, "官网在售" },
    { L"L7 Ultra",            0, "官网在售" },
    { L"L7 Pro Ultra",        0, "官网在售" },
    { L"M7 Ultra",            0, "官网在售" },
    { L"AX5 Pro Max",     26000, "官网规格" },
};

/* 各 VID 的通配条目：未知机型按同方案默认参数运行 */
static const ModelSpec kWildcard[] = {
    { NULL, 0x5253, 0, 0, 0, 0, 6, 3, "unknown" },
    { NULL, 0x3837, 0, 0, 0, 0, 6, 3, "unknown" },
};

static const ModelSpec kUnknown = {
    NULL, 0, 0, 0, 0, 0, 6, 3, "未知机型（按同方案默认参数）"
};

const ModelSpec *ModelTable(int *count)
{
    if (count) *count = (int)(sizeof(kPidKnown) / sizeof(kPidKnown[0]));
    return kPidKnown;
}

bool IsKnownMchoseVid(unsigned short vid)
{
    /* 这两个 VID 都是社区/实测记录到的迈从（Realtek 方案）设备 */
    return vid == 0x5253 || vid == 0x3837;
}

bool PathLooksLikeMchose(const wchar_t *devicePath)
{
    if (devicePath == NULL) return false;
    return (wcsstr(devicePath, L"vid_5253") != NULL ||
            wcsstr(devicePath, L"vid_3837") != NULL);
}

/* 忽略大小写的子串查找 */
static bool ContainsNoCase(const wchar_t *hay, const wchar_t *needle)
{
    if (hay == NULL || needle == NULL || needle[0] == L'\0') return false;
    size_t nl = wcslen(needle);
    for (const wchar_t *p = hay; *p; p++) {
        size_t i = 0;
        while (i < nl && p[i]) {
            wchar_t a = p[i], b = needle[i];
            if (a >= L'A' && a <= L'Z') a = (wchar_t)(a - L'A' + L'a');
            if (b >= L'A' && b <= L'Z') b = (wchar_t)(b - L'A' + L'a');
            if (a != b) break;
            i++;
        }
        if (i == nl) return true;
    }
    return false;
}

/* 设备自报名有时带厂商前缀（如 "MCHOSE A7 Pro"），比较时跳过它 */
static const wchar_t *SkipVendorPrefix(const wchar_t *name)
{
    if (name == NULL) return NULL;
    if (ContainsNoCase(name, L"mchose")) {
        const wchar_t *p = name;
        while (*p && *p != L' ') p++;
        while (*p == L' ') p++;
        if (*p) return p;
    }
    return name;
}

const ModelSpec *ResolveModel(const wchar_t *reportedName,
                              unsigned short vid, unsigned short pid)
{
    const wchar_t *bare = SkipVendorPrefix(reportedName);

    /* 1) 型号名优先：PID 会跨机型复用，设备自报的名字才是权威判据。 */
    if (bare != NULL && bare[0] != L'\0') {
        for (size_t i = 0; i < sizeof(kPidKnown) / sizeof(kPidKnown[0]); i++) {
            if (kPidKnown[i].name != NULL && ContainsNoCase(bare, kPidKnown[i].name))
                return &kPidKnown[i];
        }
        /* 只知名、不知 VID/PID 的机型：给一个按名字命名的临时条目 */
        for (size_t i = 0; i < sizeof(kNameOnly) / sizeof(kNameOnly[0]); i++) {
            if (ContainsNoCase(bare, kNameOnly[i].name)) {
                static ModelSpec byName[sizeof(kNameOnly) / sizeof(kNameOnly[0])];
                static bool inited = false;
                if (!inited) {
                    for (size_t k = 0; k < sizeof(kNameOnly) / sizeof(kNameOnly[0]); k++) {
                        byName[k] = kUnknown;
                        byName[k].name    = kNameOnly[k].name;
                        byName[k].dpiMax  = kNameOnly[k].dpiMax;
                        byName[k].source  = kNameOnly[k].source;
                    }
                    inited = true;
                }
                return &byName[i];
            }
        }
    }

    /* 2) VID/PID 精确匹配 */
    for (size_t i = 0; i < sizeof(kPidKnown) / sizeof(kPidKnown[0]); i++) {
        const ModelSpec &m = kPidKnown[i];
        if (m.vid != vid) continue;
        if (m.pidWireless != 0 && m.pidWireless == pid) return &m;
        if (m.pidWired    != 0 && m.pidWired    == pid) return &m;
    }

    /* 3) 该 VID 的通配（未知机型） */
    for (size_t i = 0; i < sizeof(kWildcard) / sizeof(kWildcard[0]); i++) {
        if (kWildcard[i].vid == vid) return &kWildcard[i];
    }

    return &kUnknown;
}

ModelCaps CapsFor(const ModelSpec *spec, const wchar_t *reportedName)
{
    ModelCaps c;
    const wchar_t *bare = SkipVendorPrefix(reportedName);

    /* 展示名优先用设备自报的（最真实），否则用表里的名字 */
    if (bare != NULL && bare[0] != L'\0')      c.displayName = bare;
    else if (spec != NULL && spec->name != NULL) c.displayName = spec->name;
    else                                        c.displayName = L"MCHOSE 鼠标";

    c.dpiMax            = (spec && spec->dpiMax > 0) ? spec->dpiMax : 0;
    c.dpiStages         = (spec && spec->dpiStages > 0) ? spec->dpiStages : 6;
    c.rateCountWireless = (spec && spec->rateCountWireless > 0) ? spec->rateCountWireless : 6;
    c.rateCountWired    = (spec && spec->rateCountWired > 0) ? spec->rateCountWired : 3;
    c.source            = (spec && spec->source) ? spec->source : "unknown";
    c.known             = (spec != NULL && spec->name != NULL);
    return c;
}

}  // namespace McHose
