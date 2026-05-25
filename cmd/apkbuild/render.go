package main

import (
	"fmt"
	"io"
	"os"
	"path/filepath"
	"strings"
)

// RenderTemplate 把 sampleui_template/（embed 解压后 = template/）复制到 workDir，
// 并对所有 *.tmpl + 含 {{...}} 占位符的源文件做替换。
//
// 与 render-template.ps1 等价的 Go 实现。除模板渲染外还会一并：
//   - 填入 obfus 版 native 产物 (libeasylua.so + easylua.dex)
//   - 在 res/values/strings.xml 里渲染 LABEL
//   - 把 SampleUI ScriptRunner.kt 内的 {{DEPLOY_DIR_NAME}} 替换掉（它不带 .tmpl 后缀）
//   - 把 EasyLuaSecret.kt.tmpl 里 {{RES_KEY_BYTES}} 渲染为 byteArrayOf(0xXX.toByte(), ...)
//
// 注意：不在这里填用户脚本，那一步由 CompileLua 直接写到目标 assets/scripts/ 完成。
func RenderTemplate(p *Paths, cfg *BuildConfig, workDir string, prog *Progress) error {
	prog.Logf("info", "渲染模板 %s -> %s", p.TemplateRoot, workDir)

	// 加载 RES_KEY，让 EasyLuaSecret.kt.tmpl 占位符能被替换为真实 16 字节。
	// 失败直接返回错误：没 key 等于明文出包，违背安全意图。
	if err := LoadResKeyFromRuntime(p); err != nil {
		return fmt.Errorf("加载 RES_KEY 失败（无法渲染 EasyLuaSecret.kt）: %w", err)
	}

	// 干净的 workDir：每次重建，避免上次的占位符已被替换不可重渲染
	if err := os.RemoveAll(workDir); err != nil {
		return fmt.Errorf("清理 workDir 失败: %w", err)
	}
	if err := os.MkdirAll(workDir, 0o755); err != nil {
		return err
	}

	// 1) 拷贝模板（跳过 .gitkeep / 隐藏目录无意义）
	if err := copyDir(p.TemplateRoot, workDir, nil); err != nil {
		return fmt.Errorf("复制模板失败: %w", err)
	}

	// 2) 准备占位符表
	abiFilters := strings.Join(quoteAll(cfg.Abis), ", ")
	placeholders := map[string]string{
		"{{PKG}}":             cfg.Package,
		"{{LABEL}}":           cfg.Label,
		"{{VERSION_NAME}}":    cfg.Version.Name,
		"{{VERSION_CODE}}":    fmt.Sprintf("%d", cfg.Version.Code),
		"{{MIN_SDK}}":         fmt.Sprintf("%d", cfg.MinSdk()),
		"{{TARGET_SDK}}":      fmt.Sprintf("%d", cfg.TargetSdk()),
		"{{ABI_FILTERS}}":     abiFilters,
		"{{DEPLOY_DIR_NAME}}": cfg.DeployDirName,
		"{{RES_KEY_BYTES}}":   ResKeyKotlinByteArray(),
	}

	// 3) 渲染所有 *.tmpl
	tmplFiles := []string{}
	if err := filepath.Walk(workDir, func(path string, info os.FileInfo, err error) error {
		if err != nil {
			return err
		}
		if info.IsDir() {
			return nil
		}
		if strings.HasSuffix(path, ".tmpl") {
			tmplFiles = append(tmplFiles, path)
		}
		return nil
	}); err != nil {
		return err
	}
	for _, src := range tmplFiles {
		dst := strings.TrimSuffix(src, ".tmpl")
		if err := renderFile(src, dst, placeholders); err != nil {
			return fmt.Errorf("渲染 %s 失败: %w", filepath.Base(src), err)
		}
		_ = os.Remove(src)
		prog.Logf("info", "  渲染 %s", filepath.Base(dst))
	}

	// 4) ScriptRunner.kt 单独替换（它不是 .tmpl，是带占位符的真实 Kotlin 源文件）
	//    SampleUI 的 ScriptRunner 路径 = com/example/sampleui/ScriptRunner.kt
	scriptRunner := filepath.Join(workDir, "app", "src", "main", "java", "com", "example", "sampleui", "ScriptRunner.kt")
	if exists(scriptRunner) {
		if err := replaceInFile(scriptRunner, placeholders); err != nil {
			return fmt.Errorf("替换 ScriptRunner.kt 占位符失败: %w", err)
		}
	}

	// 5) 填入 obfus 版 native .so（embed 进二进制，与当前 obfus seed 对齐）
	for _, abi := range cfg.Abis {
		soSrc := filepath.Join(p.NativeObfusRoot, abi, "libeasylua.so")
		soDst := filepath.Join(workDir, "app", "src", "main", "jniLibs", abi, "libeasylua.so")
		if !exists(soSrc) {
			return fmt.Errorf("内嵌资源缺 %s/libeasylua.so（apkbuild.exe 损坏？）", abi)
		}
		if err := os.MkdirAll(filepath.Dir(soDst), 0o755); err != nil {
			return err
		}
		if err := copyFile(soSrc, soDst); err != nil {
			return fmt.Errorf("拷贝 %s 失败: %w", abi, err)
		}
		_ = os.Remove(filepath.Join(filepath.Dir(soDst), ".gitkeep"))
		prog.Logf("info", "  填入 %s/libeasylua.so", abi)
	}

	// 6) 填入 dex
	dexDst := filepath.Join(workDir, "app", "src", "main", "assets", "easylua", "easylua.dex")
	if !exists(p.DexFile) {
		return fmt.Errorf("内嵌资源缺 easylua.dex")
	}
	if err := copyFile(p.DexFile, dexDst); err != nil {
		return fmt.Errorf("拷贝 easylua.dex 失败: %w", err)
	}
	_ = os.Remove(filepath.Join(filepath.Dir(dexDst), ".gitkeep"))
	prog.Logf("info", "  填入 easylua.dex")

	// 7) 清掉 assets/scripts/.gitkeep（compile-lua 会写真实文件进去）
	scriptsDir := filepath.Join(workDir, "app", "src", "main", "assets", "scripts")
	if err := os.MkdirAll(scriptsDir, 0o755); err != nil {
		return err
	}
	_ = os.Remove(filepath.Join(scriptsDir, ".gitkeep"))

	return nil
}

