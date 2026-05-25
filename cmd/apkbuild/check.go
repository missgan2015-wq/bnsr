package main

import (
	"bytes"
	"fmt"
	"os"
	"os/exec"
	"path/filepath"
	"runtime"
	"strings"
)

// CheckResult 是单项检查的结果。VSIX 端会渲染成"环境检查报告"。
type CheckResult struct {
	Name   string `json:"name"`
	OK     bool   `json:"ok"`
	Detail string `json:"detail"`
	Hint   string `json:"hint,omitempty"`
}

// RunCheck 跑全部环境检查项。返回值里 ok=false 表示有任何一项失败。
//
// embed 化后只检查"用户机器上必须存在"的工具，apkbuild 内嵌资源不再单独列项。
func RunCheck(p *Paths, cfg *BuildConfig) (results []CheckResult, ok bool) {
	ok = true
	add := func(r CheckResult) {
		results = append(results, r)
		if !r.OK {
			ok = false
		}
	}

	add(checkJDK())
	add(checkAndroidSDK())
	add(checkApkSigner())
	add(checkRuntime(p))

	if cfg != nil && cfg.workspaceRoot != "" {
		add(checkWorkspace(cfg))
	}
	return
}

func checkJDK() CheckResult {
	javaHome := os.Getenv("JAVA_HOME")
	if javaHome == "" {
		return CheckResult{
			Name:   "JDK 17+",
			OK:     false,
			Detail: "JAVA_HOME 未设置",
			Hint:   "建议安装 Microsoft Build of OpenJDK 17 或 Eclipse Temurin 17，并设置 JAVA_HOME",
		}
	}
	javaBin := filepath.Join(javaHome, "bin", javaExeName())
	if !exists(javaBin) {
		return CheckResult{
			Name:   "JDK 17+",
			OK:     false,
			Detail: fmt.Sprintf("JAVA_HOME=%s 但 %s 不存在", javaHome, javaBin),
		}
	}
	out, err := exec.Command(javaBin, "-version").CombinedOutput()
	if err != nil {
		return CheckResult{
			Name:   "JDK 17+",
			OK:     false,
			Detail: fmt.Sprintf("%s -version 失败: %v", javaBin, err),
		}
	}
	versionLine := firstLine(string(out))
	major := parseJavaMajor(versionLine)
	if major < 17 {
		return CheckResult{
			Name:   "JDK 17+",
			OK:     false,
			Detail: fmt.Sprintf("JDK 版本过低: %s", versionLine),
			Hint:   "Gradle 8.5 + AGP 7.4.2 推荐 JDK 17~21",
		}
	}
	if major > 21 {
		// JDK 22+ 与 Gradle 8.5 不兼容：Gradle 解析新 class 文件版本时会抛
		// "Unsupported class file major version 67" 之类。让用户感知到风险。
		return CheckResult{
			Name:   "JDK",
			OK:     false,
			Detail: fmt.Sprintf("JDK 版本过新: %s", versionLine),
			Hint:   "Gradle 8.5 + AGP 7.4.2 仅支持 JDK 17~21；建议安装 OpenJDK 17 并设 JAVA_HOME",
		}
	}
	return CheckResult{
		Name:   "JDK 17~21",
		OK:     true,
		Detail: fmt.Sprintf("%s | %s", javaHome, versionLine),
	}
}

func checkAndroidSDK() CheckResult {
	sdk := androidSdkRoot()
	if sdk == "" {
		return CheckResult{
			Name:   "Android SDK",
			OK:     false,
			Detail: "ANDROID_HOME / ANDROID_SDK_ROOT 均未设置",
			Hint:   "安装 Android Studio 或 commandlinetools，并设置 ANDROID_HOME",
		}
	}
	bt := filepath.Join(sdk, "build-tools")
	if !exists(bt) {
		return CheckResult{
			Name:   "Android SDK",
			OK:     false,
			Detail: fmt.Sprintf("%s 缺 build-tools 子目录", sdk),
			Hint:   "用 sdkmanager 安装 build-tools;34.0.0+ 与 platforms;android-34",
		}
	}
	return CheckResult{
		Name:   "Android SDK",
		OK:     true,
		Detail: sdk,
	}
}

func checkApkSigner() CheckResult {
	path, err := findApkSigner()
	if err != nil {
		return CheckResult{
			Name:   "apksigner",
			OK:     false,
			Detail: err.Error(),
			Hint:   "在 Android SDK build-tools 安装 34.0.0+",
		}
	}
	return CheckResult{
		Name:   "apksigner",
		OK:     true,
		Detail: path,
	}
}

