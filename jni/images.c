/*
 * 图像处理 / 找色算法实现
 *
 * 注意性能：FindColor / GetColorCount 这种"扫整张图"的函数会被 Lua
 * 反复调用（每帧一次），逐像素比较 + 字符串解析必须高效：
 *   - color_str 解析放到函数入口处一次性完成
 *   - 内层循环只做整数比较，不做字符串处理
 *   - 不使用 lua_error / 异常路径
 */

#include "images.h"

#include <ctype.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---------- 错误日志宏 ----------
 *
 * 与 easylua.c 中的 EL_ERR 实现等价（fprintf(stderr) + "[easylua-c] error:"
 * 前缀），保持每个翻译单元自包含、避免跨 .c 文件强耦合。前缀会被
 * ScriptRunner.kt 端识别并写入设备日志、LogPanel 与 VSIX 输出 tab。
 *
 * 使用：EL_ERR("LoadTemplate: 解码失败 path=%s", path_hint); */
#define EL_ERR(fmt, ...) do { \
    fprintf(stderr, "[easylua-c] error: " fmt "\n", ##__VA_ARGS__); \
    fflush(stderr); \
} while (0)

/* stb_image_write：单文件 PNG/JPG/BMP 编码，仅在本翻译单元 STB_IMPLEMENTATION。
 * 不定义 STBI_WRITE_NO_STDIO，这样可以直接用 stbi_write_png / _jpg / _bmp。 */
#define STB_IMAGE_WRITE_IMPLEMENTATION
#define STBIW_ASSERT(x)   ((void)0)
#include "stb_image_write.h"

/* stb_image：单文件 PNG/JPG/BMP 解码，仅在本翻译单元 STB_IMPLEMENTATION。
 *
 * 通过下列 STBI_NO_* 宏裁掉所有用不到的格式与 stdio 路径，仅保留
 * PNG / JPG / BMP 三种格式的内存解码：
 *   - STBI_NO_HDR / NO_LINEAR：禁用 HDR 浮点解码与线性化分支
 *   - STBI_NO_PIC / NO_PNM / NO_GIF / NO_PSD / NO_TGA：禁用对应解码器
 *   - STBI_NO_STDIO：不引入 fopen/fread 路径，外部只能从内存解码
 *     （Stage 7 .enc 透明解密由 Lua 层 io.open 完成，C 端只接受字节缓冲）
 *
 * 既能控制二进制体积，也避免了 C 端意外触达磁盘 IO。 */
#define STB_IMAGE_IMPLEMENTATION
#define STBI_NO_HDR
#define STBI_NO_LINEAR
#define STBI_NO_PIC
#define STBI_NO_PNM
#define STBI_NO_GIF
#define STBI_NO_PSD
#define STBI_NO_TGA
#define STBI_NO_STDIO
#define STBI_ASSERT(x)    ((void)0)
#include "stb_image.h"

/* NEON 检测：arm64 自动启用，其它架构走 scalar 路径。 */
#if defined(__aarch64__) && defined(__ARM_NEON)
#include <arm_neon.h>
#define EL_HAS_NEON 1
#else
#define EL_HAS_NEON 0
#endif

/* NEON dense 路径前向声明（实现在 try_match / try_match_delta 之后）。
 * 仅 arm64 启用；放在 try_match 之前，让两段式 SAD 与色差模式可调用。 */
struct EasyLuaTemplate;
#if EL_HAS_NEON
static int try_match_dense_sad_neon(const uint8_t *data, int W,
                                    const struct EasyLuaTemplate *t,
                                    int px, int py,
                                    int64_t total_budget);
static int try_match_dense_delta_neon(const uint8_t *data, int W,
                                      const struct EasyLuaTemplate *t,
                                      int px, int py,
                                      int dr_thr, int dg_thr, int db_thr,
                                      int pass_budget, int fail_budget);
#endif

/* ---------- 单个候选颜色 ----------
 *
 * 一个候选 = (R, G, B, tolR, tolG, tolB)
 * 命中条件：|frameR - R| <= tolR && |G差| <= tolG && |B差| <= tolB
 */
typedef struct {
    uint8_t r, g, b;
    uint8_t tr, tg, tb;
} ColorTok;

#define MAX_COLORS 16

