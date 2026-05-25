/*
 * 图像处理 / 找色算法
 *
 * 所有函数都从全局帧缓存（easylua.c 中的 g_frame_*）读，纯 CPU 计算，
 * 不需要 JNI 反射、不需要锁外面的资源。
 *
 * 颜色字符串格式（兼容 AutoGo）：
 *   "RRGGBB"            精确颜色
 *   "RRGGBB-RRGGBB"     带容差，后半段是每个通道的允许偏移（0-255）
 *   "RRGGBB|RRGGBB-..." 多个候选颜色用 | 分隔，命中其中任一即可
 *
 * 所有函数返回值都是基础数值类型，FFI 友好。
 */

#ifndef EASYLUA_IMAGES_H
#define EASYLUA_IMAGES_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 由 easylua.c 提供：返回 frame_data / w / h，调用方需自行同步 */
extern const uint8_t *EasyLua_FrameDataLocked(int *out_w, int *out_h);
extern void           EasyLua_FrameUnlock(void);

/**
 * 取像素颜色（无锁封装，给 Lua FFI 用）。
 * 越界返回 0。
 */
int Images_Pixel(int x, int y);   /* 返回 0xRRGGBB */

/**
 * 比单点颜色：测试 (x, y) 像素是否匹配 colorStr。
 * 返回 1 = 匹配，0 = 不匹配，-1 = 帧无效 / 越界。
 */
int Images_CmpColor(int x, int y, const char *color_str, float sim);

/**
 * 在区域 (x1, y1)-(x2, y2) 内找颜色，返回首个匹配的坐标。
 * x2/y2 == 0 表示用屏幕最大宽/高。
 * dir 0..3：扫描方向（0=L→R T→B; 1=R→L T→B; 2=L→R B→T; 3=R→L B→T）。
 *
 * out_x / out_y 写入找到的坐标（找不到时写 -1, -1）。
 * 返回 0 = 找到，-1 = 没找到 / 帧无效。
 */
int Images_FindColor(int x1, int y1, int x2, int y2,
                     const char *color_str, float sim, int dir,
                     int *out_x, int *out_y);

/**
 * 区域内颜色像素计数。返回符合条件的像素数（>= 0）。
 */
int Images_GetColorCountInRegion(int x1, int y1, int x2, int y2,
                                 const char *color_str, float sim);

/**
 * 多点比色（DetectsMultiColors，等价 AutoGo）：
 * colors 格式：x1,y1,RRGGBB-tolHexBytes,x2,y2,RRGGBB-tol,...
 * 一组三元（x, y, RRGGBB[-tol]），全部命中才返回 1。
 */
int Images_DetectsMultiColors(const char *colors, float sim);

/**
 * 多点找色（FindMultiColors，等价 AutoGo）：在区域内扫描，找第一个像素 P
 * 满足：P 自身颜色匹配 base_color，且每个相对偏移点 P+(dx,dy) 也匹配各自颜色。
 *
 * colors 格式：baseRRGGBB[-tol],dx1,dy1,RRGGBB[-tol],dx2,dy2,RRGGBB[-tol],...
 *   - 首段 base color 是绝对扫描的颜色
 *   - 后续每 3 段一组：相对 dx, dy, 颜色
 *
 * 返回 0 = 找到（写 out_x/out_y 为绝对坐标），-1 = 没找到 / 解析失败。
 */
int Images_FindMultiColors(int x1, int y1, int x2, int y2,
                           const char *colors, float sim, int dir,
                           int *out_x, int *out_y);

/**
 * 多点找色 - 找全部匹配位置。
 *
 * out_xy 是调用方提供的 int[max_n*2] 数组，按 [x0,y0, x1,y1, ...] 顺序写入。
 * max_n 是数组能放的点数（不是 int 数）。
 * 返回实际写入的点数（>= 0），-1 = 解析失败。
 *
 * 算法：找到一处后，把 (basePoint + bbox) 矩形屏蔽掉避免重复，继续扫。
 */
int Images_FindMultiColorsAll(int x1, int y1, int x2, int y2,
                              const char *colors, float sim, int dir,
                              int *out_xy, int max_n);

/* 把当前帧的指定区域抓成独立快照。
 * 参考 AutoGo 的 images.CaptureScreen：返回 malloc 内存，调用方持有所有权。
 * Lua 端用 cdata + __gc 自动释放。
 *
 * 返回 NULL：区域非法 / 视频流无帧 / 内存不足。
 */
typedef struct EasyLuaSnapshot {
    int      w;
    int      h;
    uint8_t *pix;   /* 紧凑 RGBA8888，stride = w * 4 */
} EasyLuaSnapshot;

EasyLuaSnapshot *Images_Capture(int x1, int y1, int x2, int y2);
void             Images_CaptureFree(EasyLuaSnapshot *s);