// checkRuntime 检查 embed 解出来的 runtime 是否完整。
func checkRuntime(p *Paths) CheckResult {
	if p == nil || p.Runtime == nil {
		return CheckResult{
			Name:   "内嵌资源",
			OK:     false,
			Detail: "runtime 未初始化",
		}
	}
	missing := []string{}
	if !exists(p.TemplateRoot) {
		missing = append(missing, "template_apk")
	}
	if !exists(p.DexFile) {
		missing = append(missing, "dex/easylua.dex")
	}
	if !exists(filepath.Join(p.NativeObfusRoot, "arm64-v8a", "libeasylua.so")) {
		missing = append(missing, "native/arm64-v8a/libeasylua.so")
	}
	if !exists(filepath.Join(p.NativeObfusRoot, "x86_64", "libeasylua.so")) {
		missing = append(missing, "native/x86_64/libeasylua.so")
	}
	if !exists(p.HostLuajit) {
		missing = append(missing, "host/luajit")
	}
	if len(missing) > 0 {
		return CheckResult{
			Name:   "内嵌资源",
			OK:     false,
			Detail: "缺少 " + strings.Join(missing, ", ") + "（apkbuild.exe 可能被损坏）",
			Hint:   "重新下载 apkbuild.exe / 重装 VSIX",
		}
	}
	return CheckResult{
		Name:   "内嵌资源",
		OK:     true,
		Detail: fmt.Sprintf("%s (version=%s)", p.Runtime.Root, p.Runtime.Version),
	}
}

func checkWorkspace(cfg *BuildConfig) CheckResult {
	main := filepath.Join(cfg.workspaceRoot, "main.lua")
	if !exists(main) {
		return CheckResult{
			Name:   "工作区入口",
			OK:     false,
			Detail: "工作区根缺 main.lua: " + main,
			Hint:   "用日志栏的『初始』按钮创建标准布局",
		}
	}
	return CheckResult{
		Name:   "工作区入口",
		OK:     true,
		Detail: main,
	}
}

// ---- helpers --------------------------------------------------------------

func javaExeName() string {
	if runtime.GOOS == "windows" {
		return "java.exe"
	}
	return "java"
}

func androidSdkRoot() string {
	for _, key := range []string{"ANDROID_HOME", "ANDROID_SDK_ROOT"} {
		if v := os.Getenv(key); v != "" {
			return v
		}
	}
	return ""
}

// findApkSigner 在 Android SDK build-tools 下挑最新版的 apksigner。
func findApkSigner() (string, error) {
	sdk := androidSdkRoot()
	if sdk == "" {
		return "", fmt.Errorf("ANDROID_HOME 未设置")
	}
	btRoot := filepath.Join(sdk, "build-tools")
	entries, err := os.ReadDir(btRoot)
	if err != nil {
		return "", fmt.Errorf("无法读 %s: %w", btRoot, err)
	}
	bin := "apksigner.bat"
	if runtime.GOOS != "windows" {
		bin = "apksigner"
	}
	candidates := []string{}
	for _, e := range entries {
		if e.IsDir() {
			candidates = append(candidates, e.Name())
		}
	}
	if len(candidates) == 0 {
		return "", fmt.Errorf("%s 下没有任何 build-tools 版本", btRoot)
	}
	for i := len(candidates) - 1; i >= 0; i-- {
		path := filepath.Join(btRoot, candidates[i], bin)
		if exists(path) {
			return path, nil
		}
	}
	return "", fmt.Errorf("build-tools 都不带 apksigner")
}

func parseJavaMajor(versionLine string) int {
	idx := strings.Index(versionLine, "\"")
	if idx < 0 {
		return 0
	}
	rest := versionLine[idx+1:]
	endIdx := strings.Index(rest, "\"")
	if endIdx < 0 {
		return 0
	}
	ver := rest[:endIdx]
	parts := strings.SplitN(ver, ".", 3)
	major := 0
	for _, ch := range parts[0] {
		if ch < '0' || ch > '9' {
			break
		}
		major = major*10 + int(ch-'0')
	}
	if major == 1 && len(parts) > 1 {
		minor := 0
		for _, ch := range parts[1] {
			if ch < '0' || ch > '9' {
				break
			}
			minor = minor*10 + int(ch-'0')
		}
		return minor
	}
	return major
}

func firstLine(s string) string {
	if i := bytes.IndexByte([]byte(s), '\n'); i >= 0 {
		return strings.TrimSpace(s[:i])
	}
	return strings.TrimSpace(s)
}