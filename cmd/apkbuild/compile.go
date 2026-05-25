package main

import (
	"fmt"
	"os"
	"path/filepath"
	"strings"
	"time"
)

// CompileLua 把工作区里所有 .lua 用 host luajit 编成 .luac，写到
// workDir 下的 app/src/main/assets/scripts/。
//
// 实现：
//   - host luajit 来自 embed，已解到 <runtime>/host/luajit
//   - 直接 spawn `wsl bash <runtime>/scripts/_compile_one.sh ...`（Windows）
//     或 spawn `bash _compile_one.sh ...`（Linux/Mac）
//   - 编译入口仍是 main.lua，递归扫所有 .lua（排除 .git/node_modules 等）
//   - **每次全量编译**：放弃增量缓存，因为缓存键容易因 obfus seed / luajit 版本
//     等隐式输入失效，全量更可预测；单 .luac 编译约 50 ms，几十文件可接受
func CompileLua(p *Paths, cfg *BuildConfig, workDir string, prog *Progress) error {
	if cfg == nil {
		return fmt.Errorf("配置为空，无法编译脚本")
	}
	if cfg.workspaceRoot == "" {
		return fmt.Errorf("工作区根为空，无法编译脚本")
	}
	if !exists(filepath.Join(cfg.workspaceRoot, "main.lua")) {
		return fmt.Errorf("工作区缺入口 main.lua: %s", cfg.workspaceRoot)
	}

	scriptsOut := AssetsScriptsDir(workDir)
	if err := os.MkdirAll(scriptsOut, 0o755); err != nil {
		return err
	}
	prog.Logf("info", "编译 .lua → .luac：%s → %s", cfg.workspaceRoot, scriptsOut)

	// 加载资源加密 key（方案 B1 + C 共用）。失败直接返回错误：避免无 key 静默落明文。
	if err := LoadResKeyFromRuntime(p); err != nil {
		return fmt.Errorf("加载 RES_KEY 失败: %w", err)
	}

	// 收集 .lua 文件
	files, err := collectLuaFiles(cfg.workspaceRoot)
	if err != nil {
		return err
	}
	if len(files) == 0 {
		return fmt.Errorf("工作区下没找到任何 .lua 文件: %s", cfg.workspaceRoot)
	}
	prog.Logf("info", "  扫描到 %d 个 .lua 文件", len(files))

	debugFlag := "-s"
	if cfg.DebugObfus {
		debugFlag = "-g"
	}

	compiled, fail, copied := 0, 0, 0
	for _, lf := range files {
		// SampleUI 的 LuaUIParser 是文本 DSL 解析器，必须读 ui.lua 源码（不能 luac）。
		// 方案 B1：把 ui.lua 走 ResXor 加密成 ui.lua.enc，与方案 C 同 RES_KEY；
		// Kotlin 端 DialogActivity 通过 EasyLuaSecret.decryptUi 反向解密读取。
		// 落盘文件名追加 .enc，部署到设备后路径形如 .../ui.lua.enc。
		if isDslScript(lf.rel) {
			dst := filepath.Join(scriptsOut, lf.rel+".enc")
			if err := os.MkdirAll(filepath.Dir(dst), 0o755); err != nil {
				return err
			}
			// salt 固定 0：与 native 端 lib_io.c::io_open 的 .enc fallback 解密一致；
			// Kotlin 端 EasyLuaSecret 解密也用 salt=0。三端同步。
			if err := EncryptFile(lf.full, dst, 0); err != nil {
				prog.Logf("error", "  [FAIL encrypt] %s -> %v", lf.rel, err)
				fail++
				continue
			}
			copied++
			prog.File("encrypted", lf.rel+".enc", fileSize(dst), nil)
			continue
		}

		relLuac := strings.TrimSuffix(lf.rel, ".lua") + ".luac"
		dst := filepath.Join(scriptsOut, relLuac)
		if err := os.MkdirAll(filepath.Dir(dst), 0o755); err != nil {
			return err
		}
		if err := compileOne(p, lf.full, dst, debugFlag, prog); err != nil {
			prog.Logf("error", "  [FAIL] %s -> %v", lf.rel, err)
			fail++
			continue
		}
		compiled++
		prog.File("compiled", lf.rel, fileSize(dst), nil)
	}

	prog.Logf("info", "  汇总：编译 %d，原样拷贝 %d，失败 %d", compiled, copied, fail)
	if fail > 0 {
		return fmt.Errorf("有 %d 个脚本编译失败", fail)
	}

	// 把工作区根目录下的非 .lua 资源（图片/json/二进制等）按原样拷贝进 assets/scripts/，
	// 这样设备上 scripts/<rel> 与工作区根的 <rel> 完全一一对应，
	// io.open("assets/foo.png") 这种调用在 PC 测试和真机部署里行为一致。
	if err := copyExtraResources(cfg.workspaceRoot, scriptsOut, prog); err != nil {
		return fmt.Errorf("拷贝资源失败: %w", err)
	}
	return nil
}