/* 取快照单像素（越界返回 0）。 */
int  Images_SnapshotPixel(const EasyLuaSnapshot *s, int x, int y);

/*
 * 保存快照到文件。
 *
 * type   "png" / "jpg" (alias "jpeg") / "bmp"，大小写不敏感。
 *        type == NULL 或 "" 时按 path 后缀推断，未识别则报错。
 * quality JPG 质量 1..100；其它格式忽略。<= 0 视为 90。
 *
 * 返回 0 = 成功；< 0 错误码：-1 参数 / -2 编码 / -3 写文件失败。
 */
int  Images_SnapshotSave(const EasyLuaSnapshot *s, const char *path,
                         const char *type, int quality);

/* ============================================================
 * 找图（FindPic）：模板加载 + 两段式 SAD 匹配
 *
 * 用法概览：
 *   1. 通过 Lua 层 io.open 读入模板字节（支持 Stage 7 .enc 透明解密），
 *      然后调用 Images_LoadTemplate 解码并预处理；
 *   2. 用 Images_FindPic / Images_FindPicAll 在全局帧上扫描；
 *   3. 不再需要时调用 Images_FreeTemplate 释放。
 *
 * 模板结构对调用方不透明：所有字段访问均通过 Images_Template* 元信息
 * API 完成，便于后续演进（缓存、加速结构）而不破坏 ABI。
 * ============================================================ */

/* 模板不透明类型。Lua 端通过 cdata + __gc 管理生命周期。 */
typedef struct EasyLuaTemplate EasyLuaTemplate;

/**
 * 从内存字节解码并构建模板。
 *   bytes / len : 文件原始字节（PNG / JPG / BMP）
 *   path_hint   : 仅用于错误日志，可为 NULL
 *
 * 自动判定：
 *   - alpha < 128 的像素视为透明（仅当解码出 4 通道时生效）；
 *   - 仅当 PNG / BMP 且四角四像素 RGB 严格相等时启用 color key
 *     （JPG 因量化噪点不启用）；
 *   - 任一规则命中即从 valid 像素表里剔除。
 *
 * 失败返回 NULL（解码失败 / valid_n == 0 / 尺寸越界 / 内存不足），失败时
 * 内部已通过 EL_ERR 输出错误并保证不泄漏任何中间分配。
 *
 * 调用方持有所有权，必须用 Images_FreeTemplate 释放。
 */
EasyLuaTemplate *Images_LoadTemplate(const uint8_t *bytes, int len,
                                     const char *path_hint);

/**
 * 释放模板。t == NULL 时为 no-op；释放后该指针不可再使用。
 */
void Images_FreeTemplate(EasyLuaTemplate *t);

/* 元信息访问（Lua 端 :Width() / :Height() / :ValidPx() 调试用） */
int  Images_TemplateW(const EasyLuaTemplate *t);          /* 模板宽度 */
int  Images_TemplateH(const EasyLuaTemplate *t);          /* 模板高度 */
int  Images_TemplateValidPx(const EasyLuaTemplate *t);    /* 有效像素数 */

/**
 * 单次找图。
 *   x1,y1,x2,y2 : 扫描区域；x2/y2 == 0 表示用屏幕边界
 *   t           : 已加载模板（不能为 NULL）
 *   sim         : 相似度阈值，[0, 1]，1.0 = 像素完全一致
 *   dir         : 扫描方向 0..3（与 Images_FindColor 一致）
 *   out_x/out_y : 命中时写入模板左上角坐标；未命中写 -1, -1
 *
 * 返回 0 = 命中，-1 = 未命中 / 帧无效 / 模板比扫描区还大 / 参数非法。
 * 函数返回前必定配对调用一次 EasyLua_FrameUnlock。
 */
int Images_FindPic(int x1, int y1, int x2, int y2,
                   const EasyLuaTemplate *t, float sim, int dir,
                   int *out_x, int *out_y);

/**
 * 找图 - 找全部匹配位置。
 *
 * out_xy : 调用方提供的 int[max_n*2]，按 [x0,y0, x1,y1, ...] 顺序写入；
 * max_n  : 数组能放的「点数」（不是 int 数）。
 *
 * 命中后将 (x, y) ~ (x + tpl.w - 1, y + tpl.h - 1) 矩形追加到屏蔽矩形数组，
 * 后续候选起点若落在任一屏蔽矩形内会被跳过，避免重叠重复命中。
 *
 * 返回实际写入的点数（>= 0）。out_xy == NULL 或 max_n <= 0 时返回 0。
 */
int Images_FindPicAll(int x1, int y1, int x2, int y2,
                      const EasyLuaTemplate *t, float sim,
                      int *out_xy, int max_n);

