package main

import (
	"crypto/sha256"
	"embed"
	"encoding/hex"
	"fmt"
	"io"
	"io/fs"
	"os"
	"path/filepath"
	"runtime"
	"strings"
)

// 全部打包必需资源用 go:embed 烧进二进制。
//
// embed 树（由 prepare-embedded.ps1 在 build 之前 mirror 生成）：
//
//   embedded/
//   ├── runtime-version.txt          ← seedSha8 + 时间戳，作为运行时解压缓存键
//   ├── template/...                 ← SampleUI 载体模板（含 *.tmpl）
//   ├── dex/easylua.dex              ← Java 桥
//   ├── native/<abi>/libeasylua.so   ← obfus 版 .so（多 ABI）
//   ├── host/luajit                  ← Linux ELF 编译器（编 .luac 用，需 WSL 跑）
//   ├── host/jit/{bcsave,bc,vmdef}.lua
//   └── scripts/_compile_one.sh
//
// 运行时第一次跑会把整个 embed 树解到 %LOCALAPPDATA%\easyLua\runtime\<version>\，
// 后续运行直接复用，避免每次解压。version 命中时直接跳过，0 IO。
//
// 注：必须用 all: 前缀，否则 _compile_one.sh / .gitkeep 等以 _ 或 . 开头的文件
// 会被 go embed 默认忽略。
//
//go:embed all:embedded
var embeddedFS embed.FS

const embedPrefix = "embedded"

// EmbeddedRuntime 解出来后的运行时根目录信息。Paths 通过它派生所有路径。
type EmbeddedRuntime struct {
	Root        string // %LOCALAPPDATA%\easyLua\runtime\<version>\
	Version     string // runtime-version.txt 内容
	TemplateDir string // <Root>/template
	DexFile     string // <Root>/dex/easylua.dex
	NativeDir   string // <Root>/native
	HostDir     string // <Root>/host
	HostLuajit  string // <Root>/host/luajit.exe（Windows）/ luajit（其他平台）
	HostJitDir  string // <Root>/host/jit
}

// EnsureEmbeddedRuntime 把 embed 资源解压到本地缓存目录。
//
// 实现策略：
//   - 缓存键 = embedded/runtime-version.txt 的内容（含 seedSha8 + 编译时间戳）
//   - 缓存目录已存在且 .ok 文件标记完整 → 直接复用，不解压
//   - 不存在或不完整 → 全量重写
//
// 这样 apkbuild 第一次跑会有 ~2 MB 的解压开销，后续运行 0 IO。
// 升级 apkbuild.exe 后 runtime-version 变了，会自动重新解到新目录，旧版本在用户机器上保留作回滚用。
func EnsureEmbeddedRuntime() (*EmbeddedRuntime, error) {
	verBytes, err := embeddedFS.ReadFile(embedPrefix + "/runtime-version.txt")
	if err != nil {
		return nil, fmt.Errorf("embed 损坏: 读 runtime-version.txt 失败: %w", err)
	}
	version := strings.TrimSpace(string(verBytes))
	if version == "" {
		// fallback：用 binary 自身 sha 做版本（仅理论分支）
		version = sumBinarySha8()
	}

	root, err := defaultRuntimeRoot(version)
	if err != nil {
		return nil, err
	}
	rt := &EmbeddedRuntime{
		Root:        root,
		Version:     version,
		TemplateDir: filepath.Join(root, "template"),
		DexFile:     filepath.Join(root, "dex", "easylua.dex"),
		NativeDir:   filepath.Join(root, "native"),
		HostDir:     filepath.Join(root, "host"),
		HostLuajit:  filepath.Join(root, "host", hostLuajitName()),
		HostJitDir:  filepath.Join(root, "host", "jit"),
	}

	okFile := filepath.Join(root, ".ok")
	if exists(okFile) && exists(rt.TemplateDir) && exists(rt.DexFile) && exists(rt.HostLuajit) {
		return rt, nil
	}

	// 全量重写
	if err := os.RemoveAll(root); err != nil {
		return nil, fmt.Errorf("清理旧 runtime 失败: %w", err)
	}
	if err := os.MkdirAll(root, 0o755); err != nil {
		return nil, fmt.Errorf("创建 runtime 目录失败: %w", err)
	}
	if err := extractEmbedTo(root); err != nil {
		_ = os.RemoveAll(root)
		return nil, fmt.Errorf("解压 embed 失败: %w", err)
	}
	if err := os.WriteFile(okFile, []byte(version), 0o644); err != nil {
		return nil, fmt.Errorf("写 .ok 文件失败: %w", err)
	}
	return rt, nil
}