/* 把单个 hex 字符转成 0..15；非法返回 -1 */
static int hexv(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

/* 把 6 位 hex 字符串解析为 RGB；成功返回指向结尾的指针，失败返回 NULL */
static const char *parse_rgb(const char *p, uint8_t *r, uint8_t *g, uint8_t *b)
{
    int v[6];
    for (int i = 0; i < 6; i++) {
        v[i] = hexv(p[i]);
        if (v[i] < 0) return NULL;
    }
    *r = (uint8_t)((v[0] << 4) | v[1]);
    *g = (uint8_t)((v[2] << 4) | v[3]);
    *b = (uint8_t)((v[4] << 4) | v[5]);
    return p + 6;
}

/* 解析 colorStr：支持 "RRGGBB[-RRGGBB][|RRGGBB[-RRGGBB]]..." 多候选
 * sim < 1.0 时全局再加 (1-sim)*255 容差。 */
static int parse_color_list(const char *s, float sim, ColorTok *out, int max_n)
{
    if (!s || !*s) return 0;
    int n = 0;
    int extra_tol = 0;
    if (sim < 1.0f && sim > 0.0f) {
        extra_tol = (int)((1.0f - sim) * 255.0f);
        if (extra_tol < 0) extra_tol = 0;
        if (extra_tol > 255) extra_tol = 255;
    }
    const char *p = s;
    while (*p && n < max_n) {
        ColorTok t;
        const char *q = parse_rgb(p, &t.r, &t.g, &t.b);
        if (!q) return -1;
        t.tr = t.tg = t.tb = 0;
        if (*q == '-') {
            uint8_t tr, tg, tb;
            const char *q2 = parse_rgb(q + 1, &tr, &tg, &tb);
            if (!q2) return -1;
            t.tr = tr; t.tg = tg; t.tb = tb;
            q = q2;
        }
        if (extra_tol > 0) {
            int a;
            a = (int)t.tr + extra_tol; t.tr = (uint8_t)(a > 255 ? 255 : a);
            a = (int)t.tg + extra_tol; t.tg = (uint8_t)(a > 255 ? 255 : a);
            a = (int)t.tb + extra_tol; t.tb = (uint8_t)(a > 255 ? 255 : a);
        }
        out[n++] = t;
        if (*q == '|') { p = q + 1; continue; }
        break;
    }
    return n;
}

/* 帧像素比对：r/g/b 是当前像素值，cs 是候选颜色数组 */
static int match_any(uint8_t r, uint8_t g, uint8_t b,
                     const ColorTok *cs, int n)
{
    for (int i = 0; i < n; i++) {
        const ColorTok *t = &cs[i];
        int dr = (int)r - (int)t->r; if (dr < 0) dr = -dr;
        int dg = (int)g - (int)t->g; if (dg < 0) dg = -dg;
        int db = (int)b - (int)t->b; if (db < 0) db = -db;
        if (dr <= t->tr && dg <= t->tg && db <= t->tb) return 1;
    }
    return 0;
}

/* 区域规整：把 (x1,y1)-(x2,y2) 限制到 [0..w-1]×[0..h-1] */
static void normalize_rect(int *x1, int *y1, int *x2, int *y2, int w, int h)
{
    if (*x2 == 0 || *x2 > w) *x2 = w;
    if (*y2 == 0 || *y2 > h) *y2 = h;
    if (*x1 < 0) *x1 = 0;
    if (*y1 < 0) *y1 = 0;
    if (*x2 > w) *x2 = w;
    if (*y2 > h) *y2 = h;
}

/* ---------- 屏蔽矩形（FindAll 系列共享） ----------
 *
 * 命中一个匹配后，会把它的"势力范围"（基点 + 偏移 bbox 或模板 wxh）记入屏蔽
 * 矩形数组，避免在该范围内再找出近似的同一目标。FindMultiColorsAll 与
 * FindPicAll 共用同一份定义，避免重复维护两套实现。
 */
typedef struct { int l, t, r, b; } MaskRect;

static int in_any_mask(int x, int y, const MaskRect *masks, int n)
{
    for (int i = 0; i < n; i++) {
        if (x >= masks[i].l && x <= masks[i].r &&
            y >= masks[i].t && y <= masks[i].b) return 1;
    }
    return 0;
}

/* ---------- 找图（FindPic）私有数据 ----------
 *
 * EasyLuaTemplate 对外是不透明类型（仅在 images.h 声明 typedef，未给出
 * 字段），所有访问都走 Images_Template* 元信息 API，便于后续演进。
 *
 * 数据布局参考 design.md「Data Models」：
 *   - rgb：解码后剥离 alpha 的紧凑 RGB 平铺，stride = w*3。剥 alpha 后
 *          内层 SAD 循环少读 25% 字节，且帧侧 g_frame_data 仍是 RGBA8888，
 *          所以模板偏移用 *3、帧偏移用 *4。
 *   - valid_off / valid_dxdy：SoA 平行数组，前者是 rgb 内字节偏移
 *          (vy*w + vx) * 3，后者是紧凑打包 (vy << 16) | vx。两者一一对应、
 *          按行优先顺序写入，给阶段 2 内层循环一次 ldp 载入两个 4 字节字段。
 *   - feat_off / feat_dxdy：valid 的子集，固定上限 FEAT_N = 12，直接内联
 *          到结构体省一次解引用；feat_n ∈ {0} ∪ [8, 12]，valid_n < 8 时
 *          feat_n = 0 跳过阶段 1。
 *   - has_alpha / has_color_key / key_r / key_g / key_b：透明判定元数据，
 *          调试与 Property 4 等价性测试用。 */

/* alpha 透明判定阈值：原始 alpha < ALPHA_CUT 视为透明 */
#define ALPHA_CUT          128

/* 特征点上限：阶段 1 预筛使用的最大特征点数 */
#define FEAT_N             12

/* 模板像素总数上限：约 2K×2K，防御性能/内存退化 */
#define MAX_TPL_PIXELS     (4 * 1024 * 1024)

struct EasyLuaTemplate {
    int       w, h;                 /* 模板尺寸（像素） */
    uint8_t  *rgb;                  /* 解码后 RGB 平铺，长度 w*h*3 */

    /* ---- valid 像素索引（SoA，cache 友好） ---- */
    int       valid_n;              /* 有效像素数（非透明） */
    int32_t  *valid_off;            /* rgb 内字节偏移 = (vy*w + vx) * 3 */
    int32_t  *valid_dxdy;           /* 紧凑打包 (vy << 16) | vx，
                                     * 给阶段 2 计算帧偏移用 */

    /* ---- 特征点子集（valid 的子集，size <= FEAT_N） ---- */
    int       feat_n;               /* {0} ∪ [8, FEAT_N] */
    int32_t   feat_off[FEAT_N];     /* 同 valid_off 编码 */
    int32_t   feat_dxdy[FEAT_N];    /* 同 valid_dxdy 编码 */

    /* ---- 透明判定元数据（调试 / 测试用） ---- */
    uint8_t   has_alpha;            /* 解码出来含 alpha 通道（4 通道） */
    uint8_t   has_color_key;        /* 启用了四角同色 color key */
    uint8_t   key_r, key_g, key_b;  /* has_color_key=1 时有效 */

    /* ---- NEON 行扫描快路径标记 ----
     * is_dense = 1 ⇔ valid_n == w*h，即没有任何透明像素需要剔除。
     * 此种模板可以按"行 × w 像素"的紧凑顺序扫描，启用 NEON 一次比 16 像素，
     * 比走 valid_off 随机访问快 3-5× 且 cache 友好。 */
    uint8_t   is_dense;
};

/* ---------- 图像格式枚举（内部使用） ----------
 *
 * 通过文件魔数（magic number）区分模板字节流的图像格式，决定是否启用
 * 四角同色 color key 检测。JPG 因为 DCT 量化噪点不稳定，强制禁用 color
 * key（design.md「Algorithm 1」要求），所以这里把"未知 / 默认"也归为
 * IMG_FMT_JPG，避免误启用。
 *
 * 后续 Task 2.3 (Images_LoadTemplate) 会根据字节首 N 字节做魔数判定，
 * 把结果传给 detect_color_key 驱动其 JPG 短路路径。 */
typedef enum {
    IMG_FMT_JPG = 0,                /* JPEG / 默认未识别 */
    IMG_FMT_PNG = 1,                /* PNG，魔数 89 50 4E 47 */
    IMG_FMT_BMP = 2                 /* BMP，魔数 42 4D */
} ImgFormat;

/* ---------- detect_color_key：四角同色透明键判定 ----------
 *
 * 严格按 design.md「Algorithm 1: detect_color_key」实现：
 *   1) JPG 强制禁用：JPG 解码后边角像素受 8x8 块边界与色度二次采样的影响，
 *      四角即便"看起来同色"也常有 ±1 ~ ±3 的偏移；启用 color key 会误删
 *      大量本应 valid 的像素。所以 JPG 直接返回 false。
 *   2) 尺寸过小（w < 2 或 h < 2）时四角概念不成立，返回 false。
 *   3) 严格相等比较：四角四像素的 R/G/B 三通道两两完全相等时才启用，
 *      不允许任何容差。这是最保守的策略，避免 PNG / BMP 边角真实图案被
 *      误判成 color key。
 *
 * 输入：
 *   rgba   - 已解码的 RGBA8888 紧凑缓冲，stride = w * 4，长度 ≥ w*h*4
 *   w, h   - 图像宽高（像素）
 *   format - 图像格式枚举（PNG / BMP / JPG）
 *
 * 输出（通过 out 参数）：
 *   *out_r / *out_g / *out_b - 仅当返回 1 时写入有效 RGB；返回 0 时写 0
 *
 * 返回值：1 = 启用 color key，0 = 不启用
 *
 * 不变量：函数从 rgba 仅读取 4 个像素的 RGB，绝不写入 rgba。 */
static int detect_color_key(const uint8_t *rgba, int w, int h, int format,
                            uint8_t *out_r, uint8_t *out_g, uint8_t *out_b)
{
    /* 任何返回 false 路径都把 out_r/g/b 清零，避免调用方拿到未初始化值 */
    if (out_r) *out_r = 0;
    if (out_g) *out_g = 0;
    if (out_b) *out_b = 0;

    /* JPG 强制不启用 color key（DCT 量化噪点导致边角不稳定） */
    if (format == IMG_FMT_JPG) return 0;

    /* 尺寸过小，"四角"概念不成立 */
    if (w < 2 || h < 2) return 0;

    /* 计算四角的字节偏移：左上 / 右上 / 左下 / 右下 */
    const size_t off_tl = 0;
    const size_t off_tr = ((size_t)(w - 1)) * 4;
    const size_t off_bl = ((size_t)(h - 1)) * (size_t)w * 4;
    const size_t off_br = (((size_t)(h - 1)) * (size_t)w + (size_t)(w - 1)) * 4;

    const uint8_t r0 = rgba[off_tl + 0];
    const uint8_t g0 = rgba[off_tl + 1];
    const uint8_t b0 = rgba[off_tl + 2];

    /* 严格相等：右上、左下、右下都必须与左上 RGB 完全一致 */
    if (rgba[off_tr + 0] != r0 || rgba[off_tr + 1] != g0 || rgba[off_tr + 2] != b0) return 0;
    if (rgba[off_bl + 0] != r0 || rgba[off_bl + 1] != g0 || rgba[off_bl + 2] != b0) return 0;
    if (rgba[off_br + 0] != r0 || rgba[off_br + 1] != g0 || rgba[off_br + 2] != b0) return 0;

    if (out_r) *out_r = r0;
    if (out_g) *out_g = g0;
    if (out_b) *out_b = b0;
    return 1;
}

/* ---------- 找图（FindPic）静态辅助前向声明 ----------
 *
 * pick_features 在 Task 2.4 中实现；try_match 在 Task 3.1 中实现。
 * 这里集中前向声明，便于 Images_LoadTemplate / Images_FindPic /
 * Images_FindPicAll 调用。其余静态辅助（format 判定）也可后续按需追加
 * 在此区域。 */
static void pick_features(EasyLuaTemplate *t);
static int  try_match(const uint8_t *data, int W, const EasyLuaTemplate *t,
                      int px, int py,
                      int64_t total_budget, int64_t feat_budget);

/* 通过文件首字节的魔数判定图像格式：
 *   PNG 魔数：89 50 4E 47 ("\x89PNG")
 *   BMP 魔数：42 4D       ("BM")
 *   其他（含 JPEG FF D8 与未知格式）：归为 IMG_FMT_JPG
 *
 * 之所以把"未知"也归为 JPG，是为了让 detect_color_key 走最保守路径
 * （JPG 强制禁用 color key），避免对损坏 / 异常字节流误启用透明键。 */
static int detect_format(const uint8_t *bytes, int len)
{
    if (!bytes || len < 2) return IMG_FMT_JPG;
    if (len >= 4 &&
        bytes[0] == 0x89 && bytes[1] == 0x50 &&
        bytes[2] == 0x4E && bytes[3] == 0x47) {
        return IMG_FMT_PNG;
    }
    if (bytes[0] == 0x42 && bytes[1] == 0x4D) {
        return IMG_FMT_BMP;
    }
    return IMG_FMT_JPG;
}

/* ---------- Images_LoadTemplate：解码 + 预处理主流程 ----------
 *
 * 严格按 design.md「Algorithm 2: Images_LoadTemplate」实现，分五步：
 *   1) 参数 / 解码：bytes/len 校验；stbi_load_from_memory 强制 RGBA8888
 *      4 通道输出（即使源是 RGB 也补齐 alpha=255）；尺寸防御
 *      (w*h <= MAX_TPL_PIXELS)。
 *   2) 四角 color key 判定：按文件魔数得到 format，驱动 detect_color_key。
 *   3) 第一遍扫描：统计 valid 像素数。alpha < ALPHA_CUT 或命中 color key
 *      的像素视为透明、不计入 valid。valid_n == 0 直接失败；
 *      valid_n * 20 < total_n（< 5%）输出警告但仍继续。
 *   4) 分配模板结构 + 第二遍扫描：填 t->rgb（RGB 平铺剥离 alpha）、
 *      valid_off（rgb 内字节偏移 = (vy*w+vx)*3）、valid_dxdy（紧凑打包
 *      (vy<<16)|vx）。任一 malloc 失败按 LIFO 顺序释放已分配缓冲（含
 *      stbi rgba）后返回 NULL。
 *   5) 调用 pick_features 选取阶段 1 特征点子集，写入元数据并返回 t。
 *
 * 不变量：
 *   - 任何返回 NULL 路径必定先释放本次调用产生的全部分配（包括 stbi rgba）
 *   - 返回非 NULL 时 t->w >= 1, t->h >= 1, t->valid_n >= 1
 *   - 第一遍扫描的 valid_n 与第二遍写入的 vi 严格相等（用 assert 保护） */
EasyLuaTemplate *Images_LoadTemplate(const uint8_t *bytes, int len,
                                     const char *path_hint)
{
    /* path_hint 可为 NULL，仅用于错误日志；提前规整为可打印字符串 */
    const char *hint = path_hint ? path_hint : "(null)";

    /* ---- 1) 参数校验 ---- */
    if (!bytes || len <= 0) {
        EL_ERR("LoadTemplate: 参数非法 bytes=%p len=%d", (const void *)bytes, len);
        return NULL;
    }

    /* ---- 2) 解码 ---- */
    int w = 0, h = 0, orig_comp = 0;
    /* 第 5 个参数 = 4 强制输出 RGBA8888；源是 RGB 时 stbi 自动补 alpha=255。
     * orig_comp 反映源图通道数（1/2/3/4），用于推导 has_alpha。 */
    uint8_t *rgba = stbi_load_from_memory(bytes, len, &w, &h, &orig_comp, 4);
    if (!rgba) {
        EL_ERR("LoadTemplate: 解码失败 path=%s", hint);
        return NULL;
    }

    /* ---- 3) 尺寸防御 ---- */
    if (w <= 0 || h <= 0 ||
        (int64_t)w * (int64_t)h > (int64_t)MAX_TPL_PIXELS) {
        EL_ERR("LoadTemplate: 尺寸非法 %dx%d (上限 %d 像素) path=%s",
               w, h, MAX_TPL_PIXELS, hint);
        stbi_image_free(rgba);
        return NULL;
    }
    const int has_alpha = (orig_comp == 4) ? 1 : 0;
    const int total_n   = w * h;

    /* ---- 4) 四角 color key 判定（按魔数决定是否启用，JPG 短路） ---- */
    const int format = detect_format(bytes, len);
    uint8_t kr = 0, kg = 0, kb = 0;
    const int has_key = detect_color_key(rgba, w, h, format, &kr, &kg, &kb);

    /* ---- 5) 第一遍扫描：统计 valid 像素数 ---- */
    int valid_n = 0;
    {
        const uint8_t *p = rgba;
        for (int i = 0; i < total_n; i++, p += 4) {
            const uint8_t r = p[0];
            const uint8_t g = p[1];
            const uint8_t b = p[2];
            const uint8_t a = p[3];
            const int is_alpha_t = has_alpha && (a < ALPHA_CUT);
            const int is_key_t   = has_key   && (r == kr && g == kg && b == kb);
            if (!is_alpha_t && !is_key_t) {
                valid_n++;
            }
        }
    }
    if (valid_n == 0) {
        EL_ERR("LoadTemplate: 全透明，无可匹配像素 path=%s", hint);
        stbi_image_free(rgba);
        return NULL;
    }
    /* < 5% valid 警告但不失败：用户可能故意做"边角薄轮廓" */
    if (valid_n * 20 < total_n) {
        EL_ERR("LoadTemplate: 警告 valid 像素 %d/%d (<5%%) path=%s",
               valid_n, total_n, hint);
    }

    /* ---- 6) 分配模板结构 + SoA 子缓冲（LIFO 释放保护） ---- */
    EasyLuaTemplate *t = (EasyLuaTemplate *)calloc(1, sizeof(EasyLuaTemplate));
    if (!t) {
        EL_ERR("LoadTemplate: 内存不足 (struct) path=%s", hint);
        stbi_image_free(rgba);
        return NULL;
    }
    t->rgb = (uint8_t *)malloc((size_t)total_n * 3);
    if (!t->rgb) {
        EL_ERR("LoadTemplate: 内存不足 (rgb %d B) path=%s", total_n * 3, hint);
        free(t);
        stbi_image_free(rgba);
        return NULL;
    }
    t->valid_off = (int32_t *)malloc((size_t)valid_n * sizeof(int32_t));
    if (!t->valid_off) {
        EL_ERR("LoadTemplate: 内存不足 (valid_off %d B) path=%s",
               (int)((size_t)valid_n * sizeof(int32_t)), hint);
        free(t->rgb);
        free(t);
        stbi_image_free(rgba);
        return NULL;
    }
    t->valid_dxdy = (int32_t *)malloc((size_t)valid_n * sizeof(int32_t));
    if (!t->valid_dxdy) {
        EL_ERR("LoadTemplate: 内存不足 (valid_dxdy %d B) path=%s",
               (int)((size_t)valid_n * sizeof(int32_t)), hint);
        free(t->valid_off);
        free(t->rgb);
        free(t);
        stbi_image_free(rgba);
        return NULL;
    }

    /* ---- 7) 第二遍扫描：填 rgb / valid_off / valid_dxdy ---- */
    int vi = 0;
    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
            const int src_idx = (y * w + x) * 4;
            const int dst_idx = (y * w + x) * 3;
            const uint8_t r = rgba[src_idx + 0];
            const uint8_t g = rgba[src_idx + 1];
            const uint8_t b = rgba[src_idx + 2];
            const uint8_t a = rgba[src_idx + 3];
            /* 即使透明像素也写入 rgb（解码后字节本就有），保证邻居打分时
             * 可读，且属性 P2 只承诺 valid 之外的字节扰动不影响匹配，
             * 不要求 valid 之外字节为 0。 */
            t->rgb[dst_idx + 0] = r;
            t->rgb[dst_idx + 1] = g;
            t->rgb[dst_idx + 2] = b;

            const int is_alpha_t = has_alpha && (a < ALPHA_CUT);
            const int is_key_t   = has_key   && (r == kr && g == kg && b == kb);
            if (!is_alpha_t && !is_key_t) {
                /* 防御：两遍统计应严格一致；不一致说明上层字节被并发修改 */
                if (vi >= valid_n) {
                    EL_ERR("LoadTemplate: 内部错误 valid 计数不一致 path=%s", hint);
                    free(t->valid_dxdy);
                    free(t->valid_off);
                    free(t->rgb);
                    free(t);
                    stbi_image_free(rgba);
                    return NULL;
                }
                t->valid_off[vi]  = dst_idx;
                t->valid_dxdy[vi] = (y << 16) | x;
                vi++;
            }
        }
    }
    /* 同上的不变量：第一遍与第二遍统计严格相等 */
    if (vi != valid_n) {
        EL_ERR("LoadTemplate: 内部错误 vi=%d valid_n=%d path=%s",
               vi, valid_n, hint);
        free(t->valid_dxdy);
        free(t->valid_off);
        free(t->rgb);
        free(t);
        stbi_image_free(rgba);
        return NULL;
    }

    /* ---- 8) 写入元数据 + 释放 stbi rgba ---- */
    t->w             = w;
    t->h             = h;
    t->valid_n       = valid_n;
    t->has_alpha     = (uint8_t)has_alpha;
    t->has_color_key = (uint8_t)has_key;
    t->key_r         = kr;
    t->key_g         = kg;
    t->key_b         = kb;
    /* NEON 行扫描快路径标记：模板没有任何透明像素时启用。 */
    t->is_dense      = (valid_n == w * h) ? 1 : 0;
    /* feat_n / feat_off / feat_dxdy 全部由 calloc 初始化为 0；
     * pick_features 内部会按需写入；valid_n < 8 时会保持 feat_n = 0。 */

    stbi_image_free(rgba);

    /* ---- 9) 选阶段 1 特征点（在 Task 2.4 实现） ---- */
    pick_features(t);

    return t;
}

/* ---------- pick_features 辅助：(score, idx) 排序条目 ----------
 *
 * 把"每个 valid 像素的独特性得分"与"它在 valid_off / valid_dxdy 数组里的
 * 下标"打包到同一个结构体里，方便用标准库 qsort 一次性按分数降序排好。
 * 这样比"两个平行数组 + 间接索引"少一次缓存载入，且 qsort 的比较函数也
 * 不需要 thread-local 的上下文指针（C 标准的 qsort 没有 ctx 参数）。 */
typedef struct {
    int32_t score;   /* 4 邻居 R/G/B 通道绝对差的最大值，>= 0 */
    int32_t idx;     /* 在 t->valid_off / t->valid_dxdy 中的下标 */
} FeatScoreItem;

/* qsort 比较器：score 降序；分数相等时按 idx 升序，保证排序结果稳定可复现
 * （相等分数下选取顺序与 valid 数组的行优先顺序一致，便于测试断言）。 */
static int feat_score_cmp_desc(const void *a, const void *b)
{
    const FeatScoreItem *pa = (const FeatScoreItem *)a;
    const FeatScoreItem *pb = (const FeatScoreItem *)b;
    /* 用减法可能在极端情况下溢出（score 范围 [0, 255]，不会越 int32 边界，
     * 但保留显式比较更直观稳妥） */
    if (pa->score != pb->score) {
        return (pa->score < pb->score) ? 1 : -1;   /* 大分在前 */
    }
    if (pa->idx != pb->idx) {
        return (pa->idx < pb->idx) ? -1 : 1;       /* 小下标在前 */
    }
    return 0;
}

/* ---------- pick_features：阶段 1 特征点选取 ----------
 *
 * 严格按 design.md「Algorithm 3」实现，分三步：
 *   1) 给每个 valid 像素打"独特性"分：与上下左右 4 邻居 R/G/B 三通道的
 *      绝对差中的最大值；邻居越出模板边界则跳过该方向（按伪代码语义，
 *      不参与求最大值）。透明邻居仍按 rgb 数组里的值参与（解码后那里
 *      也有值，对比噪声更小，避免边缘像素全打 0 分）。
 *   2) 把 (score, idx) 打包后用 qsort 按分数降序排序；分数相等时按下标
 *      升序，保证结果稳定可复现。
 *   3) 按 4×4 网格分散选取：候选像素的网格坐标
 *        cell_x = min(3, vx * 4 / w)
 *        cell_y = min(3, vy * 4 / h)
 *      每格至多放 2 个特征点，累计选满 FEAT_N = 12 后立即停止。
 *
 * 终止条件：picked < 8 时退化为 feat_n = 0（让 FindPic 跳过阶段 1，
 * 走"只跑阶段 2"的安全路径）；picked >= 8 时 feat_n = picked。
 *
 * 失败处理：唯一的临时堆分配（FeatScoreItem 数组）失败时也走退化路径，
 * 不影响调用方—— Images_LoadTemplate 仍可返回有效模板。
 *
 * 内存：cell_cnt 用栈上 16-int 数组（设计伪代码用 malloc，但 16 个 int
 * 的栈数组等价且零分配开销，未改变可观察行为）。FeatScoreItem 数组在
 * 函数返回前 free。 */
