/*
 * model_db.h — 迈从（MCHOSE）多机型适配表
 * ---------------------------------------------------------------------------
 * 为什么需要这张表：
 *   1) 不同机型的 VID/PID 不同。实测 A7 Pro（2.4G）是 VID 0x5253 / PID 0x1021，
 *      而社区记录 K7 Ultra 是 VID 0x3837 / PID 0x1001 —— 只认 0x5253 会漏掉整个产品线。
 *   2) **PID 不能唯一确定机型**：0x1020 在 A7 Pro 上是"有线"、在 L7 Pro 上是"无线"。
 *      因此机型判定必须以**设备自报的型号名**（推送 report 0x13 的 ASCII 名）为准，
 *      VID/PID 只用于"是否值得打开这个 HID 集合"和"候选机型"。
 *   3) 规格参数（DPI 上限、档位数、回报率档数）因机型而异，不能写死。
 *
 * 数据来源分级（与 docs/PROTOCOL.md 一致）：
 *   【实测】 本项目在真实硬件上读/写验证过
 *   【社区】 开源社区对同方案其它机型的公开记录（仅作候选，不作为唯一依据）
 *   型号名未知时一律回落到"未知机型"条目：按同方案默认参数运行，并如实标注。
 */

#ifndef MCHOSE_MODEL_DB_H
#define MCHOSE_MODEL_DB_H

namespace McHose {

/* 机型规格。字段为 0 表示"未知"，调用方应回落到默认值而不是当成 0 使用。 */
struct ModelSpec {
    const wchar_t *name;        /* 展示名；同时用于与设备自报型号名匹配 */
    unsigned short vid;
    unsigned short pidWireless;
    unsigned short pidWired;
    int  dpiMax;                /* 规格 DPI 上限（0 = 未知） */
    int  dpiStages;             /* DPI 档位数量（0 = 未知，默认按 6） */
    int  rateCountWireless;     /* 2.4G 下回报率档位数（0 = 未知，默认 6） */
    int  rateCountWired;        /* 有线档位数（0 = 未知，默认 3） */
    const char *source;         /* 数据来源，便于审计 */
};

/* 全部已知机型（含"未知机型"回落项，name == NULL） */
const ModelSpec *ModelTable(int *count);

/* 该 VID 是否属于迈从（用于决定是否尝试打开这个 HID 集合） */
bool IsKnownMchoseVid(unsigned short vid);

/* 按设备路径粗筛用：判断路径里的 vid_xxxx 是否是我们认识的厂商 */
bool PathLooksLikeMchose(const wchar_t *devicePath);

/*
 * 机型判定优先级：
 *   1) 设备自报的型号名（最可靠——PID 会跨机型复用）
 *   2) VID/PID 精确匹配
 *   3) 该 VID 的通配条目（未知机型，按默认参数运行）
 * 永不返回 NULL，最差返回"未知机型"条目。
 */
const ModelSpec *ResolveModel(const wchar_t *reportedName,
                              unsigned short vid, unsigned short pid);

/* 解析后的有效参数（把"未知"填成默认值），供设备层/界面层直接使用 */
struct ModelCaps {
    const wchar_t *displayName; /* 可能来自设备自报，也可能是表里的名字 */
    int dpiMax;
    int dpiStages;
    int rateCountWireless;
    int rateCountWired;
    const char *source;
    bool known;                 /* false = 未能识别，按同方案默认参数运行 */
};
ModelCaps CapsFor(const ModelSpec *spec, const wchar_t *reportedName);

}  // namespace McHose

#endif  // MCHOSE_MODEL_DB_H
