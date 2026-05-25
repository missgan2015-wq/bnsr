package main

import (
	"fmt"
	"os"
	"path/filepath"
	"runtime"
	"strings"
)

// RunGradleAssemble 在 workDir 跑 ./gradlew assembleRelease。
//
// 返回的 apkPath 是构建出的未签名 APK 绝对路径。Gradle 跑完后我们用 apksigner 单独签名。
func RunGradleAssemble(workDir string, prog *Progress) (string, error) {
	wrapper := "gradlew.bat"
	if runtime.GOOS != "windows" {
		wrapper = "./gradlew"
	}
	wrapperAbs := filepath.Join(workDir, wrapper)
	if !exists(wrapperAbs) {
		// 退路：用宿主 gradle 直接跑
		gradle, err := findGradleExe()
		if err != nil {
			return "", fmt.Errorf("workDir 缺 %s 且系统 gradle 也找不到: %w", wrapper, err)
		}
		wrapperAbs = gradle
	} else if runtime.GOOS != "windows" {
		// 给 gradlew 可执行权限
		_ = os.Chmod(wrapperAbs, 0o755)
	}

	args := []string{"assembleRelease", "--no-daemon", "--console=plain", "-Pandroid.useAndroidX=true"}
	prog.Logf("info", "运行 gradle: %s %s（在 %s）", filepath.Base(wrapperAbs), strings.Join(args, " "), workDir)

	err := runStreamLines(wrapperAbs, args, workDir, nil, func(line string, isErr bool) {
		ls := strings.TrimRight(line, " \r\n")
		if ls == "" {
			return
		}
		// 简单分级：错误 / 警告 / 一般
		lower := strings.ToLower(ls)
		switch {
		case strings.HasPrefix(ls, "FAILURE:") || strings.HasPrefix(ls, "BUILD FAILED"):
			prog.Log("error", ls)
		case strings.Contains(lower, "error:") && !strings.Contains(lower, "no errors"):
			prog.Log("error", ls)
		case strings.Contains(lower, "warning") || strings.Contains(lower, "deprecated"):
			prog.Log("warn", ls)
		default:
			if isErr {
				prog.Log("warn", ls)
			} else {
				prog.Log("info", ls)
			}
		}
	})
	if err != nil {
		return "", fmt.Errorf("gradle assembleRelease 失败: %w", err)
	}

	// 找产物：app/build/outputs/apk/release/*.apk
	apkDir := filepath.Join(workDir, "app", "build", "outputs", "apk", "release")
	entries, err := os.ReadDir(apkDir)
	if err != nil {
		return "", fmt.Errorf("找不到 release 输出目录 %s: %w", apkDir, err)
	}
	for _, e := range entries {
		if e.IsDir() {
			continue
		}
		if strings.HasSuffix(e.Name(), ".apk") {
			return filepath.Join(apkDir, e.Name()), nil
		}
	}
	return "", fmt.Errorf("%s 下找不到 .apk 文件", apkDir)
}
