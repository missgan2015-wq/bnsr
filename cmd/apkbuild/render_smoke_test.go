package main

import (
	"os"
	"path/filepath"
	"strings"
	"testing"
)

// TestRenderTemplate 走一遍 RenderTemplate 全流程：
//   - 渲染 *.tmpl
//   - 替换 ScriptDeployer.kt 的 {{DEPLOY_DIR_NAME}}
//   - 填入 obfus 版 native 产物（embed 解出）
func TestRenderTemplate(t *testing.T) {
	p, err := LocatePaths()
	if err != nil {
		t.Fatalf("LocatePaths 失败: %v", err)
	}
	if !exists(p.TemplateRoot) {
		t.Skipf("缺 template_apk: %s", p.TemplateRoot)
	}
	if !exists(filepath.Join(p.NativeObfusRoot, "arm64-v8a", "libeasylua.so")) {
		t.Skipf("缺 embed 资源（先跑 prepare-embedded.ps1）")
	}

	// 用显式名字的工作区，方便测试 deployDirName 派生
	wsParent := t.TempDir()
	ws := filepath.Join(wsParent, "myscript")
	if err := os.MkdirAll(ws, 0o755); err != nil {
		t.Fatalf("建工作区失败: %v", err)
	}
	if err := os.WriteFile(filepath.Join(ws, "main.lua"), []byte("print('ok')"), 0o644); err != nil {
		t.Fatalf("写 main.lua 失败: %v", err)
	}

	cfg := defaultConfig()
	cfg.workspaceRoot = ws
	cfg.Package = "com.test.smoke"
	cfg.Label = "Smoke 测试包"
	cfg.applyDefaults()
	if err := cfg.Validate(); err != nil {
		t.Fatalf("Validate 失败: %v", err)
	}

	workDir := filepath.Join(t.TempDir(), "build")
	prog := NewProgress(1)
	if err := RenderTemplate(p, cfg, workDir, prog); err != nil {
		t.Fatalf("RenderTemplate 失败: %v", err)
	}

	// 1) build.gradle 应已渲染
	buildGradle := filepath.Join(workDir, "app", "build.gradle")
	data, err := os.ReadFile(buildGradle)
	if err != nil {
		t.Fatalf("读 build.gradle 失败: %v", err)
	}
	s := string(data)
	if !strings.Contains(s, "applicationId \"com.test.smoke\"") {
		t.Errorf("build.gradle 缺 applicationId: %s", s)
	}
	if strings.Contains(s, "{{") {
		t.Errorf("build.gradle 仍含未替换的占位符:\n%s", s)
	}

	// 2) AndroidManifest.xml.tmpl 应被消费
	if exists(filepath.Join(workDir, "app", "src", "main", "AndroidManifest.xml.tmpl")) {
		t.Errorf("AndroidManifest.xml.tmpl 没被删")
	}
	if !exists(filepath.Join(workDir, "app", "src", "main", "AndroidManifest.xml")) {
		t.Errorf("缺 AndroidManifest.xml")
	}

	// 3) ScriptRunner.kt 替换（SampleUI 包路径）
	deployer := filepath.Join(workDir, "app", "src", "main", "java", "com", "example", "sampleui", "ScriptRunner.kt")
	d, err := os.ReadFile(deployer)
	if err != nil {
		t.Fatalf("读 ScriptRunner.kt 失败: %v", err)
	}
	if strings.Contains(string(d), "{{DEPLOY_DIR_NAME}}") {
		t.Errorf("ScriptRunner.kt 占位符未替换")
	}
	// 默认派生：固定 "easyLua"（与 SampleUI 仓库原版一致）
	if !strings.Contains(string(d), "\"easyLua\"") {
		t.Errorf("ScriptRunner.kt deployDirName 异常: 找不到期望子串 'easyLua'")
	}

	// 4) 填入的 .so / .dex
	for _, abi := range cfg.Abis {
		so := filepath.Join(workDir, "app", "src", "main", "jniLibs", abi, "libeasylua.so")
		if !exists(so) {
			t.Errorf("缺 %s/libeasylua.so", abi)
		}
	}
	if !exists(filepath.Join(workDir, "app", "src", "main", "assets", "easylua", "easylua.dex")) {
		t.Errorf("缺 easylua.dex")
	}
}

// TestValidatePackage 校验包名解析的边界条件。
func TestValidatePackage(t *testing.T) {
	cases := []struct {
		pkg  string
		want bool
	}{
		{"com.foo.bar", true},
		{"com.foo", true},
		{"com.foo.bar_baz", true},
		{"com.foo.123bar", false}, // 段必须以字母/_ 开头
		{"com", false},            // 至少两段
		{"com.", false},
		{"com..foo", false},
		{"com.foo.", false},
		{"", false},
		{"com.foo-bar", false}, // - 不允许
	}
	for _, c := range cases {
		got := validPackage(c.pkg)
		if got != c.want {
			t.Errorf("validPackage(%q) = %v, want %v", c.pkg, got, c.want)
		}
	}
}

// TestSanitizeIdent 校验 deployDirName 自动派生逻辑。
func TestSanitizeIdent(t *testing.T) {
	cases := map[string]string{
		"myscript":   "myscript",
		"my_script":  "my_script",
		"my-script":  "my-script",
		"中文":         "__",
		"":           "app",
		"foo.bar":    "foo_bar",
	}
	for input, want := range cases {
		got := sanitizeIdent(input)
		if got != want {
			t.Errorf("sanitizeIdent(%q) = %q, want %q", input, got, want)
		}
	}
}

// TestDeriveDeployDirName 校验 SampleUI 部署子目录派生（固定 easyLua）。
func TestDeriveDeployDirName(t *testing.T) {
	cases := []struct {
		ws, pkg string
	}{
		{"E:\\proj\\test", "com.foo.bar"},
		{"E:\\proj\\test", ""},
		{"", "com.foo.bar"},
		{"", ""},
	}
	for _, c := range cases {
		got := deriveDeployDirName(c.ws, c.pkg)
		if got != "easyLua" {
			t.Errorf("deriveDeployDirName(%q, %q) = %q, want easyLua", c.ws, c.pkg, got)
		}
	}
}
