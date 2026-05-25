# L2.5 LuaJIT bytecode 魔改 (Stage 5b)

> 把 `.luac` 落盘字节流彻底改造，让 `luajit-decompiler-v2` / 上游 `luajit` 在
> **第一个字节** 就拒绝识别。所有改动集中在 `Backport_Project/src/`，VSIX 端
> 不接触任何敏感算法（密钥、shuffle 表只存在于 PC 端 native exe 与 Android
> 端 `libeasylua.so` 内）。

## 安全模型

```
┌────────────────────────────────────┐
│ VSIX (TypeScript, 易反编译)        │  仅触发打包，不接触加密/混淆细节
├────────────────────────────────────┤
│ apkbuild.exe (native CLI, Stage 4) │  把 luajit-obfus 调用 + gradle/签名串起来
├────────────────────────────────────┤
│ luajit-obfus.exe                   │  魔改 LuaJIT 宿主版，写 .luac 时混淆
│ libluajit.a (含 obfus reader)      │  链接到 libeasylua.so，运行时反向还原
└────────────────────────────────────┘
   ↑ 攻击者拿到 VSIX 或 apkbuild.exe 都看不到 shuffle 表内容（在 .data
     段、需要先做 native reverse），也拿不到加密密钥（密钥派生在 native
     编译期，`lj_obfus_table.h` 永远只在编译机生成、立即吃进 .o 后就没用了）。
```

## 改造点

| 文件 | 用途 | 改动行数 |
| --- | --- | --- |
| `Backport_Project/src/lj_obfus_table.h` | 自动生成：opcode shuffle 表 + 16 字节 KCONST_KEY + 自定义 magic | 全文件 |
| `Backport_Project/src/lj_obfus.h` | 公共 API + LJ_HASOBFUS 开关 | 全文件 |
| `Backport_Project/src/lj_obfus.c` | pack/unpack（含 pc-xor 操作数 + opcode remap） + xor32 | 全文件 |
| `Backport_Project/src/lj_bcdump.h` | 魔数/版本号有条件改写 | +12 |
| `Backport_Project/src/lj_bcwrite.c` | `bcwrite_bytecode` 末尾调 `lj_obfus_pack_bc` | +6 |
| `Backport_Project/src/lj_bcread.c` | `bcread_bytecode` 调 `lj_obfus_unpack_bc`，跳过 lib path | +9 |
| `Backport_Project/src/lj_lex.c` | `lj_lex_setup` 同时识别 OBFUS_HEAD1 与上游 0x1B | +4 |
| `Backport_Project/src/Makefile` | LJCORE_O 添加 `lj_obfus.o` | +1 |
| `Backport_Project/src/Makefile.dep` | 加 `lj_bcread.o` / `lj_bcwrite.o` / `lj_obfus.o` 的 obfus 依赖 | +5 |
| `Backport_Project/src/ljamalg.c` | amalg 模式下 include `lj_obfus.c` | +1 |
| `Backport_Project/scripts/build-android.sh` | 加 `--obfus on/off` + `--skip-verify` 开关 | +30 |
| `easylua/scripts/gen-obfus-table.py` | seed → `lj_obfus_table.h` 表生成器 | 全文件 |
| `easylua/scripts/build-luajit-obfus.ps1` | 一站式 PowerShell 构建（gen-table → WSL build × ABI → 归集） | 全文件 |
| `easylua/scripts/test_obfus_roundtrip.sh` | 6 项 round-trip 自验证 | 全文件 |

## 三层混淆