static void pick_features(EasyLuaTemplate *t)
{
    if (!t) return;

    /* 1) 退化：valid 像素太少，跳过阶段 1（feat_off/feat_dxdy 已被 calloc
     *    清零，无需重复 memset） */
    if (t->valid_n < 8) {
        t->feat_n = 0;
        return;
    }

    const int n = t->valid_n;
    const int w = t->w;
    const int h = t->h;
    const uint8_t *rgb = t->rgb;

    /* 2) 分配 (score, idx) 数组；malloc 失败按退化路径处理 */
    FeatScoreItem *items =
        (FeatScoreItem *)malloc(sizeof(FeatScoreItem) * (size_t)n);
    if (!items) {
        EL_ERR("pick_features: 临时 score 数组分配失败 (n=%d)", n);
        t->feat_n = 0;
        return;
    }

    /* 3) 第一步：打分。4 邻居方向：左、右、上、下 */
    static const int kDx[4] = {-1, 1,  0, 0};
    static const int kDy[4] = { 0, 0, -1, 1};

    for (int i = 0; i < n; i++) {
        /* 解包 valid_dxdy[i]：低 16 位 vx，高 16 位 vy */
        const int32_t pack = t->valid_dxdy[i];
        const int vx = (int)(pack & 0xFFFF);
        const int vy = (int)((pack >> 16) & 0xFFFF);

        const uint8_t *p = rgb + t->valid_off[i];   /* 当前像素 RGB */
        int score = 0;

        for (int k = 0; k < 4; k++) {
            const int nx = vx + kDx[k];
            const int ny = vy + kDy[k];
            if (nx < 0 || nx >= w || ny < 0 || ny >= h) continue;

            const int n_off = (ny * w + nx) * 3;
            int dr = (int)p[0] - (int)rgb[n_off + 0]; if (dr < 0) dr = -dr;
            int dg = (int)p[1] - (int)rgb[n_off + 1]; if (dg < 0) dg = -dg;
            int db = (int)p[2] - (int)rgb[n_off + 2]; if (db < 0) db = -db;

            int m = dr;
            if (dg > m) m = dg;
            if (db > m) m = db;
            if (m > score) score = m;
        }
        items[i].score = (int32_t)score;
        items[i].idx   = (int32_t)i;
    }

    /* 4) 第二步：按分数降序排序（朴素 qsort，valid_n 上限约 4M 但
     *    O(n log n) 在加载阶段一次性开销可接受） */
    qsort(items, (size_t)n, sizeof(FeatScoreItem), feat_score_cmp_desc);

    /* 5) 第三步：4×4 网格分散选取，每格至多 2 个，选满 FEAT_N 后停止 */
    int cell_cnt[16];
    memset(cell_cnt, 0, sizeof(cell_cnt));

    int picked = 0;
    for (int k = 0; k < n && picked < FEAT_N; k++) {
        const int32_t i = items[k].idx;
        const int32_t pack = t->valid_dxdy[i];
        const int vx = (int)(pack & 0xFFFF);
        const int vy = (int)((pack >> 16) & 0xFFFF);

        /* 网格坐标：min(3, vx * 4 / w)；vx ∈ [0, w-1]，理论最大值
         * 为 ((w-1)*4)/w = 3，但 w == 1 时除法可能给出非预期值，min 保护 */
        int cx = (vx * 4) / (w > 0 ? w : 1);
        int cy = (vy * 4) / (h > 0 ? h : 1);
        if (cx > 3) cx = 3;
        if (cy > 3) cy = 3;
        const int cell = cy * 4 + cx;

        if (cell_cnt[cell] >= 2) continue;

        t->feat_off[picked]  = t->valid_off[i];
        t->feat_dxdy[picked] = t->valid_dxdy[i];
        cell_cnt[cell]++;
        picked++;
    }

    /* 6) 终止条件：选不满 8 个就退化（避免阶段 1 在弱模板上误杀候选） */
    if (picked < 8) {
        t->feat_n = 0;
    } else {
        t->feat_n = picked;
    }

    free(items);
}

/* ---------- try_match：两段式 SAD 早退匹配核 ----------
 *
 * 严格按 design.md「Algorithm 4: try_match」实现，是 Images_FindPic /
 * Images_FindPicAll 内层循环的唯一计算热点。给定一个候选起点 (px, py)，
 * 判定模板是否在该位置「足够相似」。
 *
 * 算法分两个阶段：
 *
 *   阶段 1（特征点稀疏预筛）：t->feat_n > 0 时执行。仅累加 feat_n 个高
 *   对比特征点的 SAD，一旦超过 feat_budget 立即返回 0。这一段以极低
 *   常量开销淘汰绝大多数候选位置，避免阶段 2 跑满 valid_n 次循环。
 *   feat_n == 0（弱模板：valid_n < 8 或网格分散选不出 8 个点）时直接
 *   跳过阶段 1。
 *
 *   阶段 2（全像素 SAD 累加）：累加 valid_n 个 valid 像素的 SAD，超过
 *   total_budget 立即返回 0。两阶段都通过则返回 1。
 *
 * 内存布局假设：
 *   - data：紧凑 RGBA8888 帧缓冲，stride = W * 4
 *   - t->rgb：紧凑 RGB 模板缓冲，stride = w * 3（已剥 alpha）
 *   - 模板偏移 t->valid_off[i] 已是 (vy * w + vx) * 3
 *   - t->valid_dxdy[i] 紧凑打包：低 16 位 vx，高 16 位 vy
 *   - 帧偏移计算：fp = data + (py + vy) * (W * 4) + (px + vx) * 4
 *
 * 调用契约（不在函数体内重复检查，由调用方保证）：
 *   - t != NULL 且经过 Images_LoadTemplate 构造，结构体不变量已建立
 *   - data != NULL 且至少 W * H * 4 字节可读
 *   - 0 <= px <= W - t->w，0 <= py <= H - t->h（扫描区已被收紧）
 *   - total_budget / feat_budget >= 0
 *
 * 性能权衡：
 *   - 把 W4 / rgb_base / off_arr / dxdy_arr / n hoist 到 const 局部变量，
 *     强迫编译器把它们缓存到寄存器，避免每次循环都走 t-> 解引用
 *   - 把 const uint8_t * 改成 const 指针，编译器可放心做别名分析
 *   - feat 阶段的 4 个变量与 valid 阶段命名分开，方便内联展开
 *
 * 返回值：1 = 命中（两阶段都通过），0 = 未命中（任一阶段早退） */
static int try_match(const uint8_t *data, int W, const EasyLuaTemplate *t,
                     int px, int py,
                     int64_t total_budget, int64_t feat_budget)
{
    const int W4 = W * 4;                       /* 帧行字节步长，hoist 到寄存器 */
    const uint8_t *const rgb_base = t->rgb;     /* 模板 RGB 起点 */

    /* ---- 阶段 1：feat_n 个特征点 SAD ---- */
    if (t->feat_n > 0) {
        const int feat_n = t->feat_n;
        const int32_t *const feat_off  = t->feat_off;
        const int32_t *const feat_dxdy = t->feat_dxdy;
        int64_t feat_sad = 0;

        for (int k = 0; k < feat_n; k++) {
            const int32_t pack = feat_dxdy[k];
            const int dx = (int)(pack & 0xFFFF);
            const int dy = (int)((pack >> 16) & 0xFFFF);

            const uint8_t *tp = rgb_base + feat_off[k];
            const uint8_t *fp = data + (py + dy) * W4 + (px + dx) * 4;

            /* SAD = |Δr| + |Δg| + |Δb|，单像素最大 3 * 255 = 765 */
            int dr = (int)tp[0] - (int)fp[0]; if (dr < 0) dr = -dr;
            int dg = (int)tp[1] - (int)fp[1]; if (dg < 0) dg = -dg;
            int db = (int)tp[2] - (int)fp[2]; if (db < 0) db = -db;
            feat_sad += (int64_t)(dr + dg + db);

            /* 早退：阶段 1 失败直接淘汰该候选起点 */
            if (feat_sad > feat_budget) return 0;
        }
    }

    /* ---- 阶段 2：valid_n 个 valid 像素 SAD ----
     * dense 模板（无透明像素）走 NEON 行扫描快路径，3-5x 加速；
     * 含透明像素的稀疏模板退回原 valid_off 路径。 */
#if EL_HAS_NEON
    if (t->is_dense) {
        return try_match_dense_sad_neon(data, W, t, px, py, total_budget);
    }
#endif
    {
        const int n = t->valid_n;
        const int32_t *const off_arr  = t->valid_off;
        const int32_t *const dxdy_arr = t->valid_dxdy;
        int64_t total_sad = 0;

        for (int i = 0; i < n; i++) {
            const int32_t pack = dxdy_arr[i];
            const int dx = (int)(pack & 0xFFFF);
            const int dy = (int)((pack >> 16) & 0xFFFF);

            const uint8_t *tp = rgb_base + off_arr[i];
            const uint8_t *fp = data + (py + dy) * W4 + (px + dx) * 4;

            int dr = (int)tp[0] - (int)fp[0]; if (dr < 0) dr = -dr;
            int dg = (int)tp[1] - (int)fp[1]; if (dg < 0) dg = -dg;
            int db = (int)tp[2] - (int)fp[2]; if (db < 0) db = -db;
            total_sad += (int64_t)(dr + dg + db);

            /* 早退：阶段 2 SAD 超阈值即放弃 */
            if (total_sad > total_budget) return 0;
        }
    }

    /* 两阶段都通过 → 命中 */
    return 1;
}

/* ---------- try_match_delta：色差模式单像素判定 + 比例早退 ----------
 *
 * 大漠 FindPic 经典语义：先按 (dr, dg, db) 判定单个像素是否"通过"，再
 * 按通过像素数 / valid_n >= sim 判定命中。
 *
 * 性能优化（与 SAD 模式对齐）：
 *   阶段 1（特征点预筛，feat_n > 0 时执行）：
 *     遍历 t->feat_n 个高对比特征点，统计其中"通过"个数；如果通过数 <
 *     feat_pass_budget，立即放弃该候选起点。这一步以 12 像素的常量开销
 *     淘汰绝大多数错位候选，避免阶段 2 跑满 valid_n 次循环。
 *   阶段 2（valid 像素全量判定）：
 *     遍历 valid_n 个像素，pass/fail 双向早退：
 *       - 通过累达 pass_budget → 立即返回 1
 *       - 失败累超 fail_budget → 立即返回 0
 *
 * feat_n == 0（弱模板）时阶段 1 跳过，与 SAD 模式行为一致。
 *
 * 性能预期：
 *   - 错位候选：阶段 1 在 12 像素内淘汰 ~ms 级
 *   - 命中候选：阶段 1 通过 + 阶段 2 一旦累达 pass_budget 立即返回
 *
 * 调用契约同 try_match：data 紧凑 RGBA8888，模板 valid_off / valid_dxdy
 * 已建立，px/py 已被扫描区收紧到合法范围，dr/dg/db ∈ [0, 255]。
 *
 * 返回值：1 = 命中，0 = 未命中。 */
static int try_match_delta(const uint8_t *data, int W, const EasyLuaTemplate *t,
                           int px, int py,
                           int dr, int dg, int db,
                           int pass_budget, int fail_budget,
                           int feat_pass_budget)
{
    const int W4 = W * 4;
    const uint8_t *const rgb_base = t->rgb;

    /* ---- 阶段 1：特征点预筛 ----
     * feat_n 个高对比像素全跑，统计通过数。错位候选大概率有大量特征点
     * 不通过，feat_pass_budget 留有余量（默认按 sim 比例计算），所以错位
     * 几乎必跌穿。 */
    if (t->feat_n > 0 && feat_pass_budget > 0) {
        const int feat_n = t->feat_n;
        const int32_t *const feat_off  = t->feat_off;
        const int32_t *const feat_dxdy = t->feat_dxdy;
        int feat_pass = 0;
        int feat_fail = 0;
        const int feat_fail_max = feat_n - feat_pass_budget;

        for (int k = 0; k < feat_n; k++) {
            const int32_t pack = feat_dxdy[k];
            const int dx = (int)(pack & 0xFFFF);
            const int dy = (int)((pack >> 16) & 0xFFFF);

            const uint8_t *tp = rgb_base + feat_off[k];
            const uint8_t *fp = data + (py + dy) * W4 + (px + dx) * 4;

            int er = (int)tp[0] - (int)fp[0]; if (er < 0) er = -er;
            int eg = (int)tp[1] - (int)fp[1]; if (eg < 0) eg = -eg;
            int eb = (int)tp[2] - (int)fp[2]; if (eb < 0) eb = -eb;

            if (er <= dr && eg <= dg && eb <= db) {
                feat_pass++;
            } else {
                feat_fail++;
                if (feat_fail > feat_fail_max) return 0;   /* 阶段 1 早退 */
            }
        }
        if (feat_pass < feat_pass_budget) return 0;
    }

    /* ---- 阶段 2：valid 像素全量判定 ----
     * dense 模板走 NEON 行扫描快路径；稀疏模板退回 valid_off 路径。 */
#if EL_HAS_NEON
    if (t->is_dense) {
        return try_match_dense_delta_neon(data, W, t, px, py,
                                          dr, dg, db,
                                          pass_budget, fail_budget);
    }
#endif
    {
        const int n = t->valid_n;
        const int32_t *const off_arr  = t->valid_off;
        const int32_t *const dxdy_arr = t->valid_dxdy;

        int pass = 0;
        int fail = 0;

        for (int i = 0; i < n; i++) {
            const int32_t pack = dxdy_arr[i];
            const int dx = (int)(pack & 0xFFFF);
            const int dy = (int)((pack >> 16) & 0xFFFF);

            const uint8_t *tp = rgb_base + off_arr[i];
            const uint8_t *fp = data + (py + dy) * W4 + (px + dx) * 4;

            int er = (int)tp[0] - (int)fp[0]; if (er < 0) er = -er;
            int eg = (int)tp[1] - (int)fp[1]; if (eg < 0) eg = -eg;
            int eb = (int)tp[2] - (int)fp[2]; if (eb < 0) eb = -eb;

            if (er <= dr && eg <= dg && eb <= db) {
                pass++;
                if (pass >= pass_budget) return 1;
            } else {
                fail++;
                if (fail > fail_budget) return 0;
            }
        }
        return (pass >= pass_budget) ? 1 : 0;
    }
}

