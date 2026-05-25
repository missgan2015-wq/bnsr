# jniLibs - Go 可执行文件嵌入

本目录用于把 Go 端 `autogo-lua` 二进制随 APK 打包发布。

## 文件命名约定

文件必须命名为 **`libautogo-lua.so`**（以 `lib` 开头、`.so` 结尾），
否则安装时 Android 系统不会把它解压到可执行目录。

## 目录结构

```
jniLibs/
├── arm64-v8a/
│   └── libautogo-lua.so      ← ARM64 真机（高通/联发科/华为/小米...）
└── x86_64/
    └── libautogo-lua.so      ← x86_64 设备（主要是 Android 模拟器）
```

## 交叉编译 Go 二进制

需要 Android NDK（建议 r25c+）。

### ARM64
```bash
export ANDROID_NDK_HOME=/path/to/android-ndk
export TOOLCHAIN=$ANDROID_NDK_HOME/toolchains/llvm/prebuilt/<host>/bin

CGO_ENABLED=1 \
GOOS=android GOARCH=arm64 \
CC=$TOOLCHAIN/aarch64-linux-android21-clang \
go build -o libautogo-lua.so ./cmd/autogo-lua

# 拷到 jniLibs/arm64-v8a/
```

### x86_64（模拟器）
```bash
CGO_ENABLED=1 \
GOOS=android GOARCH=amd64 \
CC=$TOOLCHAIN/x86_64-linux-android21-clang \
go build -o libautogo-lua.so ./cmd/autogo-lua

# 拷到 jniLibs/x86_64/
```

## 运行时取真实路径

App 通过：
```kotlin
val path = "${ctx.applicationInfo.nativeLibraryDir}/libautogo-lua.so"
```

拿到的就是 `/data/app/<package>/lib/<abi>/libautogo-lua.so`，
ABI 已经被系统自动选好（与设备 CPU 匹配的版本），文件已经是可执行的。