1. **自定义 magic**：`OBFUS_HEAD1..3 + OBFUS_VERSION` 由 seed 派生（默认 `0xFB 0xE3 0xEF 0x90`）。`luajit-decompiler-v2` 第一行 assertion 检查 `0x1B 'L' 'J'`，直接 panic
2. **opcode shuffle**：96 个 enum 值映射到 0..255 的 96 个不同字节，剩余 160 字节是陷阱（reader 收到立即 LJ_ERR_BCBAD）。同一 Lua 源码 shuffled 后字节流与上游完全不同
3. **pc-xor 操作数**：BCIns 4 个字节里除 opcode 槽外的 3 个 byte（A/B/C/D 的位置）按 `(pc * 31 + field_idx) & 15` 索引取 16 字节 KCONST_KEY 异或。即使攻击者 dump 出 OPCODE_OUT/IN 表，BCIns 的 A/B/C/D 仍按 pc 异或，重复字节模式为 0（由 `test_obfus_roundtrip.sh` 步骤 6 自动验证）

> 关键：**enum 数值不动**。`vm_*.dasc` 的 dispatch 跳表、`BC_FORL+1==BC_IFORL` 这种 LJ_STATIC_ASSERT、JIT recorder 都依赖原始 enum 数值。我们只在 reader/writer 边界做 byte-level remap，运行时透明，性能 0 损失。

## 编译启用

```sh
# Linux/WSL —— 直接 make
cd Backport_Project/src
make clean
make XCFLAGS=-DLUAJIT_ENABLE_OBFUS

# Android NDK 跨编 —— 用 build-android.sh 加 --obfus on
./scripts/build-android.sh --abi arm64-v8a --api 29 --ndk ~/android-ndk-r27c \
    --mode release --obfus on
```

## 一站式构建（推荐）

`easylua/scripts/build-luajit-obfus.ps1` 把 gen-obfus-table → WSL build-android.sh × ABI → host 编译 → 产物归集 串成一条命令：

```powershell
# 默认：复用现有 lj_obfus_table.h，编 arm64-v8a + x86_64 + Linux host，
# 归集到 easylua/out_obfus/
pwsh easylua/scripts/build-luajit-obfus.ps1

# 重新生成 seed（让所有旧 .luac 立即失效）
pwsh easylua/scripts/build-luajit-obfus.ps1 -Seed "release-2026-Q3-private"

# 仅 host 端（开发期跑 round-trip 测试用）
pwsh easylua/scripts/build-luajit-obfus.ps1 -SkipAndroid
```

输出结构：

```
easylua/out_obfus/
├── arm64-v8a/{libluajit.so, libluajit.a, luajit}      Android arm64 (供 libeasylua.so 链接)
├── x86_64/{libluajit.so, libluajit.a, luajit}         Android x86_64
├── host-linux-x86_64/{luajit, libluajit.*, jit/}      Linux 宿主端（供 .luac 编译用，Stage 4 apkbuild.exe 调）
└── build-info.json                                    seed / sha8 / abis / timestamp
```

## 使用流程

```sh
# 1. 生成混淆表（仅在打包机上跑，正式发版前换 seed）
python easylua/scripts/gen-obfus-table.py --seed "<your-private-seed>"
# -> 写 Backport_Project/src/lj_obfus_table.h

# 2. 编 PC 宿主端 luajit-obfus
cd Backport_Project/src
make clean && make XCFLAGS=-DLUAJIT_ENABLE_OBFUS

# 3. 用它编 .luac
./luajit -b -s main.lua main.luac
# main.luac 头部 = OBFUS_HEAD1..3 + OBFUS_VERSION，例如 fb e3 ef 90

# 4. 编 Android target 端 libeasylua.so，链接同源 libluajit.a
#    打到 APK 里释放到 /data/local/tmp 后，runtime 端 lj_bcread 会用
#    同样 lj_obfus_table.h 还原 main.luac
```

## 攻击成本变化

