package main

import (
	"os"
	"os/exec"
	"path/filepath"
	"runtime"
	"sort"
	"strings"
)

// AutoDetectEnv 在 apkbuild 启动时自动探测常见的工具链路径，
// 把找到的写到 os.Environ()，让后续 gradle / apksigner / keytool 等子进程能用到。
//
// 行为：
//   - JAVA_HOME 缺失 → 自动探测一个合适版本（偏好 17）
//   - JAVA_HOME 已设但版本不在 17~21 范围 → 自动找一个兼容版本覆盖（只对 apkbuild 子进程生效，
//     不动用户系统）。这一步对常见场景必要：用户装了 JDK 24 后系统设了 JAVA_HOME=D:\jdk-24，
//     但 Gradle 8.5 + AGP 7.4.2 不兼容 22+，必须切回 17/21
//   - ANDROID_HOME / ANDROID_SDK_ROOT 缺失 → 自动探测
//
// 这样在 VS Code 这种"启动时不读 shell rc"的场景下，用户即使没在系统级
// 配环境变量，也能开箱即用——前提是工具装在常见路径。
func AutoDetectEnv() {
	currentJava := os.Getenv("JAVA_HOME")
	if currentJava == "" || !isCompatibleJdk(currentJava) {
		if path := autoDetectJavaHome(); path != "" {
			_ = os.Setenv("JAVA_HOME", path)
			prependPath(filepath.Join(path, "bin"))
		}
	}
	if os.Getenv("ANDROID_HOME") == "" && os.Getenv("ANDROID_SDK_ROOT") == "" {
		if path := autoDetectAndroidSdk(); path != "" {
			_ = os.Setenv("ANDROID_HOME", path)
			_ = os.Setenv("ANDROID_SDK_ROOT", path)
		}
	}
}

// isCompatibleJdk 判断目录是不是 JDK 17~21 范围（apkbuild 流水线兼容范围）。
func isCompatibleJdk(javaHome string) bool {
	if !isValidJavaHome(javaHome) {
		return false
	}
	major := readJdkMajor(javaHome)
	return major >= 17 && major <= 21
}

// readJdkMajor 调用 java -version，从输出里解出主版本号。失败返回 0。
func readJdkMajor(javaHome string) int {
	bin := "java"
	if runtime.GOOS == "windows" {
		bin = "java.exe"
	}
	javaBin := filepath.Join(javaHome, "bin", bin)
	if !exists(javaBin) {
		return 0
	}
	out, err := exec.Command(javaBin, "-version").CombinedOutput()
	if err != nil {
		return 0
	}
	first := firstLine(string(out))
	return parseJavaMajor(first)
}

// autoDetectJavaHome 找一个 17~21 范围的 JDK 安装目录。
// 用 java -version 实际验证版本,目录名匹配只是排序提示。
func autoDetectJavaHome() string {
	candidates := commonJavaCandidates()
	// 偏好 17,目录名命中 17 的排前面
	sort.SliceStable(candidates, func(i, j int) bool {
		return jdkScore(candidates[i]) > jdkScore(candidates[j])
	})

	// 第一遍：找 17~21 严格匹配
	for _, c := range candidates {
		if !isValidJavaHome(c) {
			continue
		}
		major := readJdkMajor(c)
		if major >= 17 && major <= 21 {
			return c
		}
	}
	// 第二遍 fallback：找任意 17+ 的（gradle 报错时让用户看到具体版本号）
	for _, c := range candidates {
		if !isValidJavaHome(c) {
			continue
		}
		if readJdkMajor(c) >= 17 {
			return c
		}
	}
	// 最后兜底：任何带 javac 的
	for _, c := range candidates {
		if isValidJavaHome(c) {
			return c
		}
	}
	return ""
}

// jdkScore 给候选目录打分，越高越优先。
//   - 命中 jdk-17* / openjdk-17 → 100
//   - 命中 jdk-21 → 80
//   - 命中 jdk-11 → 60（向下兼容老项目）
//   - 命中其他 → 50
func jdkScore(path string) int {
	low := strings.ToLower(path)
	switch {
	case strings.Contains(low, "jdk-17") || strings.Contains(low, "openjdk-17") || strings.Contains(low, "jdk17"):
		return 100
	case strings.Contains(low, "jdk-21") || strings.Contains(low, "openjdk-21") || strings.Contains(low, "jdk21"):
		return 80
	case strings.Contains(low, "jdk-11") || strings.Contains(low, "openjdk-11") || strings.Contains(low, "jdk11"):
		return 60
	default:
		return 50
	}
}

