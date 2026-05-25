package com.easylua.support;

/**
 * 显示器信息的轻量值对象。
 *
 * 字段映射来自 android.view.DisplayInfo 的反射读取：
 *   - width/height ← logicalWidth/logicalHeight（按当前旋转后的逻辑分辨率）
 *   - rotation     ← rotation (0/1/2/3)
 *   - dpi          ← logicalDensityDpi
 *   - layerStack   ← layerStack（截屏时设置 setDisplayLayerStack 用到）
 *   - flags        ← flags（FLAG_SUPPORTS_PROTECTED_BUFFERS 等位掩码）
 *   - uniqueId     ← uniqueId（"local:0" / "virtual:autogo-..."）
 */
public final class SizeInfo {
    public final int displayId;
    public final int width;
    public final int height;
    public final int rotation;
    public final int dpi;
    public final int layerStack;
    public final int flags;
    public final String uniqueId;

    public SizeInfo(int displayId, int width, int height, int rotation,
                    int dpi, int layerStack, int flags, String uniqueId) {
        this.displayId = displayId;
        this.width = width;
        this.height = height;
        this.rotation = rotation;
        this.dpi = dpi;
        this.layerStack = layerStack;
        this.flags = flags;
        this.uniqueId = uniqueId;
    }
}