| 攻击者技能 | 上游 LuaJIT | L1 (raw bytecode) | L2.5 (本方案) |
| --- | --- | --- | --- |
| 普通脚本娃 | unzip + cat 看不到代码（有 BCDUMP magic） | luajit-decompiler-v2 一键反编 | luajit-decompiler-v2 入口 assertion 直接 panic |
| 会用 IDA 的 | 同上 | 同上 | 必须先 reverse `libeasylua.so` 找 OPCODE_IN 表（在 .data 段，160 字节陷阱掩护下不易扫到） |
| 工业逆向工程师 | 同上 | 同上 | reverse 出 IN 表后还得改 ljd2 源码加上自定义魔数支持 + opcode unshuffle，再编 → 总成本约 3-5 天，远高于 L1 的"半小时" |

## Round-trip 验证

`easylua/scripts/test_obfus_roundtrip.sh` 一键跑 6 项检查：

1. obfus luajit 编译 `.lua → .luac` 成功
2. 落盘 magic 与 `lj_obfus_table.h::OBFUS_HEAD*` 一致
3. obfus luajit 反向加载 `.luac` 业务逻辑正确
4. 上游 luajit 拒绝加载（错误信息体现"被当成源码 parse 失败"）
5. luajit-decompiler-v2 头部 assertion 不通过（`0x1B 'L' 'J'` 检查失败）
6. **pc-xor 操作数生效**：构造 13 条相同 KSHORT，落盘后高频 4-byte 重复块占比 0/35

WSL Linux 验证日志（最新一次）：

```
[test] 1) obfus 编译 -> /tmp/obfus_test_xxx.luac
[test] 2) magic 头校验   ✓ fbe3ef90
[test] 3) obfus 自反加载 ✓ fact(5)=120
[test] 4) 上游 luajit 拒绝加载 ✓ rc=1
[test] 5) ljd2 头部断言失败    ✓ 第一字节非 0x1B
[test] 6) pc-xor 操作数        ✓ 重复 4-byte 块 0/35
```

## 工程坑（已踩 + 已解）

1. **lib_read_lfunc 路径冲突**：LuaJIT 内置库（如 `string.len`、`table.foreachi`）通过 `host/genlibbc.lua` + `buildvm_lib.c` 走特殊路径嵌进 `libbc_code[]`，运行时 `lj_lib_register → lib_read_lfunc → lj_bcread_proto → bcread_bytecode`，但喂的是 **raw enum 字节**（host 端用 `BC_xxx` 名字直接拼，从未经过 `lj_bcwrite`）。
   - **解**：用 `ls->c >= 0` 区分两条路径。`lj_bcread` 主入口必有 `ls->c == BCDUMP_HEAD1 >= 0`；`lib_read_lfunc` 设 `ls->c = -1`。只在前者路径调 `lj_obfus_unpack_bc`。
2. **`lj_obfus.c` 头文件 include 顺序**：早期版本把 `#include "lj_obfus.h"` 放在 `#if LJ_HASOBFUS` 内部，导致宏未定义时整个 .c 被预处理跳过 → 链接缺符号。改成"先 include，再 `#if`"。
3. **lex_loadbc 入口识别**：上游 LuaJIT 用 `LUA_SIGNATURE[0] == 0x1B` 识别 bytecode dump。改了 magic 后必须给 `lj_lex_setup` 加一条 `|| ls->c == LJ_OBFUS_HEAD1` 分支，否则魔改 .luac 会被当成 Lua 源码送去 lj_parse → 立即报语法错。

## 后续迭代

- [ ] 把 `lj_obfus_xor32 / unxor32` 真正连接到 KNUM/KSHORT/KCDATA 的 ULEB128 编码层（hook 已就位且 baseline 编译通过；当前留为 dead code，warnings: unused function 已知）
- [ ] 把 build-info.json 接入 Stage 4 `apkbuild.exe`：检查打包时的 seed sha8 与 jniLibs 中 .so 的 seed sha8 一致，防止"用旧 luajit 编 .luac，新 .so 跑"导致的运行时 LJ_ERR_BCBAD
- [ ] 跨发版 seed 轮换流程：每次发布前换 seed → 旧版 .luac 在新 runtime 拒载，等于强制升级
