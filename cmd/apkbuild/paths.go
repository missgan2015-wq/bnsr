package main

import (
	"fmt"
	"os"
)

// exists 检查文件或目录是否存在。
func exists(path string) bool {
	_, err := os.Stat(path)
	return err == nil
}

// Paths 把 apkbuild 涉及的所有路径集中起来，方便子命令引用。
//
// 设计原则（embed 化后）：
//   - 所有"模板/产物"全部 embed 进二进制（template + native .so + dex + host luajit）
//   - 启动时 EnsureEmbeddedRuntime() 把 embed 解到 %LOCALAPPDATA%\easyLua\runtime\<version>\
//   - apkbuild.exe 拷到任何机器都自包含运行，不再依赖外部 easylua/ 目录或 WSL
type Paths struct {
	// 运行时根目录（解压自 embed）
	Runtime *EmbeddedRuntime

	// 模板与产物（都从 Runtime 派生）
	TemplateRoot    string // <runtime>/template
	NativeObfusRoot string // <runtime>/native（含 arm64-v8a/ + x86_64/）
	DexFile         string // <runtime>/dex/easylua.dex
	HostLuajit      string // <runtime>/host/luajit.exe（Windows）/ luajit（其他平台）
	HostJitDir      string // <runtime>/host/jit

	// 工作区相关（仅在 build/check 时填）
	WorkspaceRoot string // 用户工作区根（含 main.lua）
	WorkDir       string // 临时构建目录（apkbuild 内部使用）
}

// LocatePaths 解 embed → 派生 Paths。
func LocatePaths() (*Paths, error) {
	rt, err := EnsureEmbeddedRuntime()
	if err != nil {
		return nil, fmt.Errorf("初始化嵌入资源失败: %w", err)
	}
	return &Paths{
		Runtime:         rt,
		TemplateRoot:    rt.TemplateDir,
		NativeObfusRoot: rt.NativeDir,
		DexFile:         rt.DexFile,
		HostLuajit:      rt.HostLuajit,
		HostJitDir:      rt.HostJitDir,
	}, nil
}
