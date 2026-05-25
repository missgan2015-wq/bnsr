/*
 * test_findpic_pbt.c
 *
 * Images.FindPic 属性测试（host 端，单 TU 编译）。
 *
 * 覆盖：
 *   - Property 1（Task 3.4）：完美匹配 identity
 *       Validates: Requirements 8.1
 *   - Property 2（Task 3.5）：透明像素不参与匹配
 *       Validates: Requirements 9.1, 9.2
 *
 * 编译策略：本文件直接 `#include "../images.c"` 进入同一编译单元，从而
 *   1) 拿到 EasyLuaTemplate 的完整结构定义（valid_off / valid_dxdy /
 *      rgb / valid_n / w / h / feat_* 等内部字段），P2 必须扰动透明像素
 *      位置的 rgb 字节，无法只走公开 API；
 *   2) 复用 images.c 中 STB_IMAGE_WRITE_IMPLEMENTATION 暴露的
 *      stbi_write_png_to_mem，把合成模板编码成 PNG 字节流再喂给
 *      Images_LoadTemplate，验证完整加载/匹配链路；
 *   3) mock 实现 EasyLua_FrameDataLocked / FrameUnlock，绕过 easylua.c
 *      的 pthread / 视频流依赖。mock 必须在 #include "../images.c"
 *      之前定义且不能 static（images.h 把它们声明为 extern）。
 *
 * 运行：
 *   build_host.bat                # 编译并执行
 *   test_findpic_pbt.exe          # 默认种子 0xC0FFEE
 *   test_findpic_pbt.exe 12345    # 自定义种子
 *
 * 任一属性失败立即打印诊断并 abort；全部通过打印 "ALL PBT TESTS PASSED"
 * 后 return 0。
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <assert.h>

/* ---------- mock：EasyLua_FrameDataLocked / FrameUnlock ----------
 *
 * images.h 把这两个函数声明为 extern。测试 TU 直接给出实现，让
 * Images_FindPic / FindPicAll 走过同样的"取锁 → 读帧 → 解锁"路径。
 *
 * g_test_frame 是当前帧像素（紧凑 RGBA8888，stride = g_test_W * 4）。
 * 测试用例修改 g_test_frame / g_test_W / g_test_H 来注入帧。
 *
 * mock 不能为 static —— 否则与 images.h 中的 extern 声明类型不一致。 */
static const uint8_t *g_test_frame = NULL;
static int g_test_W = 0;
static int g_test_H = 0;

const uint8_t *EasyLua_FrameDataLocked(int *out_w, int *out_h)
{
    if (out_w) *out_w = g_test_W;
    if (out_h) *out_h = g_test_H;
    return g_test_frame;
}

void EasyLua_FrameUnlock(void)
{
    /* 测试 mock：无锁，no-op */
}

/* ---------- 把整个 images.c 拉进来 ----------
 *
 * 关键点：images.c 顶部已经 #define STB_IMAGE_IMPLEMENTATION 与
 * STB_IMAGE_WRITE_IMPLEMENTATION，所以测试 TU 不需要、也不能再次
 * 定义这两个宏（否则会重复定义）。 */
#include "../images.c"

/* stbi_write_png_to_mem 在 stb_image_write.h 的 header 段没有 forward
 * 声明，只在 implementation 段定义为 STBIWDEF（默认 extern）。这里补一
 * 行声明给后续测试代码用。 */
extern unsigned char *stbi_write_png_to_mem(const unsigned char *pixels,
                                            int stride_bytes,
                                            int x, int y, int n,
                                            int *out_len);

/* ---------- 简易 xorshift32 RNG ----------
 *
 * 自带状态以便按种子复现失败用例。不追求统计学最优分布，只要"覆盖度
 * 足够 + 可复现"即可。 */
typedef struct { uint32_t s; } rng_t;

static uint32_t rng_next(rng_t *r)
{
    uint32_t x = r->s;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    r->s = x ? x : 0xDEADBEEFu;   /* 防止退化为 0 */
    return r->s;
}

/* 闭区间 [lo, hi] 上的随机整数 */
static int rng_int(rng_t *r, int lo, int hi)
{
    uint32_t span = (uint32_t)(hi - lo + 1);
    return (int)(lo + (int)(rng_next(r) % span));
}

static uint8_t rng_byte(rng_t *r)
{
    return (uint8_t)(rng_next(r) & 0xFFu);
}

/* ---------- 通用工具：把模板 RGB 粘到底图 RGBA 的 (x0, y0) ----------
 *
 * 仅复制 valid 像素位置的 RGB（透明像素位置保持底图原值，模拟"模板把
 * 自己贴到帧上"的真实行为）。底图的 alpha 保持原值。
 *
 * 入参：
 *   tpl  : 已加载模板（提供 valid_off / valid_dxdy / w / h）
 *   frame: 底图 RGBA 缓冲，stride = W * 4
 *   W    : 底图宽度
 *   x0,y0: 粘贴左上角 */