/**
 * 找图（色差模式）—— 大漠 FindPic 兼容签名。
 *
 * 与 Images_FindPic 的差别：
 *   - 单像素判定走"色差通过"而非 SAD 累加：
 *       valid 像素 (vx, vy) 上，
 *         |Δr| <= dr && |Δg| <= dg && |Δb| <= db  ⇒ 该像素"通过"
 *   - 命中规则：通过的像素数 / valid_n >= sim
 *   - dr/dg/db 来自 6 位 hex（"RRGGBB"），分别对应红/绿/蓝通道允许的最大
 *     绝对差。"000000" + sim=1.0 ≈ 像素完全一致；"101010" + sim=0.9 ≈
 *     90% 像素三通道差 ≤ 16。
 *
 * 与 SAD 模式相比，色差模式：
 *   - 对反走样、轻微抖动更稳健（局部高对比像素不会拖累整体平均差）；
 *   - 不需要 sim → SAD 阈值的换算，参数语义与大漠经典签名一致；
 *   - 单像素早退更激进（一旦失败比例过高，立即放弃）。
 *
 * 参数：
 *   x1,y1,x2,y2 : 扫描区，x2/y2 == 0 = 屏幕边界
 *   t           : 已加载模板（不能为 NULL）
 *   dr,dg,db    : R/G/B 通道色差阈值，[0, 255]
 *   sim         : 通过比例阈值 [0, 1]，1.0 = 全部 valid 像素必须通过
 *   dir         : 扫描方向 0..3
 *   out_x/out_y : 命中时写入模板左上角；未命中写 -1, -1
 *
 * 返回 0 = 命中，-1 = 未命中。函数返回前必定配对调用一次 EasyLua_FrameUnlock。
 */
int Images_FindPicDelta(int x1, int y1, int x2, int y2,
                        const EasyLuaTemplate *t,
                        int dr, int dg, int db,
                        float sim, int dir,
                        int *out_x, int *out_y);

/**
 * 找图（色差模式）- 找全部。语义同 Images_FindPicAll，单像素判定走色差。
 */
int Images_FindPicAllDelta(int x1, int y1, int x2, int y2,
                           const EasyLuaTemplate *t,
                           int dr, int dg, int db,
                           float sim,
                           int *out_xy, int max_n);

/* ============================================================
 * 多模板找图（"A.png|B.png" 格式 ⇒ C 内单次扫描）
 *
 * 性能优势：单次 EasyLua_FrameDataLocked + 单次区域规整 + 候选起点
 * 轮询所有模板。比 Lua 层循环调 FindPic N 次显著快——避免 N-1 次
 * 重复取锁、重复扫描相同候选起点。
 *
 * 命中语义：扫描区按 dir 顺序遍历，每个候选起点依次尝试 tpls[0..n)，
 * 第一个命中的模板写出索引（out_idx）和坐标，然后 FindPicMulti 立即返回。
 *
 * FindPicAllMulti 用同一套屏蔽矩形（向四周扩 max(t->w-1, t->h-1) 个
 * 像素），保证不同模板的命中点 bbox 也不重叠。
 *
 * 参数：
 *   tpls[i]    : 已加载模板，允许 NULL（自动跳过）
 *   n_tpls     : tpls 数组长度（>= 1）
 *   out_idx    : FindPicMulti 命中时写入命中模板下标（[0, n_tpls)）
 *   out_xy     : FindPicAllMulti 输出坐标，按 [x0,y0, x1,y1, ...] 顺序
 *   out_idxs   : FindPicAllMulti 命中模板下标数组（与 out_xy 一一对应）
 *   max_n      : out_xy / out_idxs 容量（点数，不是 int 数）
 * ============================================================ */
int Images_FindPicMulti(int x1, int y1, int x2, int y2,
                        const EasyLuaTemplate * const *tpls, int n_tpls,
                        float sim, int dir,
                        int *out_x, int *out_y, int *out_idx);

int Images_FindPicAllMulti(int x1, int y1, int x2, int y2,
                           const EasyLuaTemplate * const *tpls, int n_tpls,
                           float sim,
                           int *out_xy, int *out_idxs, int max_n);

int Images_FindPicMultiDelta(int x1, int y1, int x2, int y2,
                             const EasyLuaTemplate * const *tpls, int n_tpls,
                             int dr, int dg, int db,
                             float sim, int dir,
                             int *out_x, int *out_y, int *out_idx);

int Images_FindPicAllMultiDelta(int x1, int y1, int x2, int y2,
                                const EasyLuaTemplate * const *tpls, int n_tpls,
                                int dr, int dg, int db,
                                float sim,
                                int *out_xy, int *out_idxs, int max_n);

#ifdef __cplusplus
}
#endif
#endif
