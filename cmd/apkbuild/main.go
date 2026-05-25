// apkbuild —— easyLua APK 打包流水线 CLI
//
// 子命令：
//   build         串完整流水线（check → compile → render → gradle → sign → verify）
//   check         仅跑环境检查
//   rotate-seed   重新生成 obfus seed + 重编 luajit-obfus
//   verify <apk>  对一个已存在 APK 做 magic 检查
//
// VSIX 端通过 stdout 上的 NDJSON 协议消费实时进度；详见 progress.go。
package main

import (
	"encoding/json"
	"flag"
	"fmt"
	"os"
	"path/filepath"
	"strings"
)

const usage = `apkbuild —— easyLua APK 打包流水线

用法:
  apkbuild build       [选项]              # 串完整流水线
  apkbuild check       [选项]              # 仅环境检查
  apkbuild verify <apk>                    # 校验现有 APK
  apkbuild --help

通用选项:
  --workspace <dir>     用户工作区根（默认当前目录）
  --workdir <dir>       临时构建目录（默认 <workspace>/.easylua/work-<ts>）

build 选项:
  --check-only          只跑前置检查，不实际打包

注：所有打包资源（模板 / .so / dex / host luajit）都已 embed 进 apkbuild.exe，
    首次运行会自动解到 %LOCALAPPDATA%\easyLua\runtime\<version>\，无需任何外部目录。
    keystore 由 apkbuild 内部管理；默认在 ~/.easylua/keystore/ 自动生成 debug 版本。
    需要正式 release 签名时，在 easylua.json 里显式配置 keystore 字段即可。
`

func main() {
	if len(os.Args) < 2 {
		fmt.Fprint(os.Stderr, usage)
		os.Exit(2)
	}

	// 自动探测 JAVA_HOME / ANDROID_HOME（用户没显式设置时）
	// 这一步必须在子命令解析之前跑，让 check.go 的探测拿到自动值。
	AutoDetectEnv()

	switch os.Args[1] {
	case "build":
		os.Exit(cmdBuild(os.Args[2:]))
	case "check":
		os.Exit(cmdCheck(os.Args[2:]))
	case "verify":
		os.Exit(cmdVerify(os.Args[2:]))
	case "-h", "--help", "help":
		fmt.Print(usage)
		os.Exit(0)
	default:
		fmt.Fprintf(os.Stderr, "未知子命令: %s\n\n%s", os.Args[1], usage)
		os.Exit(2)
	}
}

// ---- common flag parsing ---------------------------------------------------

type commonFlags struct {
	workspace string
	workdir   string
}

func parseCommon(fs *flag.FlagSet, args []string) (*commonFlags, []string) {
	c := &commonFlags{}
	fs.StringVar(&c.workspace, "workspace", "", "用户工作区根（默认当前目录）")
	fs.StringVar(&c.workdir, "workdir", "", "临时构建目录")
	// 兼容旧用法：忽略已废弃的 --easylua-root（embed 化后不再需要）
	var deprecated string
	fs.StringVar(&deprecated, "easylua-root", "", "已废弃（embed 化后不再使用）")
	_ = fs.Parse(args)
	return c, fs.Args()
}

func resolveCommon(c *commonFlags) (*Paths, *BuildConfig, error) {
	ws := c.workspace
	if ws == "" {
		var err error
		ws, err = os.Getwd()
		if err != nil {
			return nil, nil, err
		}
	}
	wsAbs, err := filepath.Abs(ws)
	if err != nil {
		return nil, nil, err
	}
	p, err := LocatePaths()
	if err != nil {
		return nil, nil, err
	}
	cfg, err := LoadConfig(wsAbs)
	if err != nil {
		return nil, nil, err
	}
	return p, cfg, nil
}

// defaultWorkDir 返回构建临时目录。固定路径 <workspace>/.easylua/work/，
// 每次构建前由 RenderTemplate 整体清空：每次都全量。
//
// 设计取舍：放弃"按时间戳隔离 + 增量缓存"模式，因为：
//   - 时间戳目录长期堆积（每次 ~50 MB gradle 缓存），用户得手动清
//   - gradle 增量虽然能快几十秒，但容易遇到"假绿"（缓存 stale 但 gradle 不知道）
//   - 全量构建只多 30 秒，可预测、可靠、可解释
//
// 后续若做增量热更新，会另开 hot-update 通道（pull 旧 luac + diff），
// 不会复用本地 gradle 缓存。
func defaultWorkDir(cfg *BuildConfig, override string) string {
	if override != "" {
		return override
	}
	return filepath.Join(cfg.workspaceRoot, ".easylua", "work")
}

