package main

import (
	"encoding/json"
	"fmt"
	"io"
	"os"
	"sync"
)

// Progress 把流水线状态以 NDJSON 的形式写到 stdout。
//
// 协议设计动机：VSIX 端要在打包过程里实时刷进度条 / 日志面板，又要严格隔离 apkbuild.exe
// 的内部细节（敏感算法、签名密钥都封死在这里）。NDJSON 比双向 RPC 简单，单向输出即可，
// 子进程退出码兜底告诉调用方是否成功。
type Progress struct {
	w     io.Writer
	mu    sync.Mutex
	step  int
	total int
}

// NewProgress 用 stdout 构造一个进度发射器。total 在第一次 Step 之前可以给 0，
// 后续 Step 调用里现填。
func NewProgress(total int) *Progress {
	return &Progress{w: os.Stdout, total: total}
}

// SetTotal 在流水线里中途调整总步数（例如 check-only 模式只跑前几步）。
func (p *Progress) SetTotal(total int) {
	p.mu.Lock()
	defer p.mu.Unlock()
	p.total = total
}

// Step 推进到下一步并广播 step 事件。msg 可以为空；name 是机器可读的子步骤标识，
// VSIX 端用 name 决定是否高亮某个子区域。
func (p *Progress) Step(name, msg string) {
	p.mu.Lock()
	p.step++
	step := p.step
	total := p.total
	p.mu.Unlock()
	p.emit(map[string]any{
		"type":  "step",
		"step":  step,
		"total": total,
		"name":  name,
		"msg":   msg,
	})
}

// Log 写一条日志。level 取 info / warn / error；其他值会被 VSIX 当 info 处理。
func (p *Progress) Log(level, msg string) {
	p.emit(map[string]any{
		"type":  "log",
		"level": level,
		"msg":   msg,
	})
}

// Logf 与 Log 一致，只是支持 fmt.Sprintf 风格。
func (p *Progress) Logf(level, format string, args ...any) {
	p.Log(level, fmt.Sprintf(format, args...))
}

// File 报告一个被编译/拷贝的文件。action 取 compiled / cached / failed / staged 等。
func (p *Progress) File(action, rel string, size int64, extra map[string]any) {
	ev := map[string]any{
		"type":   "file",
		"action": action,
		"rel":    rel,
	}
	if size > 0 {
		ev["size"] = size
	}
	for k, v := range extra {
		ev[k] = v
	}
	p.emit(ev)
}

// Result 写最终结果事件。ok=false 时 apkbuild 进程也会用非 0 退出码退出，
// 双保险保证 VSIX 不漏报错误。
func (p *Progress) Result(ok bool, msg, apkPath string, sizeKb int64) {
	ev := map[string]any{
		"type": "result",
		"ok":   ok,
		"msg":  msg,
	}
	if apkPath != "" {
		ev["apk"] = apkPath
	}
	if sizeKb > 0 {
		ev["sizeKb"] = sizeKb
	}
	p.emit(ev)
}

func (p *Progress) emit(ev map[string]any) {
	p.mu.Lock()
	defer p.mu.Unlock()
	b, err := json.Marshal(ev)
	if err != nil {
		// 序列化失败极少发生（map[string]any 里只放基本类型）；
		// 兜底写一行带类型 marker 的 stderr，避免把 stdout 弄出半截 JSON 行。
		fmt.Fprintf(os.Stderr, "apkbuild: progress emit failed: %v\n", err)
		return
	}
	p.w.Write(b)
	p.w.Write([]byte("\n"))
	if f, ok := p.w.(*os.File); ok {
		f.Sync()
	}
}
