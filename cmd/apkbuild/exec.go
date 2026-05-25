package main

import (
	"bufio"
	"bytes"
	"fmt"
	"io"
	"os/exec"
	"strings"
)

// runWithOutput 同步执行命令，把合并的 stdout/stderr 整体返回。
// 失败时 err 非 nil，输出仍然返回，方便上层把诊断信息塞进进度日志。
func runWithOutput(name string, args []string, workdir string, env []string) (string, error) {
	cmd := exec.Command(name, args...)
	cmd.Dir = workdir
	if env != nil {
		cmd.Env = env
	}
	var buf bytes.Buffer
	cmd.Stdout = &buf
	cmd.Stderr = &buf
	err := cmd.Run()
	return buf.String(), err
}

// runStreamLines 同步执行命令，把 stdout / stderr 按行回调给 onLine。
//
// 用于 gradle / pwsh 这种长输出，让 VSIX 端能实时看到子进程日志而不是
// 等命令跑完才一口气吐出来。
func runStreamLines(name string, args []string, workdir string, env []string, onLine func(line string, isErr bool)) error {
	cmd := exec.Command(name, args...)
	cmd.Dir = workdir
	if env != nil {
		cmd.Env = env
	}
	stdout, err := cmd.StdoutPipe()
	if err != nil {
		return err
	}
	stderr, err := cmd.StderrPipe()
	if err != nil {
		return err
	}
	if err := cmd.Start(); err != nil {
		return err
	}

	done := make(chan struct{}, 2)
	go pumpLines(stdout, false, onLine, done)
	go pumpLines(stderr, true, onLine, done)
	<-done
	<-done
	return cmd.Wait()
}

func pumpLines(r io.ReadCloser, isErr bool, onLine func(string, bool), done chan struct{}) {
	defer func() { done <- struct{}{} }()
	defer r.Close()
	br := bufio.NewReader(r)
	for {
		line, err := br.ReadString('\n')
		if line != "" {
			line = strings.TrimRight(line, "\r\n")
			if onLine != nil {
				onLine(line, isErr)
			}
		}
		if err != nil {
			return
		}
	}
}

// findPwsh 优先选 pwsh（PowerShell 7），找不到时退到 powershell.exe。
// 现有 PowerShell 脚本会用到 UTF-8 中文输出，pwsh 默认 UTF-8 兼容性更好。
func findPwsh() (string, error) {
	for _, name := range []string{"pwsh", "powershell"} {
		if p, err := exec.LookPath(name); err == nil {
			return p, nil
		}
	}
	return "", fmt.Errorf("找不到 pwsh 或 powershell.exe")
}

// findGradleExe 优先选系统 gradle，用于 wrapper jar bootstrap。
func findGradleExe() (string, error) {
	for _, name := range []string{"gradle.bat", "gradle"} {
		if p, err := exec.LookPath(name); err == nil {
			return p, nil
		}
	}
	return "", fmt.Errorf("PATH 里找不到 gradle")
}