static void paste_template_valid(const EasyLuaTemplate *tpl,
                                 uint8_t *frame, int W,
                                 int x0, int y0)
{
    for (int i = 0; i < tpl->valid_n; i++) {
        int32_t dxdy = tpl->valid_dxdy[i];
        int vx = dxdy & 0xFFFF;
        int vy = (dxdy >> 16) & 0xFFFF;
        int32_t toff = tpl->valid_off[i];        /* (vy*w + vx) * 3 */
        const uint8_t *tp = tpl->rgb + toff;
        uint8_t *fp = frame + ((y0 + vy) * W + (x0 + vx)) * 4;
        fp[0] = tp[0];
        fp[1] = tp[1];
        fp[2] = tp[2];
        /* alpha 保留底图原值（帧本来也只读 RGB） */
    }
}

/* ============================================================
 * Property 1（Task 3.4）：完美匹配 identity
 *
 * 随机生成 w×h ∈ [4, 32] 模板，alpha 全 255（视为完全不透明），编码 PNG
 * 后通过 Images_LoadTemplate 加载；随机底图 W×H ∈ [模板尺寸, 96]，把
 * 模板 RGB 粘到 (x0, y0)（含 (0,0) 与 (W-w, H-h) 边界），断言：
 *   Images_FindPic(0, 0, W, H, t, 1.0f, 0, ...) == 0
 *   (*out_x, *out_y) == (x0, y0)
 *
 * Validates: Requirements 8.1
 * ============================================================ */
static int run_property1(rng_t *rng, int iters)
{
    for (int it = 0; it < iters; it++) {
        /* 1) 随机模板尺寸 */
        int w = rng_int(rng, 4, 32);
        int h = rng_int(rng, 4, 32);

        /* 2) 合成 RGBA 模板：alpha 全 255，RGB 随机 */
        uint8_t *tpl_rgba = (uint8_t *)malloc((size_t)w * h * 4);
        if (!tpl_rgba) {
            fprintf(stderr, "[PBT P1] iter=%d malloc 模板缓冲失败\n", it);
            return 1;
        }
        for (int i = 0; i < w * h; i++) {
            tpl_rgba[i * 4 + 0] = rng_byte(rng);
            tpl_rgba[i * 4 + 1] = rng_byte(rng);
            tpl_rgba[i * 4 + 2] = rng_byte(rng);
            tpl_rgba[i * 4 + 3] = 255;            /* 全不透明 */
        }

        /* 3) 编码为 PNG 字节流 */
        int png_len = 0;
        unsigned char *png_bytes = stbi_write_png_to_mem(
            tpl_rgba, w * 4, w, h, 4, &png_len);
        if (!png_bytes || png_len <= 0) {
            fprintf(stderr,
                "[PBT P1] iter=%d seed_state=0x%08x stbi_write_png_to_mem 失败\n",
                it, rng->s);
            free(tpl_rgba);
            return 1;
        }

        /* 4) 加载模板 */
        EasyLuaTemplate *t = Images_LoadTemplate(png_bytes, png_len, "p1");
        if (!t) {
            fprintf(stderr,
                "[PBT P1] iter=%d w=%d h=%d Images_LoadTemplate 返回 NULL\n",
                it, w, h);
            STBIW_FREE(png_bytes);
            free(tpl_rgba);
            return 1;
        }

        /* 5) 随机底图尺寸 W,H ∈ [模板尺寸, 96] */
        int W = rng_int(rng, w, 96);
        int H = rng_int(rng, h, 96);

        /* 6) 选择粘贴位置 (x0, y0)：每 16 次强制覆盖一次 (0,0) 与
         *    (W-w, H-h) 边界，确保边界用例出现。 */
        int x0, y0;
        int corner = it & 15;
        if (corner == 0) {
            x0 = 0; y0 = 0;
        } else if (corner == 1) {
            x0 = W - w; y0 = H - h;
        } else if (corner == 2) {
            x0 = 0; y0 = H - h;
        } else if (corner == 3) {
            x0 = W - w; y0 = 0;
        } else {
            x0 = rng_int(rng, 0, W - w);
            y0 = rng_int(rng, 0, H - h);
        }

        /* 7) 合成底图：先填随机噪声，再把模板的 RGB 覆盖到 (x0, y0) */
        uint8_t *frame = (uint8_t *)malloc((size_t)W * H * 4);
        if (!frame) {
            fprintf(stderr, "[PBT P1] iter=%d malloc 底图失败\n", it);
            Images_FreeTemplate(t);
            STBIW_FREE(png_bytes);
            free(tpl_rgba);
            return 1;
        }
        for (int i = 0; i < W * H * 4; i++) frame[i] = rng_byte(rng);
        paste_template_valid(t, frame, W, x0, y0);

        /* 8) 注入帧并调用 Images_FindPic */
        g_test_frame = frame;
        g_test_W = W;
        g_test_H = H;

        int rx = -999, ry = -999;
        int rc = Images_FindPic(0, 0, W, H, t, 1.0f, 0, &rx, &ry);

        g_test_frame = NULL;
        g_test_W = 0;
        g_test_H = 0;

        /* 9) 断言：完美匹配必须命中且坐标精确等于 (x0, y0) */
        if (rc != 0 || rx != x0 || ry != y0) {
            fprintf(stderr,
                "[PBT P1] FAILED iter=%d seed_state=0x%08x\n"
                "  模板 w=%d h=%d, 底图 W=%d H=%d, 期望 (x0,y0)=(%d,%d)\n"
                "  实际 rc=%d (out_x,out_y)=(%d,%d)\n",
                it, rng->s, w, h, W, H, x0, y0, rc, rx, ry);
            free(frame);
            Images_FreeTemplate(t);
            STBIW_FREE(png_bytes);
            free(tpl_rgba);
            return 1;
        }

        /* 10) 资源释放 */
        free(frame);
        Images_FreeTemplate(t);
        STBIW_FREE(png_bytes);
        free(tpl_rgba);
    }
    return 0;
}

