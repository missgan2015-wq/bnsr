package main

import (
	"encoding/json"
	"errors"
	"fmt"
	"os"
	"path/filepath"
	"strings"
)

// BuildConfig 对应工作区根的 easylua.json。
//
// 用户面向的 schema 极简：只暴露 package / label / version 三个必填字段。
// SDK 版本、ABI、obfus、keystore 等 apkbuild 内部全部走默认值，不让用户填错。
//
//	{
//	  "package": "com.foo.myscript",
//	  "label": "我的脚本",
//	  "version": { "name": "1.0.0", "code": 1 }
//	}
//
// 高级字段（覆盖 deployDirName / debugObfus / keystore）保留 JSON 解析能力，
// 但默认不在面板里暴露；高级用户手动编辑 easylua.json 仍能生效。
type BuildConfig struct {
	Package string        `json:"package"`
	Label   string        `json:"label"`
	Version VersionConfig `json:"version"`

	// 以下都是高级覆盖项，保留 JSON 字段但默认不暴露给 UI
	Abis          []string `json:"abis,omitempty"`
	DeployDirName string   `json:"deployDirName,omitempty"` // 留空时按工作区根目录名派生
	DebugObfus    bool     `json:"debugObfus,omitempty"`    // -DebugInfo 透传，保留 .luac 行号；正式打包必关
	OutputDir     string   `json:"outputDir,omitempty"`     // APK 最终归集目录，默认 "out"
	Exclude       []string `json:"exclude,omitempty"`       // compile-lua.ps1 排除项

	// 高级用户自带 keystore（不指定时 apkbuild 自动生成 debug keystore）
	Keystore *KeystoreConfig `json:"keystore,omitempty"`

	// 内部用：VSIX 通过 --workdir 传递的临时构建目录（不写入 easylua.json）
	workspaceRoot string `json:"-"`

	// 派生字段（applyDefaults 计算填）；标 json:"-" 不进文件
	resolvedKeystore *KeystoreConfig `json:"-"`
}

type VersionConfig struct {
	Name string `json:"name"`
	Code int    `json:"code"`
}

type KeystoreConfig struct {
	Path     string `json:"path"`               // .jks 路径，相对工作区根或绝对
	Alias    string `json:"alias"`              // 默认 "easylua"
	Password string `json:"password,omitempty"` // 高级配置；缺失时回退到 APKBUILD_KS_PASS / 默认 debug 密码
}

// 写死的 SDK / ABI / obfus 默认值，不暴露给用户。
const (
	defaultMinSdk    = 24
	defaultTargetSdk = 34
	// debugKeystorePassword 用于自动生成的 debug keystore；不依赖此密码做严肃发布。
	// 用户要做 release 签名时通过 easylua.json 的 keystore 字段或环境变量覆盖。
	debugKeystorePassword = "easylua-debug"
	debugKeystoreAlias    = "easylua"
)

func defaultAbis() []string { return []string{"arm64-v8a", "x86_64"} }

// LoadConfig 读 easylua.json；不存在时返回带默认值的空 config（不报错），
// 让 CLI 在没有配置时也能跑 check 子命令。
func LoadConfig(workspaceRoot string) (*BuildConfig, error) {
	cfg := defaultConfig()
	cfg.workspaceRoot = workspaceRoot

	cfgPath := filepath.Join(workspaceRoot, "easylua.json")
	if data, err := os.ReadFile(cfgPath); err == nil {
		if err := json.Unmarshal(data, cfg); err != nil {
			return nil, fmt.Errorf("解析 easylua.json 失败: %w", err)
		}
	} else if !errors.Is(err, os.ErrNotExist) {
		return nil, fmt.Errorf("读 easylua.json 失败: %w", err)
	}
	cfg.applyDefaults()
	return cfg, nil
}

func defaultConfig() *BuildConfig {
	return &BuildConfig{
		Package: "",
		Label:   "",
		Version: VersionConfig{Name: "1.0.0", Code: 1},
		// 其余字段留空，applyDefaults 派生
	}
}

func (c *BuildConfig) applyDefaults() {
	if len(c.Abis) == 0 {
		c.Abis = defaultAbis()
	}
	if c.Version.Name == "" {
		c.Version.Name = "1.0.0"
	}
	if c.Version.Code == 0 {
		c.Version.Code = 1
	}
	if c.OutputDir == "" {
		c.OutputDir = "out"
	}
	// deployDirName 默认按工作区根目录名派生；与 VSIX 里 F5 运行的 deploy 路径一致
	if c.DeployDirName == "" {
		c.DeployDirName = deriveDeployDirName(c.workspaceRoot, c.Package)
	}
	// keystore：用户没在 easylua.json 写，apkbuild 用默认 debug keystore
	if c.Keystore == nil {
		c.resolvedKeystore = &KeystoreConfig{
			Path:     defaultDebugKeystorePath(),
			Alias:    debugKeystoreAlias,
			Password: debugKeystorePassword,
		}
	} else {
		ks := *c.Keystore
		if ks.Alias == "" {
			ks.Alias = debugKeystoreAlias
		}
		c.resolvedKeystore = &ks
	}
}

// ResolvedKeystore 返回 apkbuild 实际使用的 keystore 配置。
// 优先级：easylua.json 显式配置 > 默认 debug keystore（apkbuild 自动生成）。
func (c *BuildConfig) ResolvedKeystore() *KeystoreConfig {
	if c.resolvedKeystore == nil {
		c.applyDefaults()
	}
	return c.resolvedKeystore
}