// ---- build -----------------------------------------------------------------

func cmdBuild(args []string) int {
	fs := flag.NewFlagSet("build", flag.ContinueOnError)
	fs.SetOutput(os.Stderr)
	var checkOnly bool
	var abisFlag string
	var obfusDebug bool
	var keepWorkdir bool
	fs.BoolVar(&checkOnly, "check-only", false, "只跑前置检查，不实际打包")
	fs.StringVar(&abisFlag, "abis", "", "覆盖 easylua.json 的 abis 字段，逗号分隔（例：arm64-v8a,x86_64）")
	fs.BoolVar(&obfusDebug, "obfus-debug", false, "保留 .luac 调试信息（-g），便于看错误堆栈；正式打包请关闭")
	fs.BoolVar(&keepWorkdir, "keep-workdir", false, "打包结束后保留 <workspace>/.easylua/work/（默认会自动清理）")
	c, _ := parseCommon(fs, args)

	p, cfg, err := resolveCommon(c)
	if err != nil {
		fmt.Fprintf(os.Stderr, "apkbuild: %v\n", err)
		return 1
	}

	// CLI 参数覆盖 easylua.json 的字段（让 VSIX 弹窗里现选的设置生效）
	if abisFlag != "" {
		parts := []string{}
		for _, a := range strings.Split(abisFlag, ",") {
			a = strings.TrimSpace(a)
			if a != "" {
				parts = append(parts, a)
			}
		}
		if len(parts) > 0 {
			cfg.Abis = parts
		}
	}
	if obfusDebug {
		cfg.DebugObfus = true
	}

	prog := NewProgress(7)

	// Step 1: check
	prog.Step("check-env", "检查工具链与产物…")
	results, ok := RunCheck(p, cfg)
	for _, r := range results {
		level := "info"
		if !r.OK {
			level = "error"
		}
		mark := "✔"
		if !r.OK {
			mark = "✘"
		}
		prog.Logf(level, "  %s %s — %s", mark, r.Name, r.Detail)
		if r.Hint != "" {
			prog.Logf(level, "      提示：%s", r.Hint)
		}
	}
	if !ok {
		prog.Result(false, "环境检查失败，请先解决上面 ✘ 项再重试", "", 0)
		return 1
	}
	if err := cfg.Validate(); err != nil {
		prog.Log("error", err.Error())
		prog.Result(false, "配置校验失败", "", 0)
		return 1
	}
	if checkOnly {
		prog.SetTotal(1)
		prog.Result(true, "环境检查通过（--check-only 不打包）", "", 0)
		return 0
	}

	// Step 2: compile lua
	prog.Step("compile-lua", "编译用户脚本为 .luac…")
	workDir := defaultWorkDir(cfg, c.workdir)
	prog.Logf("info", "构建临时目录: %s", workDir)

	// Step 3: render template
	prog.Step("render-template", "渲染载体 APK 模板…")
	if err := RenderTemplate(p, cfg, workDir, prog); err != nil {
		prog.Log("error", err.Error())
		prog.Result(false, "模板渲染失败", "", 0)
		return 1
	}

	// 编译脚本（先渲染再编译——assets/scripts/ 目录由渲染步骤创建好）
	if err := CompileLua(p, cfg, workDir, prog); err != nil {
		prog.Log("error", err.Error())
		prog.Result(false, ".lua 编译失败", "", 0)
		return 1
	}

	// Step 4: gradle
	prog.Step("gradle-assemble", "执行 gradle assembleRelease…")
	if err := EnsureGradleWrapperJar(workDir, prog); err != nil {
		prog.Log("error", err.Error())
		prog.Result(false, "gradle wrapper 缺失", "", 0)
		return 1
	}
	unsignedAPK, err := RunGradleAssemble(workDir, prog)
	if err != nil {
		prog.Log("error", err.Error())
		prog.Result(false, "gradle 构建失败", "", 0)
		return 1
	}
	prog.Logf("info", "Gradle 产物: %s", unsignedAPK)

	// Step 5: sign
	prog.Step("apksigner", "对 APK 签名…")
	ks := cfg.ResolvedKeystore()
	if cfg.IsUsingDebugKeystore() {
		prog.Log("warn", "使用 apkbuild 内置 debug keystore（仅适合本地/测试用）")
		prog.Logf("warn", "  → 正式发布请在 easylua.json 添加 keystore 配置")
	}
	if err := EnsureKeystore(ks, prog); err != nil {
		prog.Log("error", err.Error())
		prog.Result(false, "keystore 自动生成失败", "", 0)
		return 1
	}
	signedAPK, err := SignAPK(unsignedAPK, cfg.AbsPath(ks.Path), ks.Alias, ks.Password, prog)
	if err != nil {
		prog.Log("error", err.Error())
		prog.Result(false, "APK 签名失败", "", 0)
		return 1
	}

	// Step 6: verify
	prog.Step("verify", "静态校验 APK 内容…")
	if err := VerifyAPK(signedAPK, prog); err != nil {
		prog.Log("error", err.Error())
		prog.Result(false, "APK 校验失败", signedAPK, 0)
		return 1
	}

	// Step 7: finalize
	prog.Step("done", "归集最终 APK…")
	finalPath, size, err := FinalizeOutputAPK(signedAPK, cfg)
	if err != nil {
		prog.Log("error", err.Error())
		prog.Result(false, "归集 APK 失败", signedAPK, 0)
		return 1
	}
	prog.Logf("info", "最终 APK: %s (%.1f KB)", finalPath, float64(size)/1024.0)

	// 成功后清理 work 目录：避免 .easylua 长期堆积（约 50 MB gradle 缓存）。
	// 用户传 --workdir 显式指定时不动（视为外部托管）；--keep-workdir 时也保留。
	if c.workdir == "" && !keepWorkdir {
		if err := os.RemoveAll(workDir); err != nil {
			prog.Logf("warn", "清理工作目录失败（不影响打包结果）：%v", err)
		} else {
			// 同时尝试清掉父目录 .easylua/，如果它已经空了
			parent := filepath.Dir(workDir)
			if entries, err := os.ReadDir(parent); err == nil && len(entries) == 0 {
				_ = os.Remove(parent)
			}
		}
	}

	prog.Result(true, "打包完成", finalPath, size/1024)
	return 0
}