/* ============================================================
 * Property 2（Task 3.5）：透明像素扰动后匹配结果不变
 *
 * 加载含 alpha < 128 透明像素与不透明像素混合的模板 T_alpha，把所有
 * 透明像素位置（即 t->rgb 中"非 valid_off 命中"的字节）随机扰动得到
 * T_perturbed，但保持 valid_off / valid_dxdy / feat_off / feat_dxdy /
 * w / h / valid_n / feat_n 不变。同一帧、同一参数下断言：
 *   FindPic(扰动前) == FindPic(扰动后)（返回值 + (out_x, out_y)）
 *
 * Validates: Requirements 9.1, 9.2
 * ============================================================ */
static int run_property2(rng_t *rng, int iters)
{
    int it = 0;
    int regen = 0;

    while (it < iters) {
        /* 1) 随机模板尺寸 ∈ [6, 32]，让 valid_n 有较高概率 ≥ 8（触发
         *    feat_n > 0 路径）。 */
        int w = rng_int(rng, 6, 32);
        int h = rng_int(rng, 6, 32);

        /* 2) 合成 RGBA 模板：每个像素 60% 不透明（alpha=255），40% 随机
         *    透明（alpha < 128）。 */
        uint8_t *tpl_rgba = (uint8_t *)malloc((size_t)w * h * 4);
        if (!tpl_rgba) {
            fprintf(stderr, "[PBT P2] iter=%d malloc 模板缓冲失败\n", it);
            return 1;
        }
        for (int i = 0; i < w * h; i++) {
            tpl_rgba[i * 4 + 0] = rng_byte(rng);
            tpl_rgba[i * 4 + 1] = rng_byte(rng);
            tpl_rgba[i * 4 + 2] = rng_byte(rng);
            uint32_t roll = rng_next(rng) % 100;
            if (roll < 60) {
                tpl_rgba[i * 4 + 3] = 255;
            } else {
                /* alpha ∈ [0, 127]，确保 < ALPHA_CUT */
                tpl_rgba[i * 4 + 3] = (uint8_t)(rng_next(rng) % 128);
            }
        }

        /* 3) 编码 PNG */
        int png_len = 0;
        unsigned char *png_bytes = stbi_write_png_to_mem(
            tpl_rgba, w * 4, w, h, 4, &png_len);
        if (!png_bytes || png_len <= 0) {
            fprintf(stderr,
                "[PBT P2] iter=%d seed_state=0x%08x stbi_write_png_to_mem 失败\n",
                it, rng->s);
            free(tpl_rgba);
            return 1;
        }

        /* 4) 加载模板。若全透明（valid_n == 0）会返回 NULL，跳过本轮重试。 */
        EasyLuaTemplate *t = Images_LoadTemplate(png_bytes, png_len, "p2");
        if (!t) {
            STBIW_FREE(png_bytes);
            free(tpl_rgba);
            regen++;
            if (regen > iters * 4) {
                fprintf(stderr,
                    "[PBT P2] 连续重试 %d 次仍未生成有效模板，提前退出\n",
                    regen);
                return 1;
            }
            continue;
        }

        /* 5) 合成底图（W,H ∈ [模板尺寸, 96]）+ 随机粘贴位置 */
        int W = rng_int(rng, w, 96);
        int H = rng_int(rng, h, 96);
        int x0 = rng_int(rng, 0, W - w);
        int y0 = rng_int(rng, 0, H - h);

        uint8_t *frame = (uint8_t *)malloc((size_t)W * H * 4);
        if (!frame) {
            fprintf(stderr, "[PBT P2] iter=%d malloc 底图失败\n", it);
            Images_FreeTemplate(t);
            STBIW_FREE(png_bytes);
            free(tpl_rgba);
            return 1;
        }
        for (int i = 0; i < W * H * 4; i++) frame[i] = rng_byte(rng);
        paste_template_valid(t, frame, W, x0, y0);

        /* 6) 第一次匹配 */
        g_test_frame = frame;
        g_test_W = W;
        g_test_H = H;

        int x1 = -999, y1 = -999;
        int rc1 = Images_FindPic(0, 0, W, H, t, 0.95f, 0, &x1, &y1);

        /* 7) 计算"非 valid"字节集合：visited[i] = 1 表示 t->rgb[i] 属于
         *    某个 valid 像素（不能扰动）；剩余的就是透明像素的 RGB 字节。 */
        int rgb_n = w * h * 3;
        uint8_t *visited = (uint8_t *)calloc((size_t)rgb_n, 1);
        if (!visited) {
            fprintf(stderr, "[PBT P2] iter=%d calloc visited 失败\n", it);
            free(frame);
            Images_FreeTemplate(t);
            STBIW_FREE(png_bytes);
            free(tpl_rgba);
            return 1;
        }
        for (int i = 0; i < t->valid_n; i++) {
            int32_t off = t->valid_off[i];
            if (off + 2 < rgb_n) {
                visited[off + 0] = 1;
                visited[off + 1] = 1;
                visited[off + 2] = 1;
            }
        }

        /* 8) 备份原始 rgb，扰动透明像素位置 */
        uint8_t *rgb_backup = (uint8_t *)malloc((size_t)rgb_n);
        if (!rgb_backup) {
            fprintf(stderr, "[PBT P2] iter=%d malloc rgb 备份失败\n", it);
            free(visited);
            free(frame);
            Images_FreeTemplate(t);
            STBIW_FREE(png_bytes);
            free(tpl_rgba);
            return 1;
        }
        memcpy(rgb_backup, t->rgb, (size_t)rgb_n);

        int perturbed_n = 0;
        for (int i = 0; i < rgb_n; i++) {
            if (!visited[i]) {
                t->rgb[i] = rng_byte(rng);
                perturbed_n++;
            }
        }

        /* 9) 第二次匹配（同一帧、同一参数） */
        int x2 = -999, y2 = -999;
        int rc2 = Images_FindPic(0, 0, W, H, t, 0.95f, 0, &x2, &y2);

        g_test_frame = NULL;
        g_test_W = 0;
        g_test_H = 0;

        /* 10) 断言：返回值与坐标必须完全一致 */
        if (rc1 != rc2 || x1 != x2 || y1 != y2) {
            fprintf(stderr,
                "[PBT P2] FAILED iter=%d seed_state=0x%08x\n"
                "  模板 w=%d h=%d valid_n=%d feat_n=%d perturbed=%d/%d 字节\n"
                "  底图 W=%d H=%d 粘贴 (x0,y0)=(%d,%d)\n"
                "  扰动前 rc=%d (x,y)=(%d,%d)\n"
                "  扰动后 rc=%d (x,y)=(%d,%d)\n",
                it, rng->s, w, h, t->valid_n, t->feat_n,
                perturbed_n, rgb_n,
                W, H, x0, y0, rc1, x1, y1, rc2, x2, y2);
            free(rgb_backup);
            free(visited);
            free(frame);
            Images_FreeTemplate(t);
            STBIW_FREE(png_bytes);
            free(tpl_rgba);
            return 1;
        }

        /* 11) 资源释放 */
        free(rgb_backup);
        free(visited);
        free(frame);
        Images_FreeTemplate(t);
        STBIW_FREE(png_bytes);
        free(tpl_rgba);

        it++;
    }
    return 0;
}

