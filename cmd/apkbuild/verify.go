package main

import (
	"archive/zip"
	"bytes"
	"encoding/binary"
	"errors"
	"fmt"
	"io"
	"strings"
)

// VerifyAPK 静态校验 APK 内的 .so / .luac 是不是 easyLua 流水线产物。
//
// 当前断言：
//   - lib/<abi>/libeasylua.so 存在且 ELF 头 e_machine 与 abi 匹配
//   - assets/scripts/main.luac 存在且 magic = FB E3 EF 90（L2.5 obfus 头）
//   - assets/easylua/easylua.dex 存在且以 "dex\n" 开头
//
// 不做签名验证（apksigner 已经做过）。
func VerifyAPK(apkPath string, prog *Progress) error {
	zr, err := zip.OpenReader(apkPath)
	if err != nil {
		return fmt.Errorf("打开 APK 失败: %w", err)
	}
	defer zr.Close()

	files := map[string]*zip.File{}
	for _, f := range zr.File {
		files[f.Name] = f
	}

	// dex
	if f, ok := files["assets/easylua/easylua.dex"]; ok {
		head, err := readFileHead(f, 4)
		if err != nil {
			return fmt.Errorf("读 easylua.dex 失败: %w", err)
		}
		if !bytes.HasPrefix(head, []byte("dex\n")) {
			return fmt.Errorf("easylua.dex magic 异常: % X", head)
		}
		prog.Log("info", "✔ assets/easylua/easylua.dex magic OK")
	} else {
		return errors.New("APK 缺少 assets/easylua/easylua.dex")
	}

	// .so 多 ABI
	soChecked := 0
	for name, f := range files {
		if !strings.HasPrefix(name, "lib/") || !strings.HasSuffix(name, "/libeasylua.so") {
			continue
		}
		abi := strings.TrimSuffix(strings.TrimPrefix(name, "lib/"), "/libeasylua.so")
		head, err := readFileHead(f, 20)
		if err != nil {
			return fmt.Errorf("读 %s 失败: %w", name, err)
		}
		if len(head) < 20 || !bytes.HasPrefix(head, []byte{0x7F, 'E', 'L', 'F'}) {
			return fmt.Errorf("%s 不是 ELF 文件", name)
		}
		// e_machine：偏移 18，u16 little-endian
		em := binary.LittleEndian.Uint16(head[18:20])
		want := uint16(0)
		switch abi {
		case "arm64-v8a":
			want = 0xB7 // EM_AARCH64
		case "x86_64":
			want = 0x3E // EM_X86_64
		case "armeabi-v7a":
			want = 0x28 // EM_ARM
		case "x86":
			want = 0x03 // EM_386
		}
		if want != 0 && em != want {
			return fmt.Errorf("%s e_machine=0x%X 与 ABI %s 不匹配（期望 0x%X）", name, em, abi, want)
		}
		prog.Logf("info", "✔ lib/%s/libeasylua.so e_machine=0x%X", abi, em)
		soChecked++
	}
	if soChecked == 0 {
		return errors.New("APK 不包含任何 lib/<abi>/libeasylua.so")
	}

	// .luac magic
	luacChecked := 0
	for name, f := range files {
		if !strings.HasPrefix(name, "assets/scripts/") || !strings.HasSuffix(name, ".luac") {
			continue
		}
		head, err := readFileHead(f, 4)
		if err != nil {
			return fmt.Errorf("读 %s 失败: %w", name, err)
		}
		// L2.5 obfus magic = FB E3 EF 90
		expect := []byte{0xFB, 0xE3, 0xEF, 0x90}
		if !bytes.Equal(head, expect) {
			// 上游 LuaJIT magic = 1B 4C 4A 02；命中说明 obfus 没生效
			if bytes.HasPrefix(head, []byte{0x1B, 'L', 'J'}) {
				return fmt.Errorf("%s 是上游 LuaJIT 字节码（magic 1B 4C 4A），未走 obfus 流程", name)
			}
			return fmt.Errorf("%s magic 不是 obfus 头：% X", name, head)
		}
		luacChecked++
	}
	if luacChecked == 0 {
		prog.Log("warn", "APK 不含任何 assets/scripts/*.luac（无脚本？）")
	} else {
		prog.Logf("info", "✔ %d 个 .luac magic 都是 FB E3 EF 90", luacChecked)
	}

	return nil
}

func readFileHead(f *zip.File, n int) ([]byte, error) {
	rc, err := f.Open()
	if err != nil {
		return nil, err
	}
	defer rc.Close()
	buf := make([]byte, n)
	read, err := io.ReadFull(rc, buf)
	if err != nil && err != io.ErrUnexpectedEOF {
		return nil, err
	}
	return buf[:read], nil
}