/* ============================================================
 * NEON 行扫描快路径（仅 arm64，对全不透明模板生效）
 *
 * 触发条件：t->is_dense == 1（即 valid_n == w*h，模板没有任何透明像素）。
 * 对绝大多数实际素材（按钮 / 图标 / UI 元素）都成立。
 *
 * 算法：
 *   - 模板按行紧凑存放，stride = w*3
 *   - 帧也按行紧凑（RGBA8888，stride = W*4），起点 (px, py)
 *   - 每行用 NEON 一次比 16 像素：
 *       vld3q_u8 拿模板 16 像素的 r/g/b 三通道平面
 *       vld4q_u8 拿帧的 r/g/b/a 三通道（丢弃 a）
 *       vabdq_u8 算每通道绝对差
 *   - SAD 模式：把三通道差累加 → vaddvq_u16 → 16 位标量
 *   - 色差模式：vcleq_u8 与三个 dr/dg/db 比 → 三个 mask 与运算 → popcount
 *
 * 比阶段 2 走 valid_off 的好处：
 *   1) 顺序访问，硬件 prefetch 高效；
 *   2) 一次比 16 像素 vs 1 像素，理论 16x，实际受 vabdq + vaddvq 拖累 3-5x；
 *   3) 不需要解包 valid_dxdy 计算偏移。
 *
 * 阶段 1 仍用 try_match 的特征点快路径（12 像素已经够小，向量化收益有限），
 * 错位候选在阶段 1 就被淘汰，NEON 主要服务进入阶段 2 的"接近命中"候选。
 * ============================================================ */

#if EL_HAS_NEON

/* SAD 模式 dense 路径：返回 1 命中 / 0 未命中（含早退） */
static int try_match_dense_sad_neon(const uint8_t *data, int W,
                                    const EasyLuaTemplate *t,
                                    int px, int py,
                                    int64_t total_budget)
{
    const int W4 = W * 4;
    const int w = t->w;
    const int h = t->h;
    const uint8_t *const tpl_row0 = t->rgb;
    const int t_stride = w * 3;

    int64_t total_sad = 0;

    for (int y = 0; y < h; y++) {
        const uint8_t *tp = tpl_row0 + y * t_stride;
        const uint8_t *fp = data + (py + y) * W4 + px * 4;

        int x = 0;
        /* 16 像素一组 */
        for (; x + 16 <= w; x += 16) {
            uint8x16x3_t tt = vld3q_u8(tp + x * 3);
            uint8x16x4_t ff = vld4q_u8(fp + x * 4);
            uint8x16_t dr = vabdq_u8(tt.val[0], ff.val[0]);
            uint8x16_t dg = vabdq_u8(tt.val[1], ff.val[1]);
            uint8x16_t db = vabdq_u8(tt.val[2], ff.val[2]);
            /* 16 个字节差 → 8 个 16 位累加，避免溢出 */
            uint16x8_t s_lo = vaddl_u8(vget_low_u8(dr), vget_low_u8(dg));
            uint16x8_t s_hi = vaddl_u8(vget_high_u8(dr), vget_high_u8(dg));
            s_lo = vaddw_u8(s_lo, vget_low_u8(db));
            s_hi = vaddw_u8(s_hi, vget_high_u8(db));
            uint32_t row_sad = (uint32_t)vaddvq_u16(s_lo) + (uint32_t)vaddvq_u16(s_hi);
            total_sad += (int64_t)row_sad;
            if (total_sad > total_budget) return 0;
        }
        /* 行末尾 < 16 像素的 tail，标量 */
        for (; x < w; x++) {
            const uint8_t *p = tp + x * 3;
            const uint8_t *q = fp + x * 4;
            int er = (int)p[0] - (int)q[0]; if (er < 0) er = -er;
            int eg = (int)p[1] - (int)q[1]; if (eg < 0) eg = -eg;
            int eb = (int)p[2] - (int)q[2]; if (eb < 0) eb = -eb;
            total_sad += (int64_t)(er + eg + eb);
            if (total_sad > total_budget) return 0;
        }
    }
    return 1;
}

/* 色差模式 dense 路径：通过 vcleq_u8 + popcount 计数通过像素 */
static int try_match_dense_delta_neon(const uint8_t *data, int W,
                                      const EasyLuaTemplate *t,
                                      int px, int py,
                                      int dr_thr, int dg_thr, int db_thr,
                                      int pass_budget, int fail_budget)
{
    const int W4 = W * 4;
    const int w = t->w;
    const int h = t->h;
    const uint8_t *const tpl_row0 = t->rgb;
    const int t_stride = w * 3;

    const uint8x16_t v_dr = vdupq_n_u8((uint8_t)dr_thr);
    const uint8x16_t v_dg = vdupq_n_u8((uint8_t)dg_thr);
    const uint8x16_t v_db = vdupq_n_u8((uint8_t)db_thr);
    /* vcleq_u8 在每字节通过时给 0xFF（值 = 1 在 popcount 语义下；后面除 8） */

    int pass = 0;
    int fail = 0;

    for (int y = 0; y < h; y++) {
        const uint8_t *tp = tpl_row0 + y * t_stride;
        const uint8_t *fp = data + (py + y) * W4 + px * 4;

        int x = 0;
        for (; x + 16 <= w; x += 16) {
            uint8x16x3_t tt = vld3q_u8(tp + x * 3);
            uint8x16x4_t ff = vld4q_u8(fp + x * 4);
            uint8x16_t dr = vabdq_u8(tt.val[0], ff.val[0]);
            uint8x16_t dg = vabdq_u8(tt.val[1], ff.val[1]);
            uint8x16_t db = vabdq_u8(tt.val[2], ff.val[2]);
            uint8x16_t mr = vcleq_u8(dr, v_dr);
            uint8x16_t mg = vcleq_u8(dg, v_dg);
            uint8x16_t mb = vcleq_u8(db, v_db);
            uint8x16_t m  = vandq_u8(vandq_u8(mr, mg), mb);
            /* m 中每个字节是 0x00 或 0xFF；vaddvq_u8 求和 ÷ 255 = 通过数 */
            uint32_t sum = (uint32_t)vaddvq_u8(m);
            int row_pass = (int)(sum / 255u);
            int row_fail = 16 - row_pass;
            pass += row_pass;
            fail += row_fail;
            if (fail > fail_budget) return 0;
            if (pass >= pass_budget) return 1;
        }
        /* tail 标量 */
        for (; x < w; x++) {
            const uint8_t *p = tp + x * 3;
            const uint8_t *q = fp + x * 4;
            int er = (int)p[0] - (int)q[0]; if (er < 0) er = -er;
            int eg = (int)p[1] - (int)q[1]; if (eg < 0) eg = -eg;
            int eb = (int)p[2] - (int)q[2]; if (eb < 0) eb = -eb;
            if (er <= dr_thr && eg <= dg_thr && eb <= db_thr) {
                pass++;
                if (pass >= pass_budget) return 1;
            } else {
                fail++;
                if (fail > fail_budget) return 0;
            }
        }
    }
    return (pass >= pass_budget) ? 1 : 0;
}

#endif /* EL_HAS_NEON */

/* ---------- API ---------- */

/* ---------- 找图模板：释放与元信息访问 ----------
 *
 * Images_LoadTemplate 返回的 EasyLuaTemplate 由调用方持有；Lua 端通过
 * cdata + ffi.gc 把 Images_FreeTemplate 绑定为 GC 回调，离开作用域时
 * 自动归还内存。所有元信息访问函数（W / H / ValidPx）在 t == NULL 时
 * 都会安全返回 0，避免脚本里偶发空指针时崩溃 native，给调用方一个
 * 平凡可观察的"空模板"语义。 */

void Images_FreeTemplate(EasyLuaTemplate *t)
{
    /* design.md「Images_FreeTemplate」契约：t == NULL 时 no-op，否则
     * 按 rgb → valid_off → valid_dxdy → 结构体本身的顺序释放。
     * free(NULL) 在 C 标准里就是 no-op，子缓冲分配失败的早返回路径
     * 绝不会走到这里（那条路径自己就负责清理），所以这里直接 free
     * 即可，不需要再判 NULL。 */
    if (!t) return;
    free(t->rgb);
    free(t->valid_off);
    free(t->valid_dxdy);
    free(t);
}

int Images_TemplateW(const EasyLuaTemplate *t)
{
    /* 防御性返回 0：t == NULL 时不解引用，方便 Lua 端 :Width() 在
     * tpl 已被显式 :Free() 后仍能拿到一个明确的"空"值。 */
    return t ? t->w : 0;
}

int Images_TemplateH(const EasyLuaTemplate *t)
{
    return t ? t->h : 0;
}

int Images_TemplateValidPx(const EasyLuaTemplate *t)
{
    return t ? t->valid_n : 0;
}

/* ---------- Images_FindPic：单次找图主循环 ----------
 *
 * 严格按 design.md「Algorithm 4: Images_FindPic」实现，是 Lua 端
 * `FindPic(x1, y1, x2, y2, tpl, sim, dir)` 的 C 端入口。给定扫描区与
 * 已加载模板，返回模板左上角在帧上的命中坐标。
 *
 * 主要步骤：
 *   1) 入口写 *out_x = *out_y = -1，便于调用方无脑读返回坐标；
 *   2) 取锁前早返回：t == NULL 或 dir ∉ {0,1,2,3} 直接返回 -1，不锁帧；
 *   3) EasyLua_FrameDataLocked 取锁；data == NULL（视频流首帧未到 / 超时）
 *      时仅做配对 unlock + return -1；
 *   4) normalize_rect 把 x2/y2 == 0 扩为整帧；
 *   5) 装不下检查：x2 - t->w < x1 或 y2 - t->h < y1 → EL_ERR + return -1；
 *   6) 收紧扫描区：x2 = x2 - t->w + 1, y2 = y2 - t->h + 1，扫描的是模板
 *      「左上角」起点的开区间右边界；
 *   7) 阈值预算：
 *        total_budget = (int64)((1 - sim) * valid_n * 765)
 *        feat_budget  = (int64)((1 - sim) * feat_n  * 765 * 1.5)
 *      feat_n == 0 时 feat_budget = 0 且 try_match 内部直接跳过阶段 1；
 *   8) 按 dir 顺序遍历候选起点，命中即写 out_x/out_y、跳到 done unlock。
 *
 * 不变量：所有从 EasyLua_FrameDataLocked 之后的返回路径都通过 done 标签
 * 统一 EasyLua_FrameUnlock 一次，绝不会忘记或重复 unlock。 */
int Images_FindPic(int x1, int y1, int x2, int y2,
                   const EasyLuaTemplate *t, float sim, int dir,
                   int *out_x, int *out_y)
{
    /* 1) 默认未命中坐标，调用方即使忽略返回值也能拿到 -1 */
    if (out_x) *out_x = -1;
    if (out_y) *out_y = -1;

    /* 2) 取锁前的早返回：避免 t / dir 非法时还白白取锁 */
    if (!t) return -1;
    if (dir < 0 || dir > 3) return -1;

    /* 3) 取帧锁；data == NULL 时直接走 done 配对 unlock */
    int W, H;
    const uint8_t *data = EasyLua_FrameDataLocked(&W, &H);
    int found = -1;
    if (data) {
        /* 4) 区域规整：x2/y2 == 0 → 屏幕边界 */
        normalize_rect(&x1, &y1, &x2, &y2, W, H);

        /* 5) 装不下检查：扫描区的可用宽 / 高 < 模板对应边 → 失败 */
        if (x2 - t->w < x1 || y2 - t->h < y1) {
            EL_ERR("FindPic: 扫描区 (%d,%d)-(%d,%d) 装不下模板 %dx%d",
                   x1, y1, x2, y2, t->w, t->h);
            goto done;
        }

        /* 6) 收紧扫描区：扫描的是模板左上角起点。这里把右下边界从
         *    「区域右下角」推回「最后一个合法起点 + 1」，使得后续循环
         *    用 `< x2` / `< y2` 即可，无需再做越界判断。 */
        x2 = x2 - t->w + 1;
        y2 = y2 - t->h + 1;

        /* 7) 阈值预算（design.md「Algorithm 4」）。765 = 255 * 3，是单像素
         *    最大 SAD；阶段 1 的 1.5x 宽容防止特征点恰好落在边缘抖动处。 */
        const int64_t total_budget =
            (int64_t)((1.0f - sim) * (float)t->valid_n * 765.0f);
        const int64_t feat_budget = (t->feat_n > 0)
            ? (int64_t)((1.0f - sim) * (float)t->feat_n * 765.0f * 1.5f)
            : 0;

        /* 8a) dir == 0：左→右、上→下，使用递增 for 便于编译器自动向量化，
         *     与 FindColor / FindMultiColors 的快路径风格一致。 */
        if (dir == 0) {
            for (int py = y1; py < y2; py++) {
                for (int px = x1; px < x2; px++) {
                    if (try_match(data, W, t, px, py,
                                  total_budget, feat_budget)) {
                        if (out_x) *out_x = px;
                        if (out_y) *out_y = py;
                        found = 0;
                        goto done;
                    }
                }
            }
            goto done;
        }

        /* 8b) dir 1/2/3：与 Images_FindColor / FindMultiColors 完全一致
         *     的反向起止边界，使「dir 命中等价性」（Property 5）天然成立。
         *       dir = 1：R→L, T→B
         *       dir = 2：L→R, B→T
         *       dir = 3：R→L, B→T */
        int sx = (dir == 1 || dir == 3) ? x2 - 1 : x1;
        int ex = (dir == 1 || dir == 3) ? x1 - 1 : x2;
        int dx = (dir == 1 || dir == 3) ? -1 : 1;
        int sy = (dir == 2 || dir == 3) ? y2 - 1 : y1;
        int ey = (dir == 2 || dir == 3) ? y1 - 1 : y2;
        int dy = (dir == 2 || dir == 3) ? -1 : 1;

        for (int py = sy; py != ey; py += dy) {
            for (int px = sx; px != ex; px += dx) {
                if (try_match(data, W, t, px, py,
                              total_budget, feat_budget)) {
                    if (out_x) *out_x = px;
                    if (out_y) *out_y = py;
                    found = 0;
                    goto done;
                }
            }
        }
    }
done:
    /* 9) 配对 unlock：与上面 EasyLua_FrameDataLocked 一一对应。
     *    data == NULL 时也走这一条，保证锁状态零泄漏。 */
    EasyLua_FrameUnlock();
    return found;
}