/* ============================================================
 * Property 3（Task 3.6）：FindPicAll 屏蔽矩形不返回重叠点
 *
 * 在底图上随机粘贴模板 N 次（粘贴位置满足 bbox 互不重叠），调用
 * Images_FindPicAll(sim=1.0)，断言：
 *   1) 返回 count == N（不漏不重）
 *   2) 任意 i ≠ j，命中点 i 与 j 的 bbox 互不重叠（即 |xi-xj| >= w 或
 *      |yi-yj| >= h）—— 这才是"屏蔽矩形真的有效"的条件
 *
 * 关键不变量：屏蔽矩形必须挡住所有"bbox 重叠"的候选起点，而不仅仅是
 * "起点落在已命中 bbox 内"的候选起点。这就是修复 #屏蔽矩形向四周展开
 * 后的核心保证。
 *
 * Validates: Requirements 10.6
 * ============================================================ */
static int run_property3(rng_t *rng, int iters)
{
    int it = 0;
    int regen = 0;

    while (it < iters) {
        /* 1) 模板尺寸 ∈ [8, 24]，留足底图空间放多个互不重叠副本 */
        int w = rng_int(rng, 8, 24);
        int h = rng_int(rng, 8, 24);

        /* 2) 合成不透明 RGBA 模板，加点结构性内容（横竖两条线 + 噪声）
         *    避免纯随机像素被 SAD 早退误判为不同副本 */
        uint8_t *tpl_rgba = (uint8_t *)malloc((size_t)w * h * 4);
        if (!tpl_rgba) {
            fprintf(stderr, "[PBT P3] iter=%d malloc 模板缓冲失败\n", it);
            return 1;
        }
        for (int i = 0; i < w * h; i++) {
            tpl_rgba[i * 4 + 0] = rng_byte(rng);
            tpl_rgba[i * 4 + 1] = rng_byte(rng);
            tpl_rgba[i * 4 + 2] = rng_byte(rng);
            tpl_rgba[i * 4 + 3] = 255;
        }

        /* 3) 编码 PNG → 加载 */
        int png_len = 0;
        unsigned char *png_bytes = stbi_write_png_to_mem(
            tpl_rgba, w * 4, w, h, 4, &png_len);
        if (!png_bytes || png_len <= 0) {
            free(tpl_rgba);
            return 1;
        }
        EasyLuaTemplate *t = Images_LoadTemplate(png_bytes, png_len, "p3");
        if (!t) {
            STBIW_FREE(png_bytes);
            free(tpl_rgba);
            return 1;
        }

        /* 4) 底图：W,H 选大一点，留足非重叠区域 */
        int W = rng_int(rng, w * 6, 192);
        int H = rng_int(rng, h * 6, 192);

        /* 5) 网格化布点：把底图分成 (W/(2w)) × (H/(2h)) 个网格，每格中
         *    心放一个模板副本，保证 bbox 之间至少间隔 w/h（互不重叠）。
         *    总数 N ∈ [3, 16]，太多可能撞上 MAX_MASKS=256 的退化路径。 */
        int cols = W / (w * 2);
        int rows = H / (h * 2);
        if (cols < 2 || rows < 2) {
            /* 底图太小放不下 ≥3 个非重叠副本，跳过本轮 */
            Images_FreeTemplate(t);
            STBIW_FREE(png_bytes);
            free(tpl_rgba);
            regen++;
            if (regen > iters * 4) return 1;
            continue;
        }
        int max_n = cols * rows;
        if (max_n > 16) max_n = 16;
        int n_paste = rng_int(rng, 3, max_n);

        /* 选 n_paste 个不同的网格位置（无放回） */
        int *grid_pick = (int *)malloc((size_t)cols * rows * sizeof(int));
        if (!grid_pick) {
            Images_FreeTemplate(t);
            STBIW_FREE(png_bytes);
            free(tpl_rgba);
            return 1;
        }
        for (int i = 0; i < cols * rows; i++) grid_pick[i] = i;
        /* Fisher-Yates shuffle 前 n_paste 项 */
        for (int i = 0; i < n_paste; i++) {
            int j = i + (int)(rng_next(rng) % (uint32_t)(cols * rows - i));
            int tmp = grid_pick[i];
            grid_pick[i] = grid_pick[j];
            grid_pick[j] = tmp;
        }

        /* 6) 合成底图：随机噪声 + 把模板贴到选定的网格位置 */
        uint8_t *frame = (uint8_t *)malloc((size_t)W * H * 4);
        if (!frame) {
            free(grid_pick);
            Images_FreeTemplate(t);
            STBIW_FREE(png_bytes);
            free(tpl_rgba);
            return 1;
        }
        for (int i = 0; i < W * H * 4; i++) frame[i] = rng_byte(rng);

        /* 记录粘贴位置（按行优先排序，便于后续与 FindPicAll 输出对照） */
        int *paste_xs = (int *)malloc((size_t)n_paste * sizeof(int));
        int *paste_ys = (int *)malloc((size_t)n_paste * sizeof(int));
        if (!paste_xs || !paste_ys) {
            free(paste_xs); free(paste_ys);
            free(frame); free(grid_pick);
            Images_FreeTemplate(t);
            STBIW_FREE(png_bytes);
            free(tpl_rgba);
            return 1;
        }
        for (int i = 0; i < n_paste; i++) {
            int gi = grid_pick[i];
            int gx = gi % cols;
            int gy = gi / cols;
            /* 网格中心位置：每格大小是 (W/cols) × (H/rows) */
            int cell_w = W / cols;
            int cell_h = H / rows;
            int x0 = gx * cell_w + (cell_w - w) / 2;
            int y0 = gy * cell_h + (cell_h - h) / 2;
            if (x0 + w > W) x0 = W - w;
            if (y0 + h > H) y0 = H - h;
            if (x0 < 0) x0 = 0;
            if (y0 < 0) y0 = 0;
            paste_xs[i] = x0;
            paste_ys[i] = y0;
            paste_template_valid(t, frame, W, x0, y0);
        }

        /* 7) 调用 FindPicAll(sim=1.0) */
        g_test_frame = frame;
        g_test_W = W;
        g_test_H = H;

        int *out_xy = (int *)malloc((size_t)n_paste * 4 * sizeof(int));
        if (!out_xy) {
            free(paste_xs); free(paste_ys);
            free(frame); free(grid_pick);
            Images_FreeTemplate(t);
            STBIW_FREE(png_bytes);
            free(tpl_rgba);
            return 1;
        }
        int got = Images_FindPicAll(0, 0, W, H, t, 1.0f,
                                    out_xy, n_paste * 2);

        g_test_frame = NULL;
        g_test_W = 0;
        g_test_H = 0;

        /* 8a) 数量断言：FindPicAll 必须找到精确 n_paste 个目标 */
        if (got != n_paste) {
            fprintf(stderr,
                "[PBT P3] FAILED iter=%d seed=0x%08x\n"
                "  模板 %dx%d 底图 %dx%d 粘贴 %d 个\n"
                "  FindPicAll 返回 count=%d\n",
                it, rng->s, w, h, W, H, n_paste, got);
            free(out_xy); free(paste_xs); free(paste_ys);
            free(frame); free(grid_pick);
            Images_FreeTemplate(t);
            STBIW_FREE(png_bytes);
            free(tpl_rgba);
            return 1;
        }

        /* 8b) 互不重叠断言：任意两个命中点的 bbox 不能重叠 */
        for (int i = 0; i < got; i++) {
            int xi = out_xy[i * 2];
            int yi = out_xy[i * 2 + 1];
            for (int j = i + 1; j < got; j++) {
                int xj = out_xy[j * 2];
                int yj = out_xy[j * 2 + 1];
                int dx = xi > xj ? xi - xj : xj - xi;
                int dy = yi > yj ? yi - yj : yj - yi;
                if (dx < w && dy < h) {
                    fprintf(stderr,
                        "[PBT P3] FAILED iter=%d seed=0x%08x\n"
                        "  模板 %dx%d 底图 %dx%d 粘贴 %d 个\n"
                        "  命中 [%d]=(%d,%d) 与 [%d]=(%d,%d) bbox 重叠 (dx=%d dy=%d)\n",
                        it, rng->s, w, h, W, H, n_paste,
                        i, xi, yi, j, xj, yj, dx, dy);
                    free(out_xy); free(paste_xs); free(paste_ys);
                    free(frame); free(grid_pick);
                    Images_FreeTemplate(t);
                    STBIW_FREE(png_bytes);
                    free(tpl_rgba);
                    return 1;
                }
            }
        }

        /* 9) 释放 */
        free(out_xy); free(paste_xs); free(paste_ys);
        free(frame); free(grid_pick);
        Images_FreeTemplate(t);
        STBIW_FREE(png_bytes);
        free(tpl_rgba);

        it++;
    }
    return 0;
}

