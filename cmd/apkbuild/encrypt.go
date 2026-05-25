// encrypt.go —— apkbuild 端的资源 / 字符串加密 helper。
//
// 算法与 Backport_Project/src/lj_obfus.c::lj_obfus_res_xor 完全同构：
//   流式 XOR + carry chain，自反；carry 只依赖 (i, key, salt, len)，与 buf 字节解耦。
//
// 用途：
//   - 方案 B1：把 ui.lua 加密成 ui.lua.enc 写到 assets/scripts/
//   - 方案 C ：把 assets/ 下非 .lua 资源加密成 <name>.enc 写到 assets/scripts/
//
// key 来源：从 embed runtime 里 lj_obfus_table.h（与 .so / .luac 同 seed 出）抽取
// LJ_OBFUS_RES_KEY[16]，确保 PC 端加密、设备端解密用同一 16 字节 key。

package main

import (
	"fmt"
	"os"
	"path/filepath"
	"regexp"
	"strconv"
	"strings"
)

// ResKey 当前加载的 16 字节资源加密密钥。LoadResKeyFromHeader 设置一次即可。
var resKey [16]byte
var resKeyLoaded bool

// LoadResKeyFromRuntime 从 embed 解出来的 lj_obfus_table.h 抽 LJ_OBFUS_RES_KEY[16]。
//
// 优先去 <runtime>/native/lj_obfus_table.h 找；prepare-embedded.ps1 没把 header
// 放进 embed 时退化到 Backport_Project/src/lj_obfus_table.h（开发期路径）。
func LoadResKeyFromRuntime(p *Paths) error {
	if resKeyLoaded {
		return nil
	}
	candidates := []string{
		filepath.Join(p.Runtime.Root, "native", "lj_obfus_table.h"),
	}
	// 开发期 fallback：apkbuild.exe 旁边的 Backport_Project/src/
	if exe, err := os.Executable(); err == nil {
		candidates = append(candidates,
			filepath.Join(filepath.Dir(exe), "..", "..", "Backport_Project", "src", "lj_obfus_table.h"),
			filepath.Join(filepath.Dir(exe), "Backport_Project", "src", "lj_obfus_table.h"),
		)
	}
	for _, c := range candidates {
		if data, err := os.ReadFile(c); err == nil {
			return parseResKey(data)
		}
	}
	return fmt.Errorf("找不到 lj_obfus_table.h（候选: %v）", candidates)
}

// parseResKey 从 header 文本里抽 LJ_OBFUS_RES_KEY 16 字节。
func parseResKey(data []byte) error {
	// 匹配 static const uint8_t LJ_OBFUS_RES_KEY[16] = { 0xXX, 0xYY, ... };
	re := regexp.MustCompile(`(?s)LJ_OBFUS_RES_KEY\[16\]\s*=\s*\{([^}]+)\}`)
	m := re.FindSubmatch(data)
	if m == nil {
		return fmt.Errorf("lj_obfus_table.h 里没找到 LJ_OBFUS_RES_KEY[16]")
	}
	hex := regexp.MustCompile(`0x([0-9A-Fa-f]{2})`).FindAllSubmatch(m[1], -1)
	if len(hex) != 16 {
		return fmt.Errorf("LJ_OBFUS_RES_KEY 字节数不对：%d 个", len(hex))
	}
	for i, h := range hex {
		v, err := strconv.ParseUint(string(h[1]), 16, 8)
		if err != nil {
			return fmt.Errorf("字节 #%d 解析失败：%v", i, err)
		}
		resKey[i] = byte(v)
	}
	resKeyLoaded = true
	return nil
}

// ResXor 对 buf 原地做流式 XOR 加 / 解密（自反）。
// salt 当前固定传 0，与 native 端 lib_io.c::io_open 的 .enc fallback 一致；
// 后续若想做"按相对路径派生 salt"以让同名文件密文不同，PC 端、device 端必须同步改。
func ResXor(buf []byte, salt uint32) {
	if !resKeyLoaded {
		// 严格守卫：未加载 key 就调用，必然出错；让调用方提早暴露。
		panic("ResXor 调用前 resKey 未加载，请先调 LoadResKeyFromRuntime")
	}
	n := uint32(len(buf))
	carry := byte(((n * 31) + (salt * 17) + 0x5A) & 0xFF)
	for i := uint32(0); i < n; i++ {
		k := resKey[(i+uint32(carry))&15]
		buf[i] ^= k
		carry = byte((uint32(carry) + uint32(k) + i) & 0xFF)
	}
}

// EncryptFile 把 src 读出来 → ResXor(salt) → 写到 dst。
func EncryptFile(src, dst string, salt uint32) error {
	data, err := os.ReadFile(src)
	if err != nil {
		return err
	}
	ResXor(data, salt)
	if err := os.MkdirAll(filepath.Dir(dst), 0o755); err != nil {
		return err
	}
	return os.WriteFile(dst, data, 0o644)
}

// EncryptBytes 加密一段内存数据（用于把内嵌字符串等加密后塞 .enc）。
func EncryptBytes(data []byte, salt uint32) []byte {
	out := make([]byte, len(data))
	copy(out, data)
	ResXor(out, salt)
	return out
}

// ResKeyHexDoubleQuoted 把 16 字节 key 渲染成 Kotlin/Java 字符串字面量友好的形式：
//
//	byteArrayOf(0x3D.toByte(), 0x55.toByte(), ..., 0xA5.toByte())
//
// 用于把 RES_KEY 烤进 EasyLuaSecret.kt（apkbuild 渲染期生成）。
func ResKeyKotlinByteArray() string {
	parts := make([]string, 16)
	for i, b := range resKey {
		parts[i] = fmt.Sprintf("0x%02X.toByte()", b)
	}
	return "byteArrayOf(" + strings.Join(parts, ", ") + ")"
}