/* ---------- Images_FindPicAll：找全部 + 屏蔽矩形 ----------
 *
 * 严格按 design.md「Algorithm 5: Images_FindPicAll」实现，是 Lua 端
 * `FindPicAll(x1, y1, x2, y2, tpl, sim)` 的 C 端入口。在扫描区内寻找
 * 模板的所有命中位置，并通过屏蔽矩形避免在同一目标的「势力范围」内
 * 重复命中。
 *
 * 与 Images_FindPic 的差异：
 *   - 参数 max_n 限定最多写入的命中点数（不是 int 数）；
 *   - 不接受 dir 参数，默认按 dir == 0（左→右、上→下）顺序扫描，与
 *     Images_FindMultiColorsAll 的快路径风格一致；
 *   - 命中后追加 (px, py, px + t->w - 1, py + t->h - 1) 到屏蔽矩形栈，
 *     后续候选起点若落在任一屏蔽矩形内立即跳过 try_match；
 *   - 屏蔽矩形数量上限 MAX_MASKS = 256（与 FindMultiColorsAll 对齐）。
 *     达到上限后不再追加新矩形，但仍继续扫描以写满剩余 max_n。
 *
 * 不变量：n_masks == count（当 count <= MAX_MASKS）；任意 i < j < count，
 * (out_xy[j*2], out_xy[j*2+1]) 不在 masks[0..i] 中任一矩形内。
 *
 * 返回实际写入的点数（>= 0）。out_xy == NULL / max_n <= 0 / t == NULL /
 * 帧未就绪 / 扫描区装不下模板时返回 0。 */
int Images_FindPicAll(int x1, int y1, int x2, int y2,
                      const EasyLuaTemplate *t, float sim,
                      int *out_xy, int max_n)
{
    /* 1) 参数防御：design.md Algorithm 5 step 1。
     *    out_xy / max_n 非法时不取锁直接返回 0；t == NULL 同样早返回，
     *    防止后续 try_match 解引用空指针。 */
    if (!out_xy || max_n <= 0) return 0;
    if (!t) return 0;

    /* 2) 取帧锁；data == NULL 时走 done 配对 unlock + 返回 count = 0 */
    int W, H;
    const uint8_t *data = EasyLua_FrameDataLocked(&W, &H);
    int count = 0;

    /* 屏蔽矩形：栈上 256 个，避免堆分配。MAX_MASKS 与
     * Images_FindMultiColorsAll 对齐，便于运维理解一致的容量约束。
     * 命中数若超过 MAX_MASKS，新命中仍会写入 out_xy，只是不再追加新
     * 屏蔽矩形（旧矩形仍生效，保证已记录区域不会重复命中）。 */
    enum { MAX_MASKS = 256 };
    MaskRect masks[MAX_MASKS];
    int n_masks = 0;

    if (data) {
        /* 3) 区域规整：x2/y2 == 0 → 屏幕边界 */
        normalize_rect(&x1, &y1, &x2, &y2, W, H);

        /* 4) 装不下检查：扫描区可用宽 / 高 < 模板对应边 → 返回 0 */
        if (x2 - t->w < x1 || y2 - t->h < y1) {
            EL_ERR("FindPicAll: 扫描区 (%d,%d)-(%d,%d) 装不下模板 %dx%d",
                   x1, y1, x2, y2, t->w, t->h);
            goto done;
        }

        /* 5) 收紧扫描区：扫描的是模板左上角起点，循环用 `< x2` / `< y2`
         *    即可，无需再做越界判断（与 Images_FindPic 同一惯例）。 */
        x2 = x2 - t->w + 1;
        y2 = y2 - t->h + 1;

        /* 6) 阈值预算（与 Images_FindPic 完全相同的公式，确保
         *    「Property 5: dir 命中等价性」与 FindPic 单次命中天然一致）。 */
        const int64_t total_budget =
            (int64_t)((1.0f - sim) * (float)t->valid_n * 765.0f);
        const int64_t feat_budget = (t->feat_n > 0)
            ? (int64_t)((1.0f - sim) * (float)t->feat_n * 765.0f * 1.5f)
            : 0;

        /* 7) 主扫描循环（dir == 0）：左→右、上→下。
         *    外层 / 内层都用 `count < max_n` 双重短路，达到 max_n 后
         *    立即 break 双层循环，不再扫描剩余像素。 */
        for (int py = y1; py < y2 && count < max_n; py++) {
            for (int px = x1; px < x2 && count < max_n; px++) {
                /* 7a) 屏蔽矩形检查：落在任一已命中矩形内则跳过本起点。
                 *     这是「FindPicAll 不返回重叠点」的核心保证。 */
                if (in_any_mask(px, py, masks, n_masks)) continue;

                /* 7b) 两段式 SAD：与 Images_FindPic 共用 try_match。 */
                if (try_match(data, W, t, px, py,
                              total_budget, feat_budget)) {
                    /* 7c) 命中：写入 out_xy 并追加屏蔽矩形 */
                    out_xy[count * 2 + 0] = px;
                    out_xy[count * 2 + 1] = py;
                    count++;

                    /* 7d) 屏蔽矩形栈未满才追加；满了之后旧矩形仍生效，
                     *     保证已记录区域不会被重复命中（达到上限属于
                     *     极端密集帧的退化场景，行为与
                     *     Images_FindMultiColorsAll 完全一致）。
                     *
                     *     屏蔽矩形向四个方向各扩 (t->w - 1) / (t->h - 1)：
                     *     这样任何后续候选起点 (px', py')，只要它的模板
                     *     bbox 与已命中目标的 bbox 有重叠（即视觉上是同
                     *     一个目标），就会被 in_any_mask 拒绝。
                     *     朴素的 [px, py, px+w-1, py+h-1] 只能挡住"起点
                     *     落在 bbox 内"的情况，挡不住"起点在 bbox 上方
                     *     或左侧、但 bbox 仍重叠"的子像素抖动命中——
                     *     真机上一个目标会被算成 2~5 个相邻命中点。 */
                    if (n_masks < MAX_MASKS) {
                        masks[n_masks].l = px - (t->w - 1);
                        masks[n_masks].t = py - (t->h - 1);
                        masks[n_masks].r = px + (t->w - 1);
                        masks[n_masks].b = py + (t->h - 1);
                        n_masks++;
                    }
                }
            }
        }
    }
done:
    /* 8) 配对 unlock：所有从 EasyLua_FrameDataLocked 之后的返回路径
     *    （含 data == NULL、装不下模板、扫描完毕）都走这里 unlock 一次。 */
    EasyLua_FrameUnlock();
    return count;
}

/* ---------- Images_FindPicDelta：找图（色差模式，大漠兼容） ----------
 *
 * 与 Images_FindPic 仅在单像素判定层不同：使用 try_match_delta 替代
 * 两段式 SAD。色差阈值 (dr, dg, db) 来自 6 位 hex（"RRGGBB"）；通过
 * 比例 sim ∈ [0, 1] 决定多少 valid 像素必须在阈值内。其余流程（取锁、
 * 区域规整、装不下检查、收紧扫描区、dir 扫描、配对 unlock）与
 * Images_FindPic 完全一致。 */
int Images_FindPicDelta(int x1, int y1, int x2, int y2,
                        const EasyLuaTemplate *t,
                        int dr, int dg, int db,
                        float sim, int dir,
                        int *out_x, int *out_y)
{
    if (out_x) *out_x = -1;
    if (out_y) *out_y = -1;

    if (!t) return -1;
    if (dir < 0 || dir > 3) return -1;
    /* 色差通道阈值钳到 [0, 255] */
    if (dr < 0) dr = 0;
    if (dr > 255) dr = 255;
    if (dg < 0) dg = 0;
    if (dg > 255) dg = 255;
    if (db < 0) db = 0;
    if (db > 255) db = 255;
    if (sim < 0.0f) sim = 0.0f;
    if (sim > 1.0f) sim = 1.0f;

    int W, H;
    const uint8_t *data = EasyLua_FrameDataLocked(&W, &H);
    int found = -1;
    if (data) {
        normalize_rect(&x1, &y1, &x2, &y2, W, H);

        if (x2 - t->w < x1 || y2 - t->h < y1) {
            EL_ERR("FindPicDelta: 扫描区 (%d,%d)-(%d,%d) 装不下模板 %dx%d",
                   x1, y1, x2, y2, t->w, t->h);
            goto done_d;
        }

        x2 = x2 - t->w + 1;
        y2 = y2 - t->h + 1;

        /* 通过/失败像素预算（基于通过率 sim） */
        const int n        = t->valid_n;
        int pass_budget    = (int)((float)n * sim + 0.999999f); /* ceil */
        if (pass_budget < 1)     pass_budget = 1;
        if (pass_budget > n)     pass_budget = n;
        const int fail_budget = n - pass_budget;
        /* 阶段 1 特征点 pass 阈值：与 sim 同比例，但放宽一档（×0.9）
         * 避免边角抖动误杀真实命中 */
        int feat_pass_budget = 0;
        if (t->feat_n > 0) {
            float ratio = sim * 0.9f;
            feat_pass_budget = (int)((float)t->feat_n * ratio + 0.5f);
            if (feat_pass_budget < 1) feat_pass_budget = 1;
            if (feat_pass_budget > t->feat_n) feat_pass_budget = t->feat_n;
        }

        if (dir == 0) {
            for (int py = y1; py < y2; py++) {
                for (int px = x1; px < x2; px++) {
                    if (try_match_delta(data, W, t, px, py,
                                        dr, dg, db,
                                        pass_budget, fail_budget,
                                        feat_pass_budget)) {
                        if (out_x) *out_x = px;
                        if (out_y) *out_y = py;
                        found = 0;
                        goto done_d;
                    }
                }
            }
            goto done_d;
        }

        int sx = (dir == 1 || dir == 3) ? x2 - 1 : x1;
        int ex = (dir == 1 || dir == 3) ? x1 - 1 : x2;
        int dxs = (dir == 1 || dir == 3) ? -1 : 1;
        int sy = (dir == 2 || dir == 3) ? y2 - 1 : y1;
        int ey = (dir == 2 || dir == 3) ? y1 - 1 : y2;
        int dys = (dir == 2 || dir == 3) ? -1 : 1;

        for (int py = sy; py != ey; py += dys) {
            for (int px = sx; px != ex; px += dxs) {
                if (try_match_delta(data, W, t, px, py,
                                    dr, dg, db,
                                    pass_budget, fail_budget,
                                    feat_pass_budget)) {
                    if (out_x) *out_x = px;
                    if (out_y) *out_y = py;
                    found = 0;
                    goto done_d;
                }
            }
        }
    }
done_d:
    EasyLua_FrameUnlock();
    return found;
}

/* ---------- Images_FindPicAllDelta：色差模式找全部 ----------
 *
 * 与 Images_FindPicAll 完全对齐，仅把 try_match 换成 try_match_delta，
 * 并在 mask 矩形 / 屏蔽逻辑上保持一致：四向扩展 (t->w-1, t->h-1)，
 * 防止子像素抖动重复命中。 */
int Images_FindPicAllDelta(int x1, int y1, int x2, int y2,
                           const EasyLuaTemplate *t,
                           int dr, int dg, int db,
                           float sim,
                           int *out_xy, int max_n)
{
    if (!out_xy || max_n <= 0) return 0;
    if (!t) return 0;
    if (dr < 0) dr = 0;
    if (dr > 255) dr = 255;
    if (dg < 0) dg = 0;
    if (dg > 255) dg = 255;
    if (db < 0) db = 0;
    if (db > 255) db = 255;
    if (sim < 0.0f) sim = 0.0f;
    if (sim > 1.0f) sim = 1.0f;

    int W, H;
    const uint8_t *data = EasyLua_FrameDataLocked(&W, &H);
    int count = 0;

    enum { MAX_MASKS = 256 };
    MaskRect masks[MAX_MASKS];
    int n_masks = 0;

    if (data) {
        normalize_rect(&x1, &y1, &x2, &y2, W, H);

        if (x2 - t->w < x1 || y2 - t->h < y1) {
            EL_ERR("FindPicAllDelta: 扫描区 (%d,%d)-(%d,%d) 装不下模板 %dx%d",
                   x1, y1, x2, y2, t->w, t->h);
            goto done_da;
        }

        x2 = x2 - t->w + 1;
        y2 = y2 - t->h + 1;

        const int n        = t->valid_n;
        int pass_budget    = (int)((float)n * sim + 0.999999f);
        if (pass_budget < 1)     pass_budget = 1;
        if (pass_budget > n)     pass_budget = n;
        const int fail_budget = n - pass_budget;
        int feat_pass_budget = 0;
        if (t->feat_n > 0) {
            float ratio = sim * 0.9f;
            feat_pass_budget = (int)((float)t->feat_n * ratio + 0.5f);
            if (feat_pass_budget < 1) feat_pass_budget = 1;
            if (feat_pass_budget > t->feat_n) feat_pass_budget = t->feat_n;
        }

        for (int py = y1; py < y2 && count < max_n; py++) {
            for (int px = x1; px < x2 && count < max_n; px++) {
                if (in_any_mask(px, py, masks, n_masks)) continue;

                if (try_match_delta(data, W, t, px, py,
                                    dr, dg, db,
                                    pass_budget, fail_budget,
                                    feat_pass_budget)) {
                    out_xy[count * 2 + 0] = px;
                    out_xy[count * 2 + 1] = py;
                    count++;

                    if (n_masks < MAX_MASKS) {
                        /* 四向各扩 (w-1, h-1)，与 Images_FindPicAll 一致 */
                        masks[n_masks].l = px - (t->w - 1);
                        masks[n_masks].t = py - (t->h - 1);
                        masks[n_masks].r = px + (t->w - 1);
                        masks[n_masks].b = py + (t->h - 1);
                        n_masks++;
                    }
                }
            }
        }
    }
done_da:
    EasyLua_FrameUnlock();
    return count;
}

/* ============================================================
 * 多模板找图（C 内单次扫描，命中任一模板即返回 / 收集）
 *
 * 设计要点：
 *   - 单次取锁 / 单次区域规整 / 候选起点轮询所有模板，避免 Lua 层多次
 *     调 FindPic 的 N 倍取锁开销；
 *   - 收紧扫描区基于"最大模板尺寸"，单个模板在边角被装不下时仍可被
 *     轮询到（小模板可能在大模板装不下的边角命中）；
 *   - 屏蔽矩形按本次命中模板的 (w-1, h-1) 向四周扩，避免子像素抖动重复
 *     命中——与单模板 FindPicAll 行为一致；
 *   - 预算（total / feat / pass / fail / feat_pass）每个模板独立计算后
 *     缓存到 budget 数组，避免每个候选起点重算。
 * ============================================================ */

/* 单模板 SAD 预算 */
typedef struct {
    int64_t total;     /* (1-sim) * valid_n * 765 */
    int64_t feat;      /* (1-sim) * feat_n  * 765 * 1.5；feat_n=0 时为 0 */
} SadBudget;

/* 单模板色差预算 */
typedef struct {
    int pass;          /* ceil(sim * valid_n) */
    int fail;          /* valid_n - pass */
    int feat_pass;     /* sim*0.9 * feat_n；feat_n=0 时为 0 */
} DeltaBudget;