/* ============================================================
 * Property 4：色差模式完美匹配 + 等价性
 *
 * 子 (a)：delta="000000" + sim=1.0 ⇔ Images_FindPic(sim=1.0)
 *   随机模板贴底图，断言两种调用都命中精确 (x0, y0)。
 *
 * 子 (b)：色差模式对透明/非 valid 字节扰动免疫
 *   构造含 alpha < 128 的混合模板，扰动 t->rgb 中所有非 valid 字节，
 *   delta="000000" + sim=1.0 在扰动前后命中坐标必须一致。
 *
 * 子 (c)：delta 越大命中数越多（单调性）
 *   同一帧、同一 sim，delta 从 "000000" 升到 "404040"，FindPicAll 返回
 *   count 必须单调不降。
 *
 * Validates: 色差模式 API 行为 + Property 1/2 在色差模式下的对应关系
 * ============================================================ */

/* 把 6 位 hex 拆 3 个 dr/dg/db */
static void parse_hex(const char *s, int *dr, int *dg, int *db)
{
    unsigned int v;
    sscanf(s, "%x", &v);
    *dr = (int)((v >> 16) & 0xFF);
    *dg = (int)((v >>  8) & 0xFF);
    *db = (int)( v        & 0xFF);
}

static int run_property4(rng_t *rng, int iters)
{
    /* (a) 完美匹配 identity */
    for (int it = 0; it < iters; it++) {
        int w = rng_int(rng, 4, 32);
        int h = rng_int(rng, 4, 32);
        uint8_t *tpl_rgba = (uint8_t *)malloc((size_t)w * h * 4);
        if (!tpl_rgba) return 1;
        for (int i = 0; i < w * h; i++) {
            tpl_rgba[i * 4 + 0] = rng_byte(rng);
            tpl_rgba[i * 4 + 1] = rng_byte(rng);
            tpl_rgba[i * 4 + 2] = rng_byte(rng);
            tpl_rgba[i * 4 + 3] = 255;
        }
        int png_len = 0;
        unsigned char *png_bytes = stbi_write_png_to_mem(
            tpl_rgba, w * 4, w, h, 4, &png_len);
        if (!png_bytes || png_len <= 0) { free(tpl_rgba); return 1; }
        EasyLuaTemplate *t = Images_LoadTemplate(png_bytes, png_len, "p4a");
        if (!t) { STBIW_FREE(png_bytes); free(tpl_rgba); return 1; }

        int W = rng_int(rng, w, 96);
        int H = rng_int(rng, h, 96);
        int x0 = rng_int(rng, 0, W - w);
        int y0 = rng_int(rng, 0, H - h);

        uint8_t *frame = (uint8_t *)malloc((size_t)W * H * 4);
        if (!frame) {
            Images_FreeTemplate(t);
            STBIW_FREE(png_bytes);
            free(tpl_rgba);
            return 1;
        }
        for (int i = 0; i < W * H * 4; i++) frame[i] = rng_byte(rng);
        paste_template_valid(t, frame, W, x0, y0);

        g_test_frame = frame;
        g_test_W = W; g_test_H = H;

        /* delta="000000" + sim=1.0 必须命中 */
        int rx = -999, ry = -999;
        int rc = Images_FindPicDelta(0, 0, W, H, t,
                                     0, 0, 0, 1.0f, 0, &rx, &ry);

        g_test_frame = NULL; g_test_W = 0; g_test_H = 0;

        if (rc != 0 || rx != x0 || ry != y0) {
            fprintf(stderr,
                "[PBT P4a] FAILED iter=%d 期望 (%d,%d) 实际 rc=%d (%d,%d)\n",
                it, x0, y0, rc, rx, ry);
            free(frame);
            Images_FreeTemplate(t);
            STBIW_FREE(png_bytes);
            free(tpl_rgba);
            return 1;
        }

        free(frame);
        Images_FreeTemplate(t);
        STBIW_FREE(png_bytes);
        free(tpl_rgba);
    }

    /* (b) 透明/非 valid 字节扰动免疫 */
    int it = 0, regen = 0;
    while (it < iters) {
        int w = rng_int(rng, 6, 32);
        int h = rng_int(rng, 6, 32);
        uint8_t *tpl_rgba = (uint8_t *)malloc((size_t)w * h * 4);
        if (!tpl_rgba) return 1;
        for (int i = 0; i < w * h; i++) {
            tpl_rgba[i * 4 + 0] = rng_byte(rng);
            tpl_rgba[i * 4 + 1] = rng_byte(rng);
            tpl_rgba[i * 4 + 2] = rng_byte(rng);
            tpl_rgba[i * 4 + 3] =
                (rng_next(rng) % 100 < 60) ? 255
                                           : (uint8_t)(rng_next(rng) % 128);
        }
        int png_len = 0;
        unsigned char *png_bytes = stbi_write_png_to_mem(
            tpl_rgba, w * 4, w, h, 4, &png_len);
        if (!png_bytes || png_len <= 0) { free(tpl_rgba); return 1; }
        EasyLuaTemplate *t = Images_LoadTemplate(png_bytes, png_len, "p4b");
        if (!t) {
            STBIW_FREE(png_bytes);
            free(tpl_rgba);
            regen++;
            if (regen > iters * 4) return 1;
            continue;
        }

        int W = rng_int(rng, w, 96);
        int H = rng_int(rng, h, 96);
        int x0 = rng_int(rng, 0, W - w);
        int y0 = rng_int(rng, 0, H - h);

        uint8_t *frame = (uint8_t *)malloc((size_t)W * H * 4);
        if (!frame) {
            Images_FreeTemplate(t);
            STBIW_FREE(png_bytes);
            free(tpl_rgba);
            return 1;
        }
        for (int i = 0; i < W * H * 4; i++) frame[i] = rng_byte(rng);
        paste_template_valid(t, frame, W, x0, y0);

        g_test_frame = frame;
        g_test_W = W; g_test_H = H;

        int x1 = -999, y1 = -999;
        int rc1 = Images_FindPicDelta(0, 0, W, H, t,
                                      0, 0, 0, 1.0f, 0, &x1, &y1);

        /* 扰动非 valid 字节 */
        int rgb_n = w * h * 3;
        uint8_t *visited = (uint8_t *)calloc((size_t)rgb_n, 1);
        if (!visited) {
            free(frame);
            Images_FreeTemplate(t);
            STBIW_FREE(png_bytes);
            free(tpl_rgba);
            return 1;
        }
        for (int i = 0; i < t->valid_n; i++) {
            int32_t off = t->valid_off[i];
            if (off + 2 < rgb_n) {
                visited[off + 0] = 1;
                visited[off + 1] = 1;
                visited[off + 2] = 1;
            }
        }
        for (int i = 0; i < rgb_n; i++) {
            if (!visited[i]) t->rgb[i] = rng_byte(rng);
        }

        int x2 = -999, y2 = -999;
        int rc2 = Images_FindPicDelta(0, 0, W, H, t,
                                      0, 0, 0, 1.0f, 0, &x2, &y2);

        g_test_frame = NULL; g_test_W = 0; g_test_H = 0;

        if (rc1 != rc2 || x1 != x2 || y1 != y2) {
            fprintf(stderr,
                "[PBT P4b] FAILED iter=%d 扰动前 rc=%d (%d,%d)  扰动后 rc=%d (%d,%d)\n",
                it, rc1, x1, y1, rc2, x2, y2);
            free(visited); free(frame);
            Images_FreeTemplate(t);
            STBIW_FREE(png_bytes);
            free(tpl_rgba);
            return 1;
        }

        free(visited); free(frame);
        Images_FreeTemplate(t);
        STBIW_FREE(png_bytes);
        free(tpl_rgba);
        it++;
    }

    /* (c) delta 单调性：delta 越大，FindPicAll count 越大或相等 */
    int mono_iters = iters / 4;   /* 这部分较慢，迭代少一点 */
    for (int it2 = 0; it2 < mono_iters; it2++) {
        int w = rng_int(rng, 6, 16);
        int h = rng_int(rng, 6, 16);
        uint8_t *tpl_rgba = (uint8_t *)malloc((size_t)w * h * 4);
        if (!tpl_rgba) return 1;
        for (int i = 0; i < w * h; i++) {
            tpl_rgba[i * 4 + 0] = rng_byte(rng);
            tpl_rgba[i * 4 + 1] = rng_byte(rng);
            tpl_rgba[i * 4 + 2] = rng_byte(rng);
            tpl_rgba[i * 4 + 3] = 255;
        }
        int png_len = 0;
        unsigned char *png_bytes = stbi_write_png_to_mem(
            tpl_rgba, w * 4, w, h, 4, &png_len);
        if (!png_bytes || png_len <= 0) { free(tpl_rgba); return 1; }
        EasyLuaTemplate *t = Images_LoadTemplate(png_bytes, png_len, "p4c");
        if (!t) { STBIW_FREE(png_bytes); free(tpl_rgba); return 1; }

        int W = rng_int(rng, w * 3, 64);
        int H = rng_int(rng, h * 3, 64);
        uint8_t *frame = (uint8_t *)malloc((size_t)W * H * 4);
        if (!frame) {
            Images_FreeTemplate(t);
            STBIW_FREE(png_bytes);
            free(tpl_rgba);
            return 1;
        }
        for (int i = 0; i < W * H * 4; i++) frame[i] = rng_byte(rng);
        /* 贴一份精确副本，再让随机噪声成为"接近但不完全像"的候选 */
        paste_template_valid(t, frame, W, 0, 0);

        g_test_frame = frame;
        g_test_W = W; g_test_H = H;

        int out_buf[256 * 2];
        const char *deltas[] = { "000000", "081008", "101010",
                                 "202020", "404040" };
        int last_count = 0;
        for (int k = 0; k < 5; k++) {
            int dr_, dg_, db_;
            parse_hex(deltas[k], &dr_, &dg_, &db_);
            int got = Images_FindPicAllDelta(0, 0, W, H, t,
                                             dr_, dg_, db_, 0.95f,
                                             out_buf, 256);
            if (got < last_count) {
                fprintf(stderr,
                    "[PBT P4c] FAILED iter=%d delta=%s count=%d 比上一档 %d 少\n",
                    it2, deltas[k], got, last_count);
                g_test_frame = NULL; g_test_W = 0; g_test_H = 0;
                free(frame);
                Images_FreeTemplate(t);
                STBIW_FREE(png_bytes);
                free(tpl_rgba);
                return 1;
            }
            last_count = got;
        }

        g_test_frame = NULL; g_test_W = 0; g_test_H = 0;
        free(frame);
        Images_FreeTemplate(t);
        STBIW_FREE(png_bytes);
        free(tpl_rgba);
    }

    return 0;
}