func isValidJavaHome(dir string) bool {
	bin := "javac"
	if runtime.GOOS == "windows" {
		bin = "javac.exe"
	}
	return exists(filepath.Join(dir, "bin", bin))
}

// commonJavaCandidates 列出常见的 JDK 安装位置（Windows 优先；其他平台简单覆盖）。
func commonJavaCandidates() []string {
	out := []string{}
	if runtime.GOOS == "windows" {
		drives := []string{"C:\\", "D:\\", "E:\\", "F:\\"}
		// 通用 OpenJDK 路径
		patterns := []string{
			"Program Files\\Java",
			"Program Files\\Eclipse Adoptium",
			"Program Files\\Microsoft\\jdk-*",
			"Program Files\\Zulu",
			"Java",            // D:\Java\jdk-17
			"jdk-*",           // D:\jdk-17.0.x
			"openjdk\\jdk-*",  // E:\Android\openjdk\jdk-17.0.12
			"Android\\openjdk\\jdk-*",
		}
		for _, d := range drives {
			for _, p := range patterns {
				out = append(out, expandGlob(filepath.Join(d, p))...)
			}
		}
	} else {
		// /usr/lib/jvm/<distro>
		out = append(out, expandGlob("/usr/lib/jvm/*")...)
		out = append(out, expandGlob("/Library/Java/JavaVirtualMachines/*/Contents/Home")...)
	}
	return out
}

// autoDetectAndroidSdk 找 Android SDK 安装目录。
//
// 标识：含 build-tools/ 与 platform-tools/ 两个子目录。
func autoDetectAndroidSdk() string {
	for _, c := range commonAndroidSdkCandidates() {
		if isValidAndroidSdk(c) {
			return c
		}
	}
	return ""
}

func isValidAndroidSdk(dir string) bool {
	return exists(filepath.Join(dir, "build-tools")) &&
		exists(filepath.Join(dir, "platform-tools"))
}

func commonAndroidSdkCandidates() []string {
	out := []string{}
	if runtime.GOOS == "windows" {
		// Android Studio 默认安装路径
		if local := os.Getenv("LOCALAPPDATA"); local != "" {
			out = append(out, filepath.Join(local, "Android", "Sdk"))
			out = append(out, filepath.Join(local, "Android", "sdk"))
		}
		if home, err := os.UserHomeDir(); err == nil {
			out = append(out, filepath.Join(home, "AppData", "Local", "Android", "Sdk"))
			out = append(out, filepath.Join(home, "AppData", "Local", "Android", "sdk"))
		}
		// 常见自定义路径（多数开发者放这）
		for _, drive := range []string{"C:\\", "D:\\", "E:\\", "F:\\"} {
			out = append(out,
				filepath.Join(drive, "android"),
				filepath.Join(drive, "Android"),
				filepath.Join(drive, "AndroidSDK"),
				filepath.Join(drive, "android-sdk"),
				filepath.Join(drive, "Android", "android-sdk"),
				filepath.Join(drive, "Android", "Sdk"),
			)
		}
	} else if runtime.GOOS == "darwin" {
		if home, err := os.UserHomeDir(); err == nil {
			out = append(out,
				filepath.Join(home, "Library", "Android", "sdk"),
				filepath.Join(home, "Android", "Sdk"),
			)
		}
	} else {
		if home, err := os.UserHomeDir(); err == nil {
			out = append(out,
				filepath.Join(home, "Android", "Sdk"),
				filepath.Join(home, "android-sdk"),
				"/opt/android-sdk",
			)
		}
	}
	return out
}

// expandGlob 是简化版的 filepath.Glob：支持后缀含 * 的目录列举。
// 这里只展开一层 *，足够覆盖 jdk-17.0.x 这类版本目录。
func expandGlob(pattern string) []string {
	if !strings.Contains(pattern, "*") {
		// 普通路径直接返回（即使不存在也让 caller 自己判断）
		return []string{pattern}
	}
	matches, err := filepath.Glob(pattern)
	if err != nil {
		return nil
	}
	return matches
}

// prependPath 把 dir 加到 PATH 最前面。
// 如果 dir 已经在 PATH 里则不动。
func prependPath(dir string) {
	if dir == "" {
		return
	}
	current := os.Getenv("PATH")
	sep := string(os.PathListSeparator)
	for _, p := range strings.Split(current, sep) {
		if filepath.Clean(p) == filepath.Clean(dir) {
			return
		}
	}
	if current == "" {
		_ = os.Setenv("PATH", dir)
	} else {
		_ = os.Setenv("PATH", dir+sep+current)
	}
}