/* 计算 SAD 预算 */
static void calc_sad_budget(const EasyLuaTemplate *t, float sim, SadBudget *out)
{
    out->total = (int64_t)((1.0f - sim) * (float)t->valid_n * 765.0f);
    out->feat = (t->feat_n > 0)
        ? (int64_t)((1.0f - sim) * (float)t->feat_n * 765.0f * 1.5f) : 0;
}

/* 计算色差预算 */
static void calc_delta_budget(const EasyLuaTemplate *t, float sim, DeltaBudget *out)
{
    int n = t->valid_n;
    int pass = (int)((float)n * sim + 0.999999f);
    if (pass < 1) pass = 1;
    if (pass > n) pass = n;
    out->pass = pass;
    out->fail = n - pass;
    if (t->feat_n > 0) {
        float ratio = sim * 0.9f;
        int fp = (int)((float)t->feat_n * ratio + 0.5f);
        if (fp < 1) fp = 1;
        if (fp > t->feat_n) fp = t->feat_n;
        out->feat_pass = fp;
    } else {
        out->feat_pass = 0;
    }
}

/* 计算所有模板里最大的 w / h，用于扫描区收紧与屏蔽矩形扩展 */
static void calc_multi_max_wh(const EasyLuaTemplate * const *tpls, int n_tpls,
                              int *max_w, int *max_h)
{
    int mw = 0, mh = 0;
    for (int i = 0; i < n_tpls; i++) {
        if (!tpls[i]) continue;
        if (tpls[i]->w > mw) mw = tpls[i]->w;
        if (tpls[i]->h > mh) mh = tpls[i]->h;
    }
    *max_w = mw;
    *max_h = mh;
}

/* 候选起点 (px, py) 处轮询所有模板，命中即写 *hit_idx 并返回 1。
 * 由调用方保证 px/py 已经被收紧到 max(w,h) 范围内；单个模板若在该
 * 起点装不下（px + t->w > frame_w 或 py + t->h > frame_h）会被跳过。 */
static int try_match_multi_sad(const uint8_t *data, int W, int H,
                               const EasyLuaTemplate * const *tpls, int n_tpls,
                               const SadBudget *budgets,
                               int px, int py, int *hit_idx)
{
    for (int i = 0; i < n_tpls; i++) {
        const EasyLuaTemplate *t = tpls[i];
        if (!t) continue;
        if (px + t->w > W || py + t->h > H) continue;   /* 该模板装不下 */
        if (try_match(data, W, t, px, py,
                      budgets[i].total, budgets[i].feat)) {
            *hit_idx = i;
            return 1;
        }
    }
    return 0;
}

static int try_match_multi_delta(const uint8_t *data, int W, int H,
                                 const EasyLuaTemplate * const *tpls, int n_tpls,
                                 const DeltaBudget *budgets,
                                 int dr, int dg, int db,
                                 int px, int py, int *hit_idx)
{
    for (int i = 0; i < n_tpls; i++) {
        const EasyLuaTemplate *t = tpls[i];
        if (!t) continue;
        if (px + t->w > W || py + t->h > H) continue;
        if (try_match_delta(data, W, t, px, py, dr, dg, db,
                            budgets[i].pass, budgets[i].fail,
                            budgets[i].feat_pass)) {
            *hit_idx = i;
            return 1;
        }
    }
    return 0;
}

/* ---------- Images_FindPicMulti（SAD）---------- */
int Images_FindPicMulti(int x1, int y1, int x2, int y2,
                        const EasyLuaTemplate * const *tpls, int n_tpls,
                        float sim, int dir,
                        int *out_x, int *out_y, int *out_idx)
{
    if (out_x) *out_x = -1;
    if (out_y) *out_y = -1;
    if (out_idx) *out_idx = -1;

    if (!tpls || n_tpls <= 0) return -1;
    if (dir < 0 || dir > 3) return -1;

    /* 至少要有一个非 NULL 模板 */
    int valid_tpls = 0;
    for (int i = 0; i < n_tpls; i++) if (tpls[i]) valid_tpls++;
    if (valid_tpls == 0) return -1;

    int W, H;
    const uint8_t *data = EasyLua_FrameDataLocked(&W, &H);
    int found = -1;
    if (data) {
        normalize_rect(&x1, &y1, &x2, &y2, W, H);

        int max_w, max_h;
        calc_multi_max_wh(tpls, n_tpls, &max_w, &max_h);
        if (x2 - max_w < x1 || y2 - max_h < y1) {
            EL_ERR("FindPicMulti: 扫描区 (%d,%d)-(%d,%d) 装不下任何模板 (max %dx%d)",
                   x1, y1, x2, y2, max_w, max_h);
            goto done_pm;
        }

        /* 收紧到"最小模板装得下"的范围；单个大模板装不下的起点会在
         * try_match_multi_sad 内部被本模板 skip，但小模板仍可命中 */
        int min_w = 0x7FFFFFFF, min_h = 0x7FFFFFFF;
        for (int i = 0; i < n_tpls; i++) {
            if (!tpls[i]) continue;
            if (tpls[i]->w < min_w) min_w = tpls[i]->w;
            if (tpls[i]->h < min_h) min_h = tpls[i]->h;
        }
        x2 = x2 - min_w + 1;
        y2 = y2 - min_h + 1;

        /* 预算 */
        SadBudget budgets[16];
        SadBudget *budgets_ptr = budgets;
        SadBudget *budgets_heap = NULL;
        if (n_tpls > 16) {
            budgets_heap = (SadBudget *)malloc(sizeof(SadBudget) * n_tpls);
            if (!budgets_heap) goto done_pm;
            budgets_ptr = budgets_heap;
        }
        for (int i = 0; i < n_tpls; i++) {
            if (tpls[i]) calc_sad_budget(tpls[i], sim, &budgets_ptr[i]);
        }

        int hit_idx = -1;
        if (dir == 0) {
            for (int py = y1; py < y2; py++) {
                for (int px = x1; px < x2; px++) {
                    if (try_match_multi_sad(data, W, H, tpls, n_tpls,
                                            budgets_ptr, px, py, &hit_idx)) {
                        if (out_x) *out_x = px;
                        if (out_y) *out_y = py;
                        if (out_idx) *out_idx = hit_idx;
                        found = 0;
                        if (budgets_heap) free(budgets_heap);
                        goto done_pm;
                    }
                }
            }
        } else {
            int sx = (dir == 1 || dir == 3) ? x2 - 1 : x1;
            int ex = (dir == 1 || dir == 3) ? x1 - 1 : x2;
            int dxs = (dir == 1 || dir == 3) ? -1 : 1;
            int sy = (dir == 2 || dir == 3) ? y2 - 1 : y1;
            int ey = (dir == 2 || dir == 3) ? y1 - 1 : y2;
            int dys = (dir == 2 || dir == 3) ? -1 : 1;
            for (int py = sy; py != ey; py += dys) {
                for (int px = sx; px != ex; px += dxs) {
                    if (try_match_multi_sad(data, W, H, tpls, n_tpls,
                                            budgets_ptr, px, py, &hit_idx)) {
                        if (out_x) *out_x = px;
                        if (out_y) *out_y = py;
                        if (out_idx) *out_idx = hit_idx;
                        found = 0;
                        if (budgets_heap) free(budgets_heap);
                        goto done_pm;
                    }
                }
            }
        }
        if (budgets_heap) free(budgets_heap);
    }
done_pm:
    EasyLua_FrameUnlock();
    return found;
}

/* ---------- Images_FindPicAllMulti（SAD）---------- */
int Images_FindPicAllMulti(int x1, int y1, int x2, int y2,
                           const EasyLuaTemplate * const *tpls, int n_tpls,
                           float sim,
                           int *out_xy, int *out_idxs, int max_n)
{
    if (!out_xy || !out_idxs || max_n <= 0) return 0;
    if (!tpls || n_tpls <= 0) return 0;

    int valid_tpls = 0;
    for (int i = 0; i < n_tpls; i++) if (tpls[i]) valid_tpls++;
    if (valid_tpls == 0) return 0;

    int W, H;
    const uint8_t *data = EasyLua_FrameDataLocked(&W, &H);
    int count = 0;

    enum { MAX_MASKS = 256 };
    MaskRect masks[MAX_MASKS];
    int n_masks = 0;

    if (data) {
        normalize_rect(&x1, &y1, &x2, &y2, W, H);

        int max_w, max_h;
        calc_multi_max_wh(tpls, n_tpls, &max_w, &max_h);
        if (x2 - max_w < x1 || y2 - max_h < y1) {
            EL_ERR("FindPicAllMulti: 扫描区 (%d,%d)-(%d,%d) 装不下任何模板 (max %dx%d)",
                   x1, y1, x2, y2, max_w, max_h);
            goto done_pam;
        }

        int min_w = 0x7FFFFFFF, min_h = 0x7FFFFFFF;
        for (int i = 0; i < n_tpls; i++) {
            if (!tpls[i]) continue;
            if (tpls[i]->w < min_w) min_w = tpls[i]->w;
            if (tpls[i]->h < min_h) min_h = tpls[i]->h;
        }
        x2 = x2 - min_w + 1;
        y2 = y2 - min_h + 1;

        SadBudget budgets[16];
        SadBudget *budgets_ptr = budgets;
        SadBudget *budgets_heap = NULL;
        if (n_tpls > 16) {
            budgets_heap = (SadBudget *)malloc(sizeof(SadBudget) * n_tpls);
            if (!budgets_heap) goto done_pam;
            budgets_ptr = budgets_heap;
        }
        for (int i = 0; i < n_tpls; i++) {
            if (tpls[i]) calc_sad_budget(tpls[i], sim, &budgets_ptr[i]);
        }

        for (int py = y1; py < y2 && count < max_n; py++) {
            for (int px = x1; px < x2 && count < max_n; px++) {
                if (in_any_mask(px, py, masks, n_masks)) continue;

                int hit_idx = -1;
                if (try_match_multi_sad(data, W, H, tpls, n_tpls,
                                        budgets_ptr, px, py, &hit_idx)) {
                    out_xy[count * 2 + 0] = px;
                    out_xy[count * 2 + 1] = py;
                    out_idxs[count] = hit_idx;
                    count++;

                    if (n_masks < MAX_MASKS) {
                        const EasyLuaTemplate *th = tpls[hit_idx];
                        masks[n_masks].l = px - (th->w - 1);
                        masks[n_masks].t = py - (th->h - 1);
                        masks[n_masks].r = px + (th->w - 1);
                        masks[n_masks].b = py + (th->h - 1);
                        n_masks++;
                    }
                }
            }
        }
        if (budgets_heap) free(budgets_heap);
    }
done_pam:
    EasyLua_FrameUnlock();
    return count;
}

/* ---------- Images_FindPicMultiDelta（色差）---------- */
int Images_FindPicMultiDelta(int x1, int y1, int x2, int y2,
                             const EasyLuaTemplate * const *tpls, int n_tpls,
                             int dr, int dg, int db,
                             float sim, int dir,
                             int *out_x, int *out_y, int *out_idx)
{
    if (out_x) *out_x = -1;
    if (out_y) *out_y = -1;
    if (out_idx) *out_idx = -1;

    if (!tpls || n_tpls <= 0) return -1;
    if (dir < 0 || dir > 3) return -1;
    if (dr < 0) dr = 0;
    if (dr > 255) dr = 255;
    if (dg < 0) dg = 0;
    if (dg > 255) dg = 255;
    if (db < 0) db = 0;
    if (db > 255) db = 255;
    if (sim < 0.0f) sim = 0.0f;
    if (sim > 1.0f) sim = 1.0f;

    int valid_tpls = 0;
    for (int i = 0; i < n_tpls; i++) if (tpls[i]) valid_tpls++;
    if (valid_tpls == 0) return -1;

    int W, H;
    const uint8_t *data = EasyLua_FrameDataLocked(&W, &H);
    int found = -1;
    if (data) {
        normalize_rect(&x1, &y1, &x2, &y2, W, H);

        int max_w, max_h;
        calc_multi_max_wh(tpls, n_tpls, &max_w, &max_h);
        if (x2 - max_w < x1 || y2 - max_h < y1) {
            EL_ERR("FindPicMultiDelta: 扫描区 (%d,%d)-(%d,%d) 装不下任何模板 (max %dx%d)",
                   x1, y1, x2, y2, max_w, max_h);
            goto done_pmd;
        }

        int min_w = 0x7FFFFFFF, min_h = 0x7FFFFFFF;
        for (int i = 0; i < n_tpls; i++) {
            if (!tpls[i]) continue;
            if (tpls[i]->w < min_w) min_w = tpls[i]->w;
            if (tpls[i]->h < min_h) min_h = tpls[i]->h;
        }
        x2 = x2 - min_w + 1;
        y2 = y2 - min_h + 1;

        DeltaBudget budgets[16];
        DeltaBudget *budgets_ptr = budgets;
        DeltaBudget *budgets_heap = NULL;
        if (n_tpls > 16) {
            budgets_heap = (DeltaBudget *)malloc(sizeof(DeltaBudget) * n_tpls);
            if (!budgets_heap) goto done_pmd;
            budgets_ptr = budgets_heap;
        }
        for (int i = 0; i < n_tpls; i++) {
            if (tpls[i]) calc_delta_budget(tpls[i], sim, &budgets_ptr[i]);
        }

        int hit_idx = -1;
        if (dir == 0) {
            for (int py = y1; py < y2; py++) {
                for (int px = x1; px < x2; px++) {
                    if (try_match_multi_delta(data, W, H, tpls, n_tpls,
                                              budgets_ptr, dr, dg, db,
                                              px, py, &hit_idx)) {
                        if (out_x) *out_x = px;
                        if (out_y) *out_y = py;
                        if (out_idx) *out_idx = hit_idx;
                        found = 0;
                        if (budgets_heap) free(budgets_heap);
                        goto done_pmd;
                    }
                }
            }
        } else {
            int sx = (dir == 1 || dir == 3) ? x2 - 1 : x1;
            int ex = (dir == 1 || dir == 3) ? x1 - 1 : x2;
            int dxs = (dir == 1 || dir == 3) ? -1 : 1;
            int sy = (dir == 2 || dir == 3) ? y2 - 1 : y1;
            int ey = (dir == 2 || dir == 3) ? y1 - 1 : y2;
            int dys = (dir == 2 || dir == 3) ? -1 : 1;
            for (int py = sy; py != ey; py += dys) {
                for (int px = sx; px != ex; px += dxs) {
                    if (try_match_multi_delta(data, W, H, tpls, n_tpls,
                                              budgets_ptr, dr, dg, db,
                                              px, py, &hit_idx)) {
                        if (out_x) *out_x = px;
                        if (out_y) *out_y = py;
                        if (out_idx) *out_idx = hit_idx;
                        found = 0;
                        if (budgets_heap) free(budgets_heap);
                        goto done_pmd;
                    }
                }
            }
        }
        if (budgets_heap) free(budgets_heap);
    }
done_pmd:
    EasyLua_FrameUnlock();
    return found;
}