// 把 embedded/ 下所有文件复制到 dst 根（去掉 embedPrefix）。
func extractEmbedTo(dst string) error {
	return fs.WalkDir(embeddedFS, embedPrefix, func(p string, d fs.DirEntry, err error) error {
		if err != nil {
			return err
		}
		rel := strings.TrimPrefix(p, embedPrefix)
		rel = strings.TrimPrefix(rel, "/")
		if rel == "" {
			return nil
		}
		target := filepath.Join(dst, filepath.FromSlash(rel))
		if d.IsDir() {
			return os.MkdirAll(target, 0o755)
		}
		if err := os.MkdirAll(filepath.Dir(target), 0o755); err != nil {
			return err
		}
		// 写文件 + 给可执行权限（非 Windows 上 luajit / .sh / .so 需要 +x）
		mode := os.FileMode(0o644)
		base := filepath.Base(rel)
		if base == hostLuajitName() ||
			strings.HasSuffix(base, ".so") || strings.HasSuffix(base, ".sh") ||
			strings.HasSuffix(base, ".exe") {
			mode = 0o755
		}
		in, err := embeddedFS.Open(p)
		if err != nil {
			return err
		}
		defer in.Close()
		out, err := os.OpenFile(target, os.O_CREATE|os.O_WRONLY|os.O_TRUNC, mode)
		if err != nil {
			return err
		}
		_, copyErr := io.Copy(out, in)
		closeErr := out.Close()
		if copyErr != nil {
			return copyErr
		}
		return closeErr
	})
}

// defaultRuntimeRoot 返回 runtime 解压根目录。
//
//   Windows: %LOCALAPPDATA%\easyLua\runtime\<version>
//   其他:    ~/.cache/easyLua/runtime/<version>
//
// 不放工作区里：apkbuild 想在任意工作区都用同一份缓存（节省解压时间）。
// 不放系统全局：不需要管理员权限。
func defaultRuntimeRoot(version string) (string, error) {
	var base string
	if runtime.GOOS == "windows" {
		base = os.Getenv("LOCALAPPDATA")
		if base == "" {
			home, err := os.UserHomeDir()
			if err != nil {
				return "", err
			}
			base = filepath.Join(home, "AppData", "Local")
		}
		base = filepath.Join(base, "easyLua", "runtime")
	} else {
		home, err := os.UserHomeDir()
		if err != nil {
			return "", err
		}
		base = filepath.Join(home, ".cache", "easyLua", "runtime")
	}
	if version == "" {
		version = "default"
	}
	return filepath.Join(base, version), nil
}

// hostLuajitName 根据平台返回 host luajit 可执行文件名。
// Windows = luajit.exe，其他平台默认 luajit。
func hostLuajitName() string {
	if runtime.GOOS == "windows" {
		return "luajit.exe"
	}
	return "luajit"
}

func sumBinarySha8() string {
	exe, err := os.Executable()
	if err != nil {
		return "default"
	}
	f, err := os.Open(exe)
	if err != nil {
		return "default"
	}
	defer f.Close()
	h := sha256.New()
	if _, err := io.Copy(h, f); err != nil {
		return "default"
	}
	return hex.EncodeToString(h.Sum(nil))[:16]
}