// copyExtraResources 把工作区下的"非 .lua 文件"加密拷贝到 scriptsOut/。
//
// 命中策略:
//   - 只看 collectLuaFiles 一样的排除规则(.git / node_modules / out / ...)
//   - 跳过 *.lua(它们已经被 luac 编译过)
//   - 其它资源 → ResXor 加密 → 写到 dst/<rel>.enc
//
// 加密一致性：salt 固定 0，与 native 端 lib_io.c::io_open 的 .enc fallback 解密对齐。
// 设备上 io.open("assets/foo.png") 找不到时透明走 .enc fallback；脚本端零改动。
func copyExtraResources(root, dst string, prog *Progress) error {
	excludes := map[string]bool{
		".git": true, ".svn": true, ".hg": true,
		"node_modules": true, ".vscode": true, "out": true,
		"build": true, "dist": true, "__pycache__": true,
		".cache": true, ".gradle": true, ".easylua": true,
	}
	count := 0
	err := filepath.Walk(root, func(p string, info os.FileInfo, err error) error {
		if err != nil {
			return err
		}
		if info.IsDir() {
			if excludes[info.Name()] {
				return filepath.SkipDir
			}
			return nil
		}
		// .lua 已经被 CompileLua 处理过(.luac + ui.lua.enc)
		if strings.HasSuffix(strings.ToLower(p), ".lua") {
			return nil
		}
		// easylua.json 是元数据,不进 APK
		rel, err := filepath.Rel(root, p)
		if err != nil {
			return err
		}
		if rel == "easylua.json" {
			return nil
		}
		// 加密：原文件 path/to/foo.png → APK assets/scripts/path/to/foo.png.enc
		// 设备上 io.open("path/to/foo.png") 通过 lib_io.c .enc fallback 透明解密。
		target := filepath.Join(dst, rel+".enc")
		if err := os.MkdirAll(filepath.Dir(target), 0o755); err != nil {
			return err
		}
		if err := EncryptFile(p, target, 0); err != nil {
			return fmt.Errorf("加密资源 %s 失败: %w", rel, err)
		}
		count++
		prog.File("encrypted", filepath.ToSlash(rel)+".enc", info.Size(), nil)
		return nil
	})
	if err != nil {
		return err
	}
	if count > 0 {
		prog.Logf("info", "  额外资源：%d 个文件加密拷贝（.enc 后缀，运行时透明解密）", count)
	}
	return nil
}

// copyFileAB 已废弃：方案 C 引入资源加密后所有非 .lua 资源都走 EncryptFile，
// 不再需要"原样拷贝"路径。保留若需 hot reload 等场景再解封。
//
// (函数体已删，以避免未使用警告)

// isDslScript 判断脚本是否是 SampleUI 私有 DSL，不能走 luac 编译。
//
// 当前规则：路径 basename 等于 ui.lua（无论在工作区根、ui/、scripts/ 哪一级）。
// SampleUI 的 LuaUIParser 是文本正则解析器（读 dialog/checkbox/edit 等 DSL 块），
// 编成 .luac 后 readText() 读到二进制 → 解析失败 → dialog 不显示。
func isDslScript(rel string) bool {
	rel = filepath.ToSlash(rel)
	base := filepath.Base(rel)
	return base == "ui.lua"
}
type luaFileEntry struct {
	full  string
	rel   string
	size  int64
	mtime time.Time
}

// collectLuaFiles 递归收集 .lua 文件，跳过常见非源码目录。
func collectLuaFiles(root string) ([]luaFileEntry, error) {
	excludes := map[string]bool{
		".git": true, ".svn": true, ".hg": true,
		"node_modules": true, ".vscode": true, "out": true,
		"build": true, "dist": true, "__pycache__": true,
		".cache": true, ".gradle": true, ".easylua": true,
	}
	out := []luaFileEntry{}
	err := filepath.Walk(root, func(p string, info os.FileInfo, err error) error {
		if err != nil {
			return err
		}
		if info.IsDir() {
			if excludes[info.Name()] {
				return filepath.SkipDir
			}
			return nil
		}
		if !strings.HasSuffix(strings.ToLower(p), ".lua") {
			return nil
		}
		rel, err := filepath.Rel(root, p)
		if err != nil {
			return err
		}
		// 把 Windows \ 转成 /，与缓存键一致
		rel = filepath.ToSlash(rel)
		out = append(out, luaFileEntry{
			full:  p,
			rel:   rel,
			size:  info.Size(),
			mtime: info.ModTime(),
		})
		return nil
	})
	return out, err
}

// compileOne 调 host luajit 把 src.lua 编成 dst.luac。
//
// embed 出的是 Windows 原生 luajit.exe（mingw 静态编），直接 spawn 即可，不需要 WSL。
// 通过 LUA_PATH 让 luajit 找到 jit/bcsave 模块（它是 luajit -b 必需的依赖）。
//
// 注意：require("jit.bcsave") 会把模块名的 "." 转成 "/" 后用 LUA_PATH 匹配，
// 所以 LUA_PATH 必须指向 jit/ 的父目录（host/），让 ?.lua → jit/bcsave.lua 命中。
func compileOne(p *Paths, srcLua, dstLuac, debugFlag string, prog *Progress) error {
	hostDir := filepath.Dir(p.HostJitDir) // = <runtime>/host
	luaPath := filepath.Join(hostDir, "?.lua") + ";" +
		filepath.Join(hostDir, "?", "init.lua")

	env := append(os.Environ(),
		"LUA_PATH="+luaPath,
		// 防止 luajit 加载用户 LUA_CPATH 里的 .so（确定性环境）
		"LUA_CPATH=",
	)

	args := []string{"-b", debugFlag, "-t", "raw", srcLua, dstLuac}
	out, err := runWithOutput(p.HostLuajit, args, "", env)
	if err != nil {
		return fmt.Errorf("%v\n%s", err, strings.TrimSpace(out))
	}
	if !exists(dstLuac) {
		return fmt.Errorf("编译完成但 luac 不存在: %s\n%s", dstLuac, out)
	}
	return nil
}

func fileSize(path string) int64 {
	info, err := os.Stat(path)
	if err != nil {
		return 0
	}
	return info.Size()
}