/* ---------- Images_FindPicAllMultiDelta（色差）---------- */
int Images_FindPicAllMultiDelta(int x1, int y1, int x2, int y2,
                                const EasyLuaTemplate * const *tpls, int n_tpls,
                                int dr, int dg, int db,
                                float sim,
                                int *out_xy, int *out_idxs, int max_n)
{
    if (!out_xy || !out_idxs || max_n <= 0) return 0;
    if (!tpls || n_tpls <= 0) return 0;
    if (dr < 0) dr = 0;
    if (dr > 255) dr = 255;
    if (dg < 0) dg = 0;
    if (dg > 255) dg = 255;
    if (db < 0) db = 0;
    if (db > 255) db = 255;
    if (sim < 0.0f) sim = 0.0f;
    if (sim > 1.0f) sim = 1.0f;

    int valid_tpls = 0;
    for (int i = 0; i < n_tpls; i++) if (tpls[i]) valid_tpls++;
    if (valid_tpls == 0) return 0;

    int W, H;
    const uint8_t *data = EasyLua_FrameDataLocked(&W, &H);
    int count = 0;

    enum { MAX_MASKS = 256 };
    MaskRect masks[MAX_MASKS];
    int n_masks = 0;

    if (data) {
        normalize_rect(&x1, &y1, &x2, &y2, W, H);

        int max_w, max_h;
        calc_multi_max_wh(tpls, n_tpls, &max_w, &max_h);
        if (x2 - max_w < x1 || y2 - max_h < y1) {
            EL_ERR("FindPicAllMultiDelta: 扫描区 (%d,%d)-(%d,%d) 装不下任何模板 (max %dx%d)",
                   x1, y1, x2, y2, max_w, max_h);
            goto done_pamd;
        }

        int min_w = 0x7FFFFFFF, min_h = 0x7FFFFFFF;
        for (int i = 0; i < n_tpls; i++) {
            if (!tpls[i]) continue;
            if (tpls[i]->w < min_w) min_w = tpls[i]->w;
            if (tpls[i]->h < min_h) min_h = tpls[i]->h;
        }
        x2 = x2 - min_w + 1;
        y2 = y2 - min_h + 1;

        DeltaBudget budgets[16];
        DeltaBudget *budgets_ptr = budgets;
        DeltaBudget *budgets_heap = NULL;
        if (n_tpls > 16) {
            budgets_heap = (DeltaBudget *)malloc(sizeof(DeltaBudget) * n_tpls);
            if (!budgets_heap) goto done_pamd;
            budgets_ptr = budgets_heap;
        }
        for (int i = 0; i < n_tpls; i++) {
            if (tpls[i]) calc_delta_budget(tpls[i], sim, &budgets_ptr[i]);
        }

        for (int py = y1; py < y2 && count < max_n; py++) {
            for (int px = x1; px < x2 && count < max_n; px++) {
                if (in_any_mask(px, py, masks, n_masks)) continue;

                int hit_idx = -1;
                if (try_match_multi_delta(data, W, H, tpls, n_tpls,
                                          budgets_ptr, dr, dg, db,
                                          px, py, &hit_idx)) {
                    out_xy[count * 2 + 0] = px;
                    out_xy[count * 2 + 1] = py;
                    out_idxs[count] = hit_idx;
                    count++;

                    if (n_masks < MAX_MASKS) {
                        const EasyLuaTemplate *th = tpls[hit_idx];
                        masks[n_masks].l = px - (th->w - 1);
                        masks[n_masks].t = py - (th->h - 1);
                        masks[n_masks].r = px + (th->w - 1);
                        masks[n_masks].b = py + (th->h - 1);
                        n_masks++;
                    }
                }
            }
        }
        if (budgets_heap) free(budgets_heap);
    }
done_pamd:
    EasyLua_FrameUnlock();
    return count;
}

int Images_Pixel(int x, int y)
{
    int w, h;
    const uint8_t *data = EasyLua_FrameDataLocked(&w, &h);
    int rgb = 0;
    if (data && x >= 0 && y >= 0 && x < w && y < h) {
        int idx = (y * w + x) * 4;
        rgb = ((int)data[idx] << 16) | ((int)data[idx + 1] << 8) | (int)data[idx + 2];
    }
    EasyLua_FrameUnlock();
    return rgb;
}

int Images_CmpColor(int x, int y, const char *color_str, float sim)
{
    ColorTok cs[MAX_COLORS];
    int n = parse_color_list(color_str, sim, cs, MAX_COLORS);
    if (n <= 0) return -1;

    int w, h;
    const uint8_t *data = EasyLua_FrameDataLocked(&w, &h);
    int rc = -1;
    if (data && x >= 0 && y >= 0 && x < w && y < h) {
        int idx = (y * w + x) * 4;
        rc = match_any(data[idx], data[idx + 1], data[idx + 2], cs, n) ? 1 : 0;
    }
    EasyLua_FrameUnlock();
    return rc;
}

int Images_FindColor(int x1, int y1, int x2, int y2,
                     const char *color_str, float sim, int dir,
                     int *out_x, int *out_y)
{
    if (out_x) *out_x = -1;
    if (out_y) *out_y = -1;

    ColorTok cs[MAX_COLORS];
    int n = parse_color_list(color_str, sim, cs, MAX_COLORS);
    if (n <= 0) return -1;

    int w, h;
    const uint8_t *data = EasyLua_FrameDataLocked(&w, &h);
    int found = -1;
    if (data) {
        normalize_rect(&x1, &y1, &x2, &y2, w, h);

        /* 默认方向 dir==0（左→右、上→下）走最紧的递增循环，便于编译器向量化 */
        if (dir == 0 && n == 1) {
            const uint8_t r0 = cs[0].r, g0 = cs[0].g, b0 = cs[0].b;
            const uint8_t tr = cs[0].tr, tg = cs[0].tg, tb = cs[0].tb;
            for (int y = y1; y < y2; y++) {
                const uint8_t *p = data + ((size_t)y * w + x1) * 4;
                for (int x = x1; x < x2; x++, p += 4) {
                    int dr = (int)p[0] - r0; if (dr < 0) dr = -dr;
                    int dg = (int)p[1] - g0; if (dg < 0) dg = -dg;
                    int db = (int)p[2] - b0; if (db < 0) db = -db;
                    if (dr <= tr && dg <= tg && db <= tb) {
                        if (out_x) *out_x = x;
                        if (out_y) *out_y = y;
                        found = 0;
                        goto done;
                    }
                }
            }
            goto done;
        }

        int sx = (dir == 1 || dir == 3) ? x2 - 1 : x1;
        int ex = (dir == 1 || dir == 3) ? x1 - 1 : x2;
        int dx = (dir == 1 || dir == 3) ? -1 : 1;
        int sy = (dir == 2 || dir == 3) ? y2 - 1 : y1;
        int ey = (dir == 2 || dir == 3) ? y1 - 1 : y2;
        int dy = (dir == 2 || dir == 3) ? -1 : 1;

        if (n == 1) {
            const uint8_t r0 = cs[0].r, g0 = cs[0].g, b0 = cs[0].b;
            const uint8_t tr = cs[0].tr, tg = cs[0].tg, tb = cs[0].tb;
            for (int y = sy; y != ey; y += dy) {
                const uint8_t *row = data + (size_t)y * w * 4;
                for (int x = sx; x != ex; x += dx) {
                    const uint8_t *p = row + x * 4;
                    int dr = (int)p[0] - r0; if (dr < 0) dr = -dr;
                    int dg = (int)p[1] - g0; if (dg < 0) dg = -dg;
                    int db = (int)p[2] - b0; if (db < 0) db = -db;
                    if (dr <= tr && dg <= tg && db <= tb) {
                        if (out_x) *out_x = x;
                        if (out_y) *out_y = y;
                        found = 0;
                        goto done;
                    }
                }
            }
        } else {
            for (int y = sy; y != ey; y += dy) {
                const uint8_t *row = data + (size_t)y * w * 4;
                for (int x = sx; x != ex; x += dx) {
                    const uint8_t *p = row + x * 4;
                    if (match_any(p[0], p[1], p[2], cs, n)) {
                        if (out_x) *out_x = x;
                        if (out_y) *out_y = y;
                        found = 0;
                        goto done;
                    }
                }
            }
        }
    }
done:
    EasyLua_FrameUnlock();
    return found;
}

int Images_GetColorCountInRegion(int x1, int y1, int x2, int y2,
                                 const char *color_str, float sim)
{
    ColorTok cs[MAX_COLORS];
    int n = parse_color_list(color_str, sim, cs, MAX_COLORS);
    if (n <= 0) return 0;

    int w, h;
    const uint8_t *data = EasyLua_FrameDataLocked(&w, &h);
    int count = 0;
    if (data) {
        normalize_rect(&x1, &y1, &x2, &y2, w, h);
        if (n == 1) {
            const uint8_t r0 = cs[0].r, g0 = cs[0].g, b0 = cs[0].b;
            const uint8_t tr = cs[0].tr, tg = cs[0].tg, tb = cs[0].tb;
            for (int y = y1; y < y2; y++) {
                const uint8_t *p = data + ((size_t)y * w + x1) * 4;
                for (int x = x1; x < x2; x++, p += 4) {
                    int dr = (int)p[0] - r0; if (dr < 0) dr = -dr;
                    int dg = (int)p[1] - g0; if (dg < 0) dg = -dg;
                    int db = (int)p[2] - b0; if (db < 0) db = -db;
                    if (dr <= tr && dg <= tg && db <= tb) count++;
                }
            }
        } else {
            for (int y = y1; y < y2; y++) {
                const uint8_t *p = data + ((size_t)y * w + x1) * 4;
                for (int x = x1; x < x2; x++, p += 4) {
                    if (match_any(p[0], p[1], p[2], cs, n)) count++;
                }
            }
        }
    }
    EasyLua_FrameUnlock();
    return count;
}

/*
 * DetectsMultiColors：解析 "x1,y1,RRGGBB-tol,x2,y2,RRGGBB-tol,..."
 * 全部点位都命中才返回 1，否则 0；解析失败返回 -1。
 */
int Images_DetectsMultiColors(const char *colors, float sim)
{
    if (!colors || !*colors) return -1;

    int w, h;
    const uint8_t *data = EasyLua_FrameDataLocked(&w, &h);
    int rc = -1;
    if (data) {
        rc = 1;
        const char *p = colors;
        int extra_tol = 0;
        if (sim < 1.0f && sim > 0.0f) {
            extra_tol = (int)((1.0f - sim) * 255.0f);
            if (extra_tol < 0) extra_tol = 0;
            if (extra_tol > 255) extra_tol = 255;
        }
        while (*p) {
            char *end;
            int x = (int)strtol(p, &end, 10);
            if (end == p) { rc = -1; break; }
            p = end;
            if (*p == ',') p++;
            int y = (int)strtol(p, &end, 10);
            if (end == p) { rc = -1; break; }
            p = end;
            if (*p == ',') p++;

            ColorTok t = {0};
            const char *q = parse_rgb(p, &t.r, &t.g, &t.b);
            if (!q) { rc = -1; break; }
            p = q;
            if (*p == '-') {
                uint8_t tr, tg, tb;
                q = parse_rgb(p + 1, &tr, &tg, &tb);
                if (!q) { rc = -1; break; }
                t.tr = tr; t.tg = tg; t.tb = tb;
                p = q;
            }
            if (extra_tol > 0) {
                int a;
                a = (int)t.tr + extra_tol; t.tr = (uint8_t)(a > 255 ? 255 : a);
                a = (int)t.tg + extra_tol; t.tg = (uint8_t)(a > 255 ? 255 : a);
                a = (int)t.tb + extra_tol; t.tb = (uint8_t)(a > 255 ? 255 : a);
            }
            if (*p == ',') p++;

            if (x < 0 || y < 0 || x >= w || y >= h) { rc = 0; break; }
            int idx = (y * w + x) * 4;
            if (!match_any(data[idx], data[idx + 1], data[idx + 2], &t, 1)) {
                rc = 0; break;
            }
        }
    }
    EasyLua_FrameUnlock();
    return rc;
}


/* ============================================================
 * FindMultiColors：基色 + 相对偏移点序列匹配
 *
 * colors 格式：base[-tol],dx1,dy1,c1[-tol1],dx2,dy2,c2[-tol2],...
 *   - 第一段 base color：在区域内逐像素扫描的基础颜色
 *   - 后续每 3 段一组：相对 dx, dy, 颜色（基点 + 偏移在帧内必须命中此颜色）
 *
 * 实现细节：
 *   - 整段一次性解析到 RelPoint 数组，避免内层循环再做字符串处理
 *   - 解析时同时追踪偏移的 bbox（min/max dx, dy）用于 FindAll 屏蔽矩形
 *   - 内层匹配的越界点直接判为不命中（与 AutoGo 行为一致）
 * ============================================================ */

#define MAX_REL_POINTS 64

typedef struct {
    int dx, dy;
    ColorTok c;
} RelPoint;

typedef struct {
    ColorTok base;
    RelPoint rel[MAX_REL_POINTS];
    int      n_rel;
    int      min_dx, max_dx, min_dy, max_dy;  /* 相对点的 bbox */
} MultiSpec;

