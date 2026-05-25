# jni/tests —— Images.FindPic 属性测试

本目录存放 host 端（Windows / mingw64）的属性测试，用来在不依赖 NDK 与
真实视频流的前提下，对 `images.c` 里的 `Images_LoadTemplate` /
`Images_FindPic` 等核心实现做随机化验证。

## 文件

| 文件 | 用途 |
| --- | --- |
| `test_findpic_pbt.c` | Property 1（完美匹配）+ Property 2（透明像素扰动）合计 2000 次随机用例 |
| `build_host.bat` | mingw64 gcc 一键编译 + 运行脚本 |

## 编译策略

`test_findpic_pbt.c` 直接 `#include "../images.c"` 进入同一编译单元，原因：

1. `EasyLuaTemplate` 的字段（`rgb / valid_off / valid_dxdy / feat_*`）
   对外不透明，Property 2 必须直接访问这些字段才能扰动透明像素位置；
2. `images.c` 已 `#define STB_IMAGE_IMPLEMENTATION` 与
   `STB_IMAGE_WRITE_IMPLEMENTATION`，单 TU 让测试可以复用
   `stbi_write_png_to_mem` 把合成模板编码为 PNG 字节再喂给
   `Images_LoadTemplate`，从而验证完整加载/匹配链路；
3. `EasyLua_FrameDataLocked` / `EasyLua_FrameUnlock` 在 `images.h` 里
   声明为 `extern`，本测试 TU 直接给出 mock 实现（写在 `#include
   "../images.c"` 之前），把全局帧切换成测试可控的 `g_test_frame`。

## 跑测试

```
cd jni\tests
build_host.bat                # 默认种子 0xC0FFEE
build_host.bat 12345          # 自定义种子（十进制或 0x 前缀十六进制）
```

预期输出：

```
[PBT] seed = 0x00c0ffee
[PBT P1] 1000/1000 passed (完美匹配 identity, Validates Req 8.1)
[PBT P2] 1000/1000 passed (透明像素扰动不影响匹配, Validates Req 9.1, 9.2)
ALL PBT TESTS PASSED
[build_host] 全部通过
```

任一属性失败时会打印诊断（种子状态、迭代号、模板尺寸、底图尺寸、
期望/实际坐标、扰动字节数）便于定位。

## 属性映射

- **Property 1（Task 3.4，Validates: Requirements 8.1）**
  随机模板贴到随机底图随机位置（含 `(0, 0)` / `(W-w, H-h)` 边界），
  断言 `FindPic(... sim=1.0, dir=0)` 返回 0 且坐标精确等于粘贴位置。
- **Property 2（Task 3.5，Validates: Requirements 9.1, 9.2）**
  含透明像素的模板，把 `t->rgb` 中所有"非 valid"字节随机扰动，断言
  扰动前后 `FindPic` 返回值与 `(out_x, out_y)` 完全一致。