/* ---------- main ----------
 *
 * 默认种子 0xC0FFEE，命令行第一个参数可覆盖（十进制或 0x 前缀十六进制）。
 * 任一属性失败立即 return 1，全部通过 return 0。 */
int main(int argc, char **argv)
{
    uint32_t seed = 0xC0FFEEu;
    if (argc >= 2) {
        char *end = NULL;
        unsigned long v = strtoul(argv[1], &end, 0);
        if (end && *end == '\0') seed = (uint32_t)v;
    }
    if (seed == 0) seed = 0xC0FFEEu;

    printf("[PBT] seed = 0x%08x\n", seed);

    /* 给三个属性各自独立的 RNG 状态，避免互相影响可复现性 */
    rng_t rng1 = { seed };
    rng_t rng2 = { seed ^ 0x9E3779B9u };
    rng_t rng3 = { seed ^ 0x6A09E667u };

    const int iters = 1000;

    if (run_property1(&rng1, iters) != 0) {
        fprintf(stderr, "[PBT P1] FAILED\n");
        return 1;
    }
    printf("[PBT P1] %d/%d passed (完美匹配 identity, Validates Req 8.1)\n",
           iters, iters);

    if (run_property2(&rng2, iters) != 0) {
        fprintf(stderr, "[PBT P2] FAILED\n");
        return 1;
    }
    printf("[PBT P2] %d/%d passed "
           "(透明像素扰动不影响匹配, Validates Req 9.1, 9.2)\n",
           iters, iters);

    /* P3 用 200 次（每次合成多副本底图开销较大） */
    if (run_property3(&rng3, 200) != 0) {
        fprintf(stderr, "[PBT P3] FAILED\n");
        return 1;
    }
    printf("[PBT P3] 200/200 passed "
           "(FindPicAll bbox 互不重叠, Validates Req 10.6)\n");

    /* P4 色差模式 */
    rng_t rng4 = { seed ^ 0xBB67AE85u };
    if (run_property4(&rng4, 500) != 0) {
        fprintf(stderr, "[PBT P4] FAILED\n");
        return 1;
    }
    printf("[PBT P4] 500a + 500b + 125c passed "
           "(色差模式 identity / 透明扰动免疫 / delta 单调)\n");

    printf("ALL PBT TESTS PASSED\n");
    return 0;
}