/* 解析多点颜色字符串。返回 0 = 成功，<0 = 失败 */
static int parse_multi_spec(const char *s, float sim, MultiSpec *out)
{
    if (!s || !*s) return -1;
    memset(out, 0, sizeof(*out));

    int extra_tol = 0;
    if (sim < 1.0f && sim > 0.0f) {
        extra_tol = (int)((1.0f - sim) * 255.0f);
        if (extra_tol < 0) extra_tol = 0;
        if (extra_tol > 255) extra_tol = 255;
    }

    /* 把容差应用到一个 ColorTok */
    #define APPLY_EXTRA_TOL(t) do {                                  \
        if (extra_tol > 0) {                                          \
            int _a;                                                    \
            _a = (int)(t).tr + extra_tol; (t).tr = (uint8_t)(_a > 255 ? 255 : _a); \
            _a = (int)(t).tg + extra_tol; (t).tg = (uint8_t)(_a > 255 ? 255 : _a); \
            _a = (int)(t).tb + extra_tol; (t).tb = (uint8_t)(_a > 255 ? 255 : _a); \
        }                                                              \
    } while (0)

    const char *p = s;

    /* 1) 解析 base color */
    {
        const char *q = parse_rgb(p, &out->base.r, &out->base.g, &out->base.b);
        if (!q) return -1;
        p = q;
        out->base.tr = out->base.tg = out->base.tb = 0;
        if (*p == '-') {
            uint8_t tr, tg, tb;
            q = parse_rgb(p + 1, &tr, &tg, &tb);
            if (!q) return -1;
            out->base.tr = tr; out->base.tg = tg; out->base.tb = tb;
            p = q;
        }
        APPLY_EXTRA_TOL(out->base);
    }

    /* 2) 后续每 3 段一组：dx, dy, color[-tol] */
    while (*p) {
        if (*p == ',') p++;
        if (!*p) break;
        if (out->n_rel >= MAX_REL_POINTS) return -1;

        char *end;
        int dx = (int)strtol(p, &end, 10);
        if (end == p) return -1;
        p = end;
        if (*p == ',') p++;
        int dy = (int)strtol(p, &end, 10);
        if (end == p) return -1;
        p = end;
        if (*p == ',') p++;

        RelPoint *rp = &out->rel[out->n_rel++];
        rp->dx = dx;
        rp->dy = dy;
        const char *q = parse_rgb(p, &rp->c.r, &rp->c.g, &rp->c.b);
        if (!q) return -1;
        p = q;
        rp->c.tr = rp->c.tg = rp->c.tb = 0;
        if (*p == '-') {
            uint8_t tr, tg, tb;
            q = parse_rgb(p + 1, &tr, &tg, &tb);
            if (!q) return -1;
            rp->c.tr = tr; rp->c.tg = tg; rp->c.tb = tb;
            p = q;
        }
        APPLY_EXTRA_TOL(rp->c);

        /* 维护 bbox（含基点 0,0） */
        if (dx < out->min_dx) out->min_dx = dx;
        if (dx > out->max_dx) out->max_dx = dx;
        if (dy < out->min_dy) out->min_dy = dy;
        if (dy > out->max_dy) out->max_dy = dy;
    }

    #undef APPLY_EXTRA_TOL

    if (out->n_rel == 0) return -1;  /* 退化为 FindColor，不走这里 */
    return 0;
}

/* 不做越界检查的快速版：仅在调用方已经保证 (px+dx, py+dy) 全部在 [0,w)x[0,h)
 * 内时使用。bbox 收紧扫描区后内层直接走这条 fast path。 */
static int match_rel_points_unsafe(const uint8_t *data, int w,
                                   int px, int py, const MultiSpec *spec)
{
    for (int i = 0; i < spec->n_rel; i++) {
        const RelPoint *rp = &spec->rel[i];
        const uint8_t *q = data + ((py + rp->dy) * w + (px + rp->dx)) * 4;
        int dr = (int)q[0] - (int)rp->c.r; if (dr < 0) dr = -dr;
        int dg = (int)q[1] - (int)rp->c.g; if (dg < 0) dg = -dg;
        int db = (int)q[2] - (int)rp->c.b; if (db < 0) db = -db;
        if (dr > rp->c.tr || dg > rp->c.tg || db > rp->c.tb) return 0;
    }
    return 1;
}

/* 把扫描矩形向内收紧，使 (x, y) ∈ [x1, x2) × [y1, y2) 时所有相对点
 * (x+dx, y+dy) 都落在帧内。等价于剪掉相对点 bbox 一定会越界的边缘像素。
 * 收紧后返回 1，若区域为空（例如用户给的扫描矩形比 bbox 还小）返回 0。 */
static int tighten_rect_for_spec(int *x1, int *y1, int *x2, int *y2,
                                 int w, int h, const MultiSpec *spec)
{
    int sxmin = -spec->min_dx;            /* 左侧需要留 -min_dx 的余量 */
    int symin = -spec->min_dy;
    int sxmax = w - spec->max_dx;         /* 右侧最远基点 + max_dx < w */
    int symax = h - spec->max_dy;
    if (*x1 < sxmin) *x1 = sxmin;
    if (*y1 < symin) *y1 = symin;
    if (*x2 > sxmax) *x2 = sxmax;
    if (*y2 > symax) *y2 = symax;
    return (*x1 < *x2 && *y1 < *y2);
}

int Images_FindMultiColors(int x1, int y1, int x2, int y2,
                           const char *colors, float sim, int dir,
                           int *out_x, int *out_y)
{
    if (out_x) *out_x = -1;
    if (out_y) *out_y = -1;

    MultiSpec spec;
    if (parse_multi_spec(colors, sim, &spec) != 0) return -1;

    int w, h;
    const uint8_t *data = EasyLua_FrameDataLocked(&w, &h);
    int found = -1;
    if (data) {
        normalize_rect(&x1, &y1, &x2, &y2, w, h);
        /* 用相对点 bbox 收紧扫描区，内层免越界检查。空区域立即退出。 */
        if (!tighten_rect_for_spec(&x1, &y1, &x2, &y2, w, h, &spec)) {
            EasyLua_FrameUnlock();
            return -1;
        }

        /* 把 base 容差 hoist 到寄存器变量，避免内层重复读结构体 */
        const uint8_t br = spec.base.r, bg = spec.base.g, bb = spec.base.b;
        const uint8_t btr = spec.base.tr, btg = spec.base.tg, btb = spec.base.tb;

        /* dir==0：递增 for，便于编译器自动向量化 */
        if (dir == 0) {
            for (int y = y1; y < y2; y++) {
                const uint8_t *p = data + ((size_t)y * w + x1) * 4;
                for (int x = x1; x < x2; x++, p += 4) {
                    int dr = (int)p[0] - br; if (dr < 0) dr = -dr;
                    int dg = (int)p[1] - bg; if (dg < 0) dg = -dg;
                    int db = (int)p[2] - bb; if (db < 0) db = -db;
                    if (dr > btr || dg > btg || db > btb) continue;
                    if (match_rel_points_unsafe(data, w, x, y, &spec)) {
                        if (out_x) *out_x = x;
                        if (out_y) *out_y = y;
                        found = 0;
                        goto done;
                    }
                }
            }
            goto done;
        }

        int sx = (dir == 1 || dir == 3) ? x2 - 1 : x1;
        int ex = (dir == 1 || dir == 3) ? x1 - 1 : x2;
        int dx = (dir == 1 || dir == 3) ? -1 : 1;
        int sy = (dir == 2 || dir == 3) ? y2 - 1 : y1;
        int ey = (dir == 2 || dir == 3) ? y1 - 1 : y2;
        int dy = (dir == 2 || dir == 3) ? -1 : 1;

        for (int y = sy; y != ey; y += dy) {
            const uint8_t *row = data + (size_t)y * w * 4;
            for (int x = sx; x != ex; x += dx) {
                const uint8_t *p = row + x * 4;
                int dr = (int)p[0] - br; if (dr < 0) dr = -dr;
                int dg = (int)p[1] - bg; if (dg < 0) dg = -dg;
                int db = (int)p[2] - bb; if (db < 0) db = -db;
                if (dr > btr || dg > btg || db > btb) continue;
                if (match_rel_points_unsafe(data, w, x, y, &spec)) {
                    if (out_x) *out_x = x;
                    if (out_y) *out_y = y;
                    found = 0;
                    goto done;
                }
            }
        }
    }
done:
    EasyLua_FrameUnlock();
    return found;
}

int Images_FindMultiColorsAll(int x1, int y1, int x2, int y2,
                              const char *colors, float sim, int dir,
                              int *out_xy, int max_n)
{
    if (max_n <= 0 || !out_xy) return 0;

    MultiSpec spec;
    if (parse_multi_spec(colors, sim, &spec) != 0) return -1;

    int w, h;
    const uint8_t *data = EasyLua_FrameDataLocked(&w, &h);
    int count = 0;

    /* 屏蔽矩形：上限 = max_n（每个匹配点贡献一个），栈上分配 */
    enum { MAX_MASKS = 256 };
    MaskRect masks[MAX_MASKS];
    int n_masks = 0;

    if (data) {
        normalize_rect(&x1, &y1, &x2, &y2, w, h);
        if (!tighten_rect_for_spec(&x1, &y1, &x2, &y2, w, h, &spec)) {
            EasyLua_FrameUnlock();
            return 0;
        }

        int sx = (dir == 1 || dir == 3) ? x2 - 1 : x1;
        int ex = (dir == 1 || dir == 3) ? x1 - 1 : x2;
        int dx = (dir == 1 || dir == 3) ? -1 : 1;
        int sy = (dir == 2 || dir == 3) ? y2 - 1 : y1;
        int ey = (dir == 2 || dir == 3) ? y1 - 1 : y2;
        int dy = (dir == 2 || dir == 3) ? -1 : 1;

        const uint8_t br = spec.base.r, bg = spec.base.g, bb = spec.base.b;
        const uint8_t btr = spec.base.tr, btg = spec.base.tg, btb = spec.base.tb;

        for (int y = sy; y != ey && count < max_n; y += dy) {
            const uint8_t *row = data + (size_t)y * w * 4;
            for (int x = sx; x != ex && count < max_n; x += dx) {
                if (in_any_mask(x, y, masks, n_masks)) continue;
                const uint8_t *p = row + x * 4;
                int dr = (int)p[0] - br; if (dr < 0) dr = -dr;
                int dg = (int)p[1] - bg; if (dg < 0) dg = -dg;
                int db = (int)p[2] - bb; if (db < 0) db = -db;
                if (dr > btr || dg > btg || db > btb) continue;
                if (match_rel_points_unsafe(data, w, x, y, &spec)) {
                    out_xy[count * 2 + 0] = x;
                    out_xy[count * 2 + 1] = y;
                    count++;
                    if (n_masks < MAX_MASKS) {
                        masks[n_masks].l = x + spec.min_dx;
                        masks[n_masks].t = y + spec.min_dy;
                        masks[n_masks].r = x + spec.max_dx;
                        masks[n_masks].b = y + spec.max_dy;
                        n_masks++;
                    }
                }
            }
        }
    }

    EasyLua_FrameUnlock();
    return count;
}


/* ============================================================
 * CaptureScreen：把当前帧的 (x1, y1)-(x2, y2) 区域复制到独立缓冲
 *
 * 与全局 g_frame_data 不同，返回的快照拥有独立内存：调用方可以慢慢用，
 * 视频流推新帧也不会覆盖。Lua 端通过 cdata + __gc 自动释放。
 *
 * 区域按 normalize_rect 规则收紧（x2/y2 == 0 → 用屏幕边界）。
 * 失败返回 NULL：区域非法、视频流首帧未到（Locked 内部超时）、内存不足。
 * ============================================================ */
EasyLuaSnapshot *Images_Capture(int x1, int y1, int x2, int y2)
{
    int w, h;
    const uint8_t *data = EasyLua_FrameDataLocked(&w, &h);
    if (!data) {
        EasyLua_FrameUnlock();
        return NULL;
    }
    normalize_rect(&x1, &y1, &x2, &y2, w, h);
    if (x1 >= x2 || y1 >= y2) {
        EasyLua_FrameUnlock();
        return NULL;
    }
    int sw = x2 - x1;
    int sh = y2 - y1;

    EasyLuaSnapshot *s = (EasyLuaSnapshot *)malloc(sizeof(EasyLuaSnapshot));
    if (!s) {
        EasyLua_FrameUnlock();
        return NULL;
    }
    s->w = sw;
    s->h = sh;
    s->pix = (uint8_t *)malloc((size_t)sw * sh * 4);
    if (!s->pix) {
        free(s);
        EasyLua_FrameUnlock();
        return NULL;
    }
    /* 行拷贝：源 stride=w*4，目标 stride=sw*4，紧凑布局 */
    for (int y = 0; y < sh; y++) {
        const uint8_t *src = data + ((size_t)(y1 + y) * w + x1) * 4;
        uint8_t       *dst = s->pix + (size_t)y * sw * 4;
        memcpy(dst, src, (size_t)sw * 4);
    }
    EasyLua_FrameUnlock();
    return s;
}

void Images_CaptureFree(EasyLuaSnapshot *s)
{
    if (!s) return;
    free(s->pix);
    free(s);
}

int Images_SnapshotPixel(const EasyLuaSnapshot *s, int x, int y)
{
    if (!s || !s->pix) return 0;
    if (x < 0 || y < 0 || x >= s->w || y >= s->h) return 0;
    const uint8_t *p = s->pix + ((size_t)y * s->w + x) * 4;
    return ((int)p[0] << 16) | ((int)p[1] << 8) | (int)p[2];
}

/* ---------- 文件保存（PNG / JPG / BMP）----------
 *
 * 思路：先用 type 显式指定格式；type 为 NULL/"" 时，从路径后缀推断。
 * 编码由 stb_image_write 完成（单线程，纯 CPU）。
 */

/* 不分配的小写比较 */
static int ieq(const char *a, const char *b)
{
    if (!a || !b) return 0;
    while (*a && *b) {
        char ca = *a; if (ca >= 'A' && ca <= 'Z') ca = (char)(ca + 32);
        char cb = *b; if (cb >= 'A' && cb <= 'Z') cb = (char)(cb + 32);
        if (ca != cb) return 0;
        a++; b++;
    }
    return *a == 0 && *b == 0;
}

static const char *guess_type_from_path(const char *path)
{
    if (!path) return NULL;
    const char *dot = strrchr(path, '.');
    if (!dot) return NULL;
    const char *ext = dot + 1;
    if (ieq(ext, "png")) return "png";
    if (ieq(ext, "jpg") || ieq(ext, "jpeg")) return "jpg";
    if (ieq(ext, "bmp")) return "bmp";
    return NULL;
}

int Images_SnapshotSave(const EasyLuaSnapshot *s, const char *path,
                        const char *type, int quality)
{
    if (!s || !s->pix || !path || !*path) return -1;

    if (!type || !*type) type = guess_type_from_path(path);
    if (!type) return -1;     /* 无法推断格式 */

    if (quality <= 0 || quality > 100) quality = 90;

    int rc = 0;
    if (ieq(type, "png")) {
        /* RGBA 4 通道；stride 紧凑 */
        rc = stbi_write_png(path, s->w, s->h, 4, s->pix, s->w * 4);
    } else if (ieq(type, "jpg") || ieq(type, "jpeg")) {
        /* JPG 不支持 alpha；stb 会忽略第 4 通道 */
        rc = stbi_write_jpg(path, s->w, s->h, 4, s->pix, quality);
    } else if (ieq(type, "bmp")) {
        rc = stbi_write_bmp(path, s->w, s->h, 4, s->pix);
    } else {
        return -1;  /* 不支持的 type */
    }

    /* stb 返回非 0 = 成功 */
    return rc ? 0 : -3;
}
