package main

import (
	"fmt"
	"os"
	"os/exec"
	"path/filepath"
	"runtime"
)

// EnsureKeystore 保证 keystore 文件存在；不存在则用 keytool 自动生成 debug 版本。
//
// 设计动机：用户跑打包时不应该被"先去用 keytool 生成 keystore"打断节奏，
// apkbuild 自动管理一份 debug keystore（路径 ~/.easylua/keystore/easylua-debug.jks）。
// 用户要做正式发布时，在 easylua.json 里显式配置 keystore 字段就能覆盖掉这套默认。
func EnsureKeystore(ks *KeystoreConfig, prog *Progress) error {
	if ks == nil {
		return fmt.Errorf("keystore 配置为空")
	}
	if ks.Path == "" {
		return fmt.Errorf("keystore.path 为空")
	}
	if exists(ks.Path) {
		return nil
	}
	prog.Logf("info", "keystore 不存在，自动生成: %s", ks.Path)
	if err := os.MkdirAll(filepath.Dir(ks.Path), 0o755); err != nil {
		return fmt.Errorf("创建 keystore 目录失败: %w", err)
	}
	keytool, err := findKeytool()
	if err != nil {
		return fmt.Errorf("找不到 keytool: %w（需要 JDK 17）", err)
	}
	password := ks.Password
	if password == "" {
		password = debugKeystorePassword
	}
	alias := ks.Alias
	if alias == "" {
		alias = debugKeystoreAlias
	}
	args := []string{
		"-genkey", "-v",
		"-keystore", ks.Path,
		"-keyalg", "RSA",
		"-keysize", "2048",
		"-validity", "10000",
		"-alias", alias,
		"-storepass", password,
		"-keypass", password,
		"-dname", "CN=easyLua, OU=Dev, O=easyLua, L=NA, ST=NA, C=CN",
	}
	out, err := exec.Command(keytool, args...).CombinedOutput()
	if err != nil {
		// 失败时把 keystore 残文件清掉，防止下次以为已存在
		_ = os.Remove(ks.Path)
		return fmt.Errorf("keytool 生成失败: %v\n%s", err, string(out))
	}
	prog.Logf("info", "已生成 debug keystore (alias=%s)", alias)
	return nil
}

func findKeytool() (string, error) {
	bin := "keytool"
	if runtime.GOOS == "windows" {
		bin = "keytool.exe"
	}
	// 优先 JAVA_HOME
	if jh := os.Getenv("JAVA_HOME"); jh != "" {
		p := filepath.Join(jh, "bin", bin)
		if exists(p) {
			return p, nil
		}
	}
	if p, err := exec.LookPath(bin); err == nil {
		return p, nil
	}
	return "", fmt.Errorf("PATH 与 JAVA_HOME/bin 都找不到 %s", bin)
}
