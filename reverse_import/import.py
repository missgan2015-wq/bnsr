#!/usr/bin/env python3
"""
把 jadx_libautogo 的反编译产物原样导入到 easylua/java/ 下。

策略
----
1. 第一版**保留所有原包名**（com.autogo.*, com.genymobile.scrcpy.*, com.activator.*, android.*）
   原因：这些类之间互相引用，盲目改包名会引入大量编译错误。
   先让原代码完整跑通（Stage A），再考虑改名（不在第一版做）。

2. **过滤**：去掉 jadx 自动生成的 `$$ExternalSyntheticLambda` 文件——这些是
   d8 生成的脱糖产物，源码里没有，jadx 误反成 .java；编译时 javac
   会自己生成新的 lambda，不需要带它们。

3. **去掉 jadx 头部注释**：'/* loaded from: xxx */' 这种是 jadx 加的，
   不影响编译，但留着碍眼，能去就去。
"""
import re
import shutil
from pathlib import Path

SRC_ROOTS = [
    # libAutoGo 配套 dex（GoJni / scrcpy / autogo.server / autogo.binder / autogo.vdm / android.*）
    Path(r"e:\auto_apk\reverse\jadx_libautogo\sources"),
    # com.activator daemon dex（shell.dex 解出来）
    Path(r"e:\auto_apk\reverse\jadx_activator\sources"),
]

DST = Path(r"e:\auto_apk\easylua\java")

# 不导入的文件（jadx 反编译伪产物 / 用不到的）
SKIP_PATTERNS = [
    re.compile(r"\$\$ExternalSyntheticLambda\d+\.java$"),  # lambda 脱糖产物
    re.compile(r"^R\.java$"),                              # 资源类，纯空
    re.compile(r"^BuildConfig\.java$"),                    # 构建配置，编译时会生成
]

# 去掉 jadx 加的元信息注释（多行）
REGEX_LOADED_FROM = re.compile(
    r"/\*\s*loaded from:[^*]*\*+(?:[^/*][^*]*\*+)*/\s*\n", re.MULTILINE)


def should_skip(name: str) -> bool:
    return any(p.search(name) for p in SKIP_PATTERNS)


def clean(text: str) -> str:
    return REGEX_LOADED_FROM.sub("", text)


def main():
    if DST.exists():
        shutil.rmtree(DST)
    DST.mkdir(parents=True, exist_ok=True)

    counts = {"copied": 0, "skipped": 0}

    for root in SRC_ROOTS:
        if not root.exists():
            print(f"[!] missing src root: {root}")
            continue
        for f in root.rglob("*.java"):
            if should_skip(f.name):
                counts["skipped"] += 1
                continue
            rel = f.relative_to(root)
            dest = DST / rel
            dest.parent.mkdir(parents=True, exist_ok=True)
            text = f.read_text(encoding="utf-8")
            text = clean(text)
            dest.write_text(text, encoding="utf-8")
            counts["copied"] += 1

    print(f"[+] copied {counts['copied']}, skipped {counts['skipped']}")
    print(f"[+] dest: {DST}")


if __name__ == "__main__":
    main()
