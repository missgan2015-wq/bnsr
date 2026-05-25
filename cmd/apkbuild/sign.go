package main

import (
	"fmt"
	"os"
	osexec "os/exec"
	"path/filepath"
	"strings"
)

// SignAPK 调 apksigner 给 unsigned APK 签名 + zipalign（apksigner 8 已自带 alignment）。
//
// keystorePath 必填（绝对或相对工作区根）；password 通过 stdin 传入，不在命令行裸露
// （apksigner 支持 --ks-pass stdin 但需要分别 stdin 输入 ks-pass 和 key-pass，
// 我们简化处理：只支持单密码，key-pass 与 ks-pass 一致；apksigner 也支持 pass:env: 协议
// 但需要先写到环境变量，复杂度类似）。
func SignAPK(unsignedAPK, keystorePath, alias, password string, prog *Progress) (string, error) {
	if !exists(unsignedAPK) {
		return "", fmt.Errorf("待签名 APK 不存在: %s", unsignedAPK)
	}
	if !exists(keystorePath) {
		return "", fmt.Errorf("keystore 不存在: %s", keystorePath)
	}
	signer, err := findApkSigner()
	if err != nil {
		return "", fmt.Errorf("找不到 apksigner: %w", err)
	}

	dir := filepath.Dir(unsignedAPK)
	base := filepath.Base(unsignedAPK)
	signed := filepath.Join(dir, strings.TrimSuffix(base, "-unsigned.apk")+".apk")
	if signed == unsignedAPK {
		// fallback：原文件名没带 -unsigned 后缀
		signed = filepath.Join(dir, "signed-"+base)
	}

	args := []string{
		"sign",
		"--ks", keystorePath,
		"--ks-key-alias", alias,
		"--ks-pass", "stdin",
		"--key-pass", "stdin",
		"--out", signed,
		unsignedAPK,
	}
	prog.Logf("info", "运行 apksigner sign --ks %s --out %s", filepath.Base(keystorePath), filepath.Base(signed))

	// stdin 内容：先 ks-pass 再 key-pass，各一行
	stdinInput := password + "\n" + password + "\n"

	if err := runWithStdin(signer, args, dir, nil, stdinInput, prog); err != nil {
		return "", fmt.Errorf("apksigner sign 失败: %w", err)
	}

	// 签名验证
	prog.Log("info", "运行 apksigner verify…")
	if out, err := runWithOutput(signer, []string{"verify", "--verbose", signed}, dir, nil); err != nil {
		prog.Logf("warn", "apksigner verify 警告: %v\n%s", err, out)
	} else {
		// 简化：只 echo 第一行结果
		first := firstLine(out)
		if first != "" {
			prog.Logf("info", "  %s", first)
		}
	}
	return signed, nil
}

// runWithStdin 同 runWithOutput，但允许传 stdin 文本。
func runWithStdin(name string, args []string, workdir string, env []string, stdin string, prog *Progress) error {
	cmd := osexec.Command(name, args...)
	cmd.Dir = workdir
	if env != nil {
		cmd.Env = env
	}
	if stdin != "" {
		cmd.Stdin = strings.NewReader(stdin)
	}
	out, err := cmd.CombinedOutput()
	if len(out) > 0 && prog != nil {
		for _, line := range strings.Split(strings.TrimRight(string(out), "\r\n"), "\n") {
			if line != "" {
				prog.Log("info", "  "+strings.TrimRight(line, "\r"))
			}
		}
	}
	if err != nil {
		return fmt.Errorf("%v: %s", err, string(out))
	}
	return nil
}

// FinalizeOutputAPK 把签名好的 APK 拷到用户配置的输出目录，文件名按 LABEL-VERSION 命名。
//
// 命名规则：
//   - 优先用 cfg.Label（应用名称）做前缀，比 applicationId 可读
//   - Label 含路径不安全字符时做 sanitize；空值时用包名末段兜底
func FinalizeOutputAPK(signedAPK string, cfg *BuildConfig) (string, int64, error) {
	outDir := cfg.AbsPath(cfg.OutputDir)
	if err := os.MkdirAll(outDir, 0o755); err != nil {
		return "", 0, err
	}
	name := apkBaseName(cfg)
	target := filepath.Join(outDir, name+"-"+cfg.Version.Name+".apk")
	if err := copyFile(signedAPK, target); err != nil {
		return "", 0, err
	}
	st, err := os.Stat(target)
	if err != nil {
		return target, 0, err
	}
	return target, st.Size(), nil
}

// apkBaseName 派生 APK 文件名前缀。
//
// 优先使用应用名称（cfg.Label），剥掉路径不安全字符：
//   - 中文 / 空格直接保留（NTFS 支持，文件管理器双击友好）
//   - 真正会破坏文件名的字符（/ \ : * ? " < > |）替换成下划线
//   - Label 全部被替换或为空时退化到包名末段
func apkBaseName(cfg *BuildConfig) string {
	if name := sanitizeFileName(cfg.Label); name != "" {
		return name
	}
	// 用包名末段（com.foo.bar → bar）做兜底
	if cfg.Package != "" {
		idx := strings.LastIndex(cfg.Package, ".")
		if idx >= 0 && idx+1 < len(cfg.Package) {
			return sanitizeFileName(cfg.Package[idx+1:])
		}
		return sanitizeFileName(cfg.Package)
	}
	return "app"
}

// sanitizeFileName 把 Windows 文件名禁用字符替换成下划线，并去除前后空白。
// 中文字符保留，便于"中文应用名 → APK 文件名"的直观对应。
func sanitizeFileName(s string) string {
	s = strings.TrimSpace(s)
	if s == "" {
		return ""
	}
	var b strings.Builder
	for _, r := range s {
		switch r {
		case '/', '\\', ':', '*', '?', '"', '<', '>', '|', 0:
			b.WriteRune('_')
		default:
			if r < 0x20 {
				b.WriteRune('_')
			} else {
				b.WriteRune(r)
			}
		}
	}
	return strings.TrimSpace(b.String())
}