// ---- check -----------------------------------------------------------------

func cmdCheck(args []string) int {
	fs := flag.NewFlagSet("check", flag.ContinueOnError)
	fs.SetOutput(os.Stderr)
	c, _ := parseCommon(fs, args)

	p, cfg, err := resolveCommon(c)
	if err != nil {
		fmt.Fprintf(os.Stderr, "apkbuild: %v\n", err)
		return 1
	}

	prog := NewProgress(1)
	prog.Step("check-env", "环境检查…")
	results, ok := RunCheck(p, cfg)
	// 同时把结构化结果作为最后一行输出，方便 VSIX 直接渲染表格
	for _, r := range results {
		level := "info"
		mark := "✔"
		if !r.OK {
			level = "error"
			mark = "✘"
		}
		prog.Logf(level, "  %s %s — %s", mark, r.Name, r.Detail)
		if r.Hint != "" {
			prog.Logf(level, "      提示：%s", r.Hint)
		}
	}
	dump, _ := json.Marshal(map[string]any{
		"type":    "checkReport",
		"ok":      ok,
		"results": results,
	})
	os.Stdout.Write(dump)
	os.Stdout.Write([]byte("\n"))

	if !ok {
		prog.Result(false, "环境检查失败", "", 0)
		return 1
	}
	prog.Result(true, "环境检查通过", "", 0)
	return 0
}

// ---- verify ----------------------------------------------------------------

func cmdVerify(args []string) int {
	fs := flag.NewFlagSet("verify", flag.ContinueOnError)
	fs.SetOutput(os.Stderr)
	c, rest := parseCommon(fs, args)
	if len(rest) == 0 {
		fmt.Fprintln(os.Stderr, "用法: apkbuild verify <apk>")
		return 2
	}
	apkPath, err := filepath.Abs(rest[0])
	if err != nil {
		fmt.Fprintf(os.Stderr, "apkbuild: %v\n", err)
		return 1
	}
	if !exists(apkPath) {
		fmt.Fprintf(os.Stderr, "APK 不存在: %s\n", apkPath)
		return 1
	}
	_ = c

	prog := NewProgress(1)
	prog.Step("verify", "校验 APK…")
	if err := VerifyAPK(apkPath, prog); err != nil {
		prog.Log("error", err.Error())
		prog.Result(false, "APK 校验失败", apkPath, 0)
		return 1
	}
	prog.Result(true, "APK 校验通过", apkPath, 0)
	return 0
}

// ---- helpers ---------------------------------------------------------------