// IsUsingDebugKeystore 判断当前 keystore 是否是 apkbuild 自动生成的 debug 版本。
// VSIX 端可以据此提示用户"当前是 debug 签名，正式发布请显式配置 keystore"。
func (c *BuildConfig) IsUsingDebugKeystore() bool {
	return c.Keystore == nil
}

// deriveDeployDirName 派生 SampleUI 模板的部署子目录名。
//
// 固定为 "easyLua"，与 SampleUI 仓库原版一致。多 APK 共存不互相覆盖靠的是：
//   - APK 启动时强制重部署（覆盖回自身资源）
//   - 用户不要同时打开多个 apkbuild 出来的 APK 跑脚本
func deriveDeployDirName(workspaceRoot, pkg string) string {
	_ = workspaceRoot
	_ = pkg
	return "easyLua"
}

// defaultDebugKeystorePath 返回 apkbuild 自动管理的 debug keystore 路径。
// 放在用户 home 下，避免污染工作区，也避免被 git 抓走。
func defaultDebugKeystorePath() string {
	home, err := os.UserHomeDir()
	if err != nil || home == "" {
		// fallback：临时目录（极端情况，正常 Windows 都能拿到 home）
		home = os.TempDir()
	}
	return filepath.Join(home, ".easylua", "keystore", "easylua-debug.jks")
}

// Validate 在 build 开始前做必填字段校验。check 子命令调用时会 fallback 给提示。
func (c *BuildConfig) Validate() error {
	var errs []string
	if c.workspaceRoot == "" {
		errs = append(errs, "未指定工作区根目录")
	}
	if !validPackage(c.Package) {
		errs = append(errs, fmt.Sprintf("应用包名 (package) 非法或为空: %q（要求形如 com.foo.bar）", c.Package))
	}
	if c.Label == "" {
		errs = append(errs, "应用名称 (label) 不能为空")
	}
	if c.Version.Name == "" || c.Version.Code <= 0 {
		errs = append(errs, "版本号 (version.name / version.code) 必填")
	}
	for _, abi := range c.Abis {
		if abi != "arm64-v8a" && abi != "x86_64" {
			errs = append(errs, fmt.Sprintf("不支持的 ABI: %q（仅 arm64-v8a / x86_64）", abi))
		}
	}
	if len(errs) > 0 {
		return fmt.Errorf("配置校验失败:\n  - %s", strings.Join(errs, "\n  - "))
	}
	return nil
}

// MinSdk / TargetSdk 都写死，UI 不暴露
func (c *BuildConfig) MinSdk() int    { return defaultMinSdk }
func (c *BuildConfig) TargetSdk() int { return defaultTargetSdk }
func (c *BuildConfig) ObfusEnabled() bool { return true }

// AbsPath 把 path 转成绝对路径：相对路径基于工作区根。
func (c *BuildConfig) AbsPath(path string) string {
	if path == "" {
		return ""
	}
	if filepath.IsAbs(path) {
		return path
	}
	return filepath.Join(c.workspaceRoot, path)
}

// Save 写回 easylua.json（VSIX UI 保存配置时用，CLI 一般只读不写）。
//
// 仅持久化用户面向的核心三字段（package / label / version）以及用户在
// easylua.json 里手动加的 keystore / debugObfus 等高级字段。SDK / ABI 等写死
// 默认值不写文件，避免用户误改导致打包失败。
func (c *BuildConfig) Save() error {
	if c.workspaceRoot == "" {
		return errors.New("workspaceRoot 为空，无法保存配置")
	}
	// 准备要序列化的对象：仅核心 + 用户已配置过的高级字段
	type savedConfig struct {
		Package    string          `json:"package"`
		Label      string          `json:"label"`
		Version    VersionConfig   `json:"version"`
		Keystore   *KeystoreConfig `json:"keystore,omitempty"`
		DebugObfus bool            `json:"debugObfus,omitempty"`
	}
	out := savedConfig{
		Package:    c.Package,
		Label:      c.Label,
		Version:    c.Version,
		Keystore:   c.Keystore,
		DebugObfus: c.DebugObfus,
	}
	data, err := json.MarshalIndent(out, "", "  ")
	if err != nil {
		return err
	}
	return os.WriteFile(filepath.Join(c.workspaceRoot, "easylua.json"), data, 0o644)
}

// validPackage 简化的 Java/Android 包名规则：至少两段，每段以字母开头 + 字母/数字/下划线。
func validPackage(pkg string) bool {
	if pkg == "" {
		return false
	}
	parts := strings.Split(pkg, ".")
	if len(parts) < 2 {
		return false
	}
	for _, p := range parts {
		if p == "" {
			return false
		}
		for i, r := range p {
			if i == 0 {
				if !(r == '_' || (r >= 'a' && r <= 'z') || (r >= 'A' && r <= 'Z')) {
					return false
				}
			} else {
				if !(r == '_' || (r >= 'a' && r <= 'z') || (r >= 'A' && r <= 'Z') || (r >= '0' && r <= '9')) {
					return false
				}
			}
		}
	}
	return true
}

func sanitizeIdent(s string) string {
	if s == "" {
		return "app"
	}
	var b strings.Builder
	for _, r := range s {
		switch {
		case (r >= 'a' && r <= 'z') || (r >= 'A' && r <= 'Z') || (r >= '0' && r <= '9'):
			b.WriteRune(r)
		case r == '-' || r == '_':
			b.WriteRune(r)
		default:
			b.WriteRune('_')
		}
	}
	out := b.String()
	if out == "" {
		return "app"
	}
	return out
}