// AssetsScriptsDir 返回 workDir 下的 app/src/main/assets/scripts/ 目录，
// compile 子流程把 .luac 写到这里。
func AssetsScriptsDir(workDir string) string {
	return filepath.Join(workDir, "app", "src", "main", "assets", "scripts")
}

// EnsureGradleWrapperJar 在 workDir 下检查 gradle/wrapper/gradle-wrapper.jar 是否存在；
// 不存在的话尝试用宿主 gradle 跑一次 `gradle wrapper`。
//
// 模板仓库里故意不入库 wrapper jar（避免二进制污染 git）；首次构建时由 apkbuild 自动 bootstrap。
func EnsureGradleWrapperJar(workDir string, prog *Progress) error {
	jar := filepath.Join(workDir, "gradle", "wrapper", "gradle-wrapper.jar")
	if exists(jar) {
		return nil
	}
	prog.Log("info", "gradle-wrapper.jar 缺失，尝试用宿主 gradle 自动 bootstrap…")

	gradle, err := findGradleExe()
	if err != nil {
		return fmt.Errorf("自动 bootstrap 失败：%w（请在工作机装 gradle 8.x，或手动执行 gradle wrapper --gradle-version 8.5）", err)
	}
	out, err := runWithOutput(gradle, []string{"wrapper", "--gradle-version", "8.5"}, workDir, nil)
	if err != nil {
		return fmt.Errorf("gradle wrapper bootstrap 失败: %v\n%s", err, out)
	}
	if !exists(jar) {
		return fmt.Errorf("gradle wrapper 跑完仍然没生成 %s", jar)
	}
	prog.Log("info", "gradle wrapper bootstrap 成功")
	return nil
}

// ---- helpers --------------------------------------------------------------

func quoteAll(arr []string) []string {
	out := make([]string, len(arr))
	for i, s := range arr {
		out[i] = "'" + s + "'"
	}
	return out
}

// copyDir 递归复制 src → dst。skip 函数返回 true 表示跳过该路径；可为 nil。
func copyDir(src, dst string, skip func(rel string) bool) error {
	return filepath.Walk(src, func(path string, info os.FileInfo, err error) error {
		if err != nil {
			return err
		}
		rel, err := filepath.Rel(src, path)
		if err != nil {
			return err
		}
		if rel == "." {
			return nil
		}
		if skip != nil && skip(rel) {
			if info.IsDir() {
				return filepath.SkipDir
			}
			return nil
		}
		target := filepath.Join(dst, rel)
		if info.IsDir() {
			return os.MkdirAll(target, info.Mode())
		}
		if err := os.MkdirAll(filepath.Dir(target), 0o755); err != nil {
			return err
		}
		return copyFile(path, target)
	})
}

func copyFile(src, dst string) error {
	in, err := os.Open(src)
	if err != nil {
		return err
	}
	defer in.Close()
	if err := os.MkdirAll(filepath.Dir(dst), 0o755); err != nil {
		return err
	}
	out, err := os.Create(dst)
	if err != nil {
		return err
	}
	defer out.Close()
	if _, err := io.Copy(out, in); err != nil {
		return err
	}
	return out.Sync()
}

// renderFile 用 placeholders 替换 src 内容并写到 dst。文件按 UTF-8 文本处理。
func renderFile(src, dst string, placeholders map[string]string) error {
	data, err := os.ReadFile(src)
	if err != nil {
		return err
	}
	s := string(data)
	for k, v := range placeholders {
		s = strings.ReplaceAll(s, k, v)
	}
	if err := os.MkdirAll(filepath.Dir(dst), 0o755); err != nil {
		return err
	}
	return os.WriteFile(dst, []byte(s), 0o644)
}

func replaceInFile(path string, placeholders map[string]string) error {
	data, err := os.ReadFile(path)
	if err != nil {
		return err
	}
	s := string(data)
	changed := false
	for k, v := range placeholders {
		if strings.Contains(s, k) {
			s = strings.ReplaceAll(s, k, v)
			changed = true
		}
	}
	if !changed {
		return nil
	}
	return os.WriteFile(path, []byte(s), 0o644)
}
