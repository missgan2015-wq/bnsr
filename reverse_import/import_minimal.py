#!/usr/bin/env python3
"""
最小子集导入：只复制截屏闭环需要的类。

闭环依赖（手工梳理）：
- com.autogo.server.Main          ← 入口（需简化）
- com.autogo.GoJni                ← native 接口持有方
- com.autogo.server.ScreenShot.ScreenShot
- com.genymobile.scrcpy.FakeContext
- com.genymobile.scrcpy.Workarounds
- com.genymobile.scrcpy.wrappers.SurfaceControl     ← hidden API 反射核心
- com.genymobile.scrcpy.wrappers.ServiceManager
- com.genymobile.scrcpy.wrappers.DisplayManager
- com.genymobile.scrcpy.wrappers.DisplayControl
- com.genymobile.scrcpy.wrappers.WindowManager
- com.genymobile.scrcpy.wrappers.PowerManager
- com.genymobile.scrcpy.wrappers.ContentProvider
- com.genymobile.scrcpy.device.DisplayInfo
- com.genymobile.scrcpy.device.Size
- com.genymobile.scrcpy.device.Orientation
- com.genymobile.scrcpy.device.Device              ← 因为 GoJni 用了
- com.genymobile.scrcpy.util.Ln
- com.autogo.server.Utils.Utils                    ← 部分依赖
- com.autogo.server.Utils.OverlayUI                ← Toast 显示，先空实现
- android.* stubs                                  ← jadx 生成的系统类签名占位
"""
import re
import shutil
from pathlib import Path

SRC = Path(r"e:\auto_apk\reverse\jadx_libautogo\sources")
SRC2 = Path(r"e:\auto_apk\reverse\jadx_activator\sources")
DST = Path(r"e:\auto_apk\easylua\java")

# 必要的类（精确匹配相对路径）
WHITELIST = {
    # autogo 入口 + 截屏
    "com/autogo/GoJni.java",
    "com/autogo/server/Main.java",
    "com/autogo/server/ScreenShot/ScreenShot.java",
    "com/autogo/server/Utils/Utils.java",
    "com/autogo/server/Utils/OverlayUI.java",   # Utils 引用了，但我们将简化为 stub

    # binder 接口（GoJni / Utils 引用）
    "com/autogo/binder/IAutoGoService.java",

    # scrcpy 核心
    "com/genymobile/scrcpy/FakeContext.java",
    "com/genymobile/scrcpy/Workarounds.java",
    "com/genymobile/scrcpy/wrappers/ServiceManager.java",
    "com/genymobile/scrcpy/wrappers/SurfaceControl.java",
    "com/genymobile/scrcpy/wrappers/DisplayManager.java",
    "com/genymobile/scrcpy/wrappers/DisplayControl.java",
    "com/genymobile/scrcpy/wrappers/WindowManager.java",
    "com/genymobile/scrcpy/wrappers/PowerManager.java",
    "com/genymobile/scrcpy/wrappers/ActivityManager.java",
    "com/genymobile/scrcpy/wrappers/ClipboardManager.java",
    "com/genymobile/scrcpy/wrappers/ContentProvider.java",
    "com/genymobile/scrcpy/wrappers/StatusBarManager.java",
    "com/genymobile/scrcpy/wrappers/InputManager.java",
    "com/genymobile/scrcpy/wrappers/DisplayWindowListener.java",
    "com/genymobile/scrcpy/device/DisplayInfo.java",
    "com/genymobile/scrcpy/device/Size.java",
    "com/genymobile/scrcpy/device/Orientation.java",
    "com/genymobile/scrcpy/device/Device.java",
    "com/genymobile/scrcpy/device/DeviceApp.java",
    "com/genymobile/scrcpy/device/Point.java",
    "com/genymobile/scrcpy/device/ConfigurationException.java",
    "com/genymobile/scrcpy/util/Ln.java",
    "com/genymobile/scrcpy/util/Command.java",
    "com/genymobile/scrcpy/util/SettingsException.java",
    "com/genymobile/scrcpy/AndroidVersions.java",

    # android.* stubs（jadx 生成的 hidden API 接口签名）
    "android/accessibilityservice/IAccessibilityServiceClient.java",
    "android/app/ActivityManagerNative.java",
    "android/app/IActivityManager.java",
    "android/app/ContentProviderHolder.java",
    "android/content/IContentProvider.java",
    "android/content/IClipboard.java",
    "android/content/IOnPrimaryClipChangedListener.java",
    "android/graphics/GraphicBuffer.java",
    "android/hardware/display/IDisplayManager.java",
    "android/hardware/display/DisplayManagerGlobal.java",
    "android/hardware/display/IVirtualDisplayCallback.java",
    "android/os/ICancellationSignal.java",
    "android/os/ServiceManager.java",
    "android/view/Surface.java",
    "android/view/SurfaceControl.java",
    "android/view/IDisplayWindowListener.java",
    "android/view/IRotationWatcher.java",
    "android/view/DisplayAdjustments.java",
    "android/view/DisplayInfo.java",
    "android/view/WindowAnimationFrameStats.java",
    "android/view/accessibility/AccessibilityInteractionClient.java",
    "android/view/accessibility/AccessibilityWindowInfo.java",
}

REGEX_LOADED_FROM = re.compile(
    r"/\*\s*loaded from:[^*]*\*+(?:[^/*][^*]*\*+)*/\s*\n", re.MULTILINE)


def main():
    if DST.exists():
        shutil.rmtree(DST)
    DST.mkdir(parents=True, exist_ok=True)

    copied = 0
    missing = []
    for rel in sorted(WHITELIST):
        src = SRC / rel
        if not src.exists():
            src = SRC2 / rel
        if not src.exists():
            missing.append(rel)
            continue
        dest = DST / rel
        dest.parent.mkdir(parents=True, exist_ok=True)
        text = src.read_text(encoding="utf-8")
        text = REGEX_LOADED_FROM.sub("", text)
        dest.write_text(text, encoding="utf-8")
        copied += 1

    print(f"[+] copied {copied}")
    if missing:
        print(f"[-] missing {len(missing)}:")
        for m in missing:
            print(f"    {m}")


if __name__ == "__main__":
    main()
