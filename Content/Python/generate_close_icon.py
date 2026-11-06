# -*- coding: utf-8 -*-
"""画「关闭 / 取消」的红叉图标，风格和手机图标、罗盘图钉同一套。

离线生成 PNG：
    python Content/Python/generate_close_icon.py

在编辑器里（Cmd 模式）生成 + 导入：
    py generate_close_icon.py

产物：
    Content/UI/Task/Source/T_IconClose.png   （512x512，透明底）
    /Game/UI/Task/T_IconClose                 （贴图）

—— 调色板全是实采的，别手改色号 ——

  * 描边 `#000818` —— **深海军蓝不是黑**，和图钉外圈、手机图标同一个值。
    用纯黑画出来和别的图标并排一看就不是一套，而单看那一枚完全正常。
  * 奶油 `#f8e0bc` —— 底盘，和手机屏幕同色。
  * 红 `(239, 58, 44)` / 暗部 `(183, 32, 26)` —— **从小地图中心那个玩家标记抄的**
    （`DeliveryMinimapWidget.cpp:278`），不是我挑的好看的红。

—— 为什么是「奶油底盘 + 红叉」而不是「红底盘 + 白叉」 ——

用户要的是"红叉"。红底白叉里真正红的是底盘、叉是白的，和说法对不上；
而且满版红在 HUD 上很抢眼，关闭按钮不该比它要关的内容更显眼。
想换成红底盘的话，把 `DISC_FILL` 和 `CROSS_LIT/SHADE` 对调即可。
"""

from __future__ import annotations

import math
import os

from PIL import Image, ImageDraw, ImageFilter


HERE = os.path.dirname(os.path.abspath(__file__))
SOURCE_DIR = os.path.normpath(os.path.join(HERE, "..", "UI", "Task", "Source"))
OUTPUT = os.path.join(SOURCE_DIR, "T_IconClose.png")

DEST_DIR = "/Game/UI/Task"
TEXTURE_NAME = "T_IconClose"

SIZE = 512
SS = 4                      # 超采样；旋转和圆角不抗锯齿，直接画 512 边缘全是毛刺

# —— 实采色 ——
OUTLINE = (0, 8, 24)
DISC_FILL = (248, 224, 188)
DISC_DIM = (240, 216, 176)
CROSS_LIT = (239, 58, 44)
CROSS_SHADE = (183, 32, 26)

# —— 几何（占贴图半宽的比例）——
DISC_R = 0.80               # 底盘半径
OUTLINE_W = 0.038           # 描边粗细
ARM_HALF = 0.44             # 叉臂半长（从中心到端点）
ARM_HALF_W = 0.085          # 叉臂半宽
ARM_ROUND = 0.085           # 叉臂端头圆角 = 半宽，正好是半圆头

# **默认不画投影。** 图钉那些是浮在 3D 世界上的标记，投影帮它们和场景分开；
# 而这是贴在 UI 面板上的平面按钮，底色是什么都不知道。低透明度的海军蓝投影
# 压在浅色背景上**看着就是一圈灰脏边**——实测半径 215~228 处 alpha 从 64 衰到 5、
# 颜色一直是 (0,8,24)，在绿底上正是那圈灰。
# 真要给深色面板上用可以开回来。
DRAW_SHADOW = False
SHADOW_OFFSET = (0.025, 0.040)
SHADOW_BLUR = 0.045


def _vertical_gradient(size, top, bottom):
    grad = Image.new("RGB", (1, size[1]))
    load = grad.load()
    for y in range(size[1]):
        t = y / max(size[1] - 1, 1)
        load[0, y] = tuple(round(a + (b - a) * t) for a, b in zip(top, bottom))

    return grad.resize(size, Image.Resampling.BILINEAR)


def _cross_mask(s: float, half_len: float, half_w: float, radius: float) -> Image.Image:
    """两根圆角长条交叉成 ×。

    **用「画横条再整层旋转」而不是自己算旋转后的四边形顶点。**
    算顶点要自己处理圆角，端头会变成切角；而 PIL 的 `rounded_rectangle`
    画出来的是正经半圆头，旋转之后仍然是半圆头。
    """
    size = int(s)
    mask = Image.new("L", (size, size), 0)
    bar = Image.new("L", (size, size), 0)
    c = size / 2.0
    ImageDraw.Draw(bar).rounded_rectangle(
        [c - half_len, c - half_w, c + half_len, c + half_w],
        radius=radius, fill=255)

    for angle in (45, -45):
        rotated = bar.rotate(angle, resample=Image.Resampling.BICUBIC,
                             center=(c, c))
        mask = Image.composite(rotated, mask, rotated.point(lambda v: 255 if v > 0 else 0))
        # 上一行只在 rotated 非零处取它，等价于逐像素取大值；
        # 直接 paste 会把第二根条之外的区域清零、只剩一根。
        mask = Image.eval(mask, lambda v: v)

    return mask


def _add_shadow(canvas, s, c, px):
    """软投影。**只模糊 alpha 蒙版**——在 RGBA 上直接模糊会把透明区的 (0,0,0)
    混进颜色、把海军蓝拖成纯黑（手机图标那次自检当场抓到 `#000000`）。"""
    mask = Image.new("L", (s, s), 0)
    ImageDraw.Draw(mask).ellipse(
        [c - px(DISC_R) + px(SHADOW_OFFSET[0]), c - px(DISC_R) + px(SHADOW_OFFSET[1]),
         c + px(DISC_R) + px(SHADOW_OFFSET[0]), c + px(DISC_R) + px(SHADOW_OFFSET[1])],
        fill=110)
    mask = mask.filter(ImageFilter.GaussianBlur(px(SHADOW_BLUR)))
    shadow = Image.new("RGBA", (s, s), OUTLINE + (0,))
    shadow.putalpha(mask)

    return Image.alpha_composite(canvas, shadow)


def build() -> Image.Image:
    s = SIZE * SS
    half = s / 2.0
    c = half

    canvas = Image.new("RGBA", (s, s), (0, 0, 0, 0))

    def px(frac):
        return half * frac

    def box(radius_frac):
        r = px(radius_frac)
        return [c - r, c - r, c + r, c + r]

    if DRAW_SHADOW:
        canvas = _add_shadow(canvas, s, c, px)

    # ———— 底盘：描边用「大一圈的实心圆」当底，不用 outline= ————
    #
    # PIL 的 outline 宽度朝内外各长一半，粗描边画出来和填充对不齐。
    disc = Image.new("RGBA", (s, s), (0, 0, 0, 0))
    ImageDraw.Draw(disc).ellipse(box(DISC_R + OUTLINE_W), fill=OUTLINE + (255,))
    fill_mask = Image.new("L", (s, s), 0)
    ImageDraw.Draw(fill_mask).ellipse(box(DISC_R), fill=255)
    disc.paste(_vertical_gradient((s, s), DISC_FILL, DISC_DIM), (0, 0), fill_mask)
    canvas = Image.alpha_composite(canvas, disc)

    # ———— 红叉：先铺一层加粗的描边叉，再在上面盖正常粗细的红叉 ————
    outline_mask = _cross_mask(s, px(ARM_HALF) + px(OUTLINE_W),
                               px(ARM_HALF_W) + px(OUTLINE_W),
                               px(ARM_ROUND) + px(OUTLINE_W))
    cross_outline = Image.new("RGBA", (s, s), OUTLINE + (0,))
    cross_outline.putalpha(outline_mask)
    canvas = Image.alpha_composite(canvas, cross_outline)

    cross_mask = _cross_mask(s, px(ARM_HALF), px(ARM_HALF_W), px(ARM_ROUND))
    cross = Image.new("RGBA", (s, s), (0, 0, 0, 0))
    cross.paste(_vertical_gradient((s, s), CROSS_LIT, CROSS_SHADE), (0, 0), cross_mask)
    canvas = Image.alpha_composite(canvas, cross)

    # **缩放必须在预乘 alpha 下做**，否则透明区的黑会混进半透明边缘，
    # 表现是描边脏边、看着像贴图压缩坏了。
    return (canvas.convert("RGBa")
            .resize((SIZE, SIZE), Image.Resampling.LANCZOS)
            .convert("RGBA"))


# ───────────────────────── 自检 ─────────────────────────

def _check(image: Image.Image) -> None:
    px = image.load()
    c = SIZE // 2
    opaque = [(x, y) for y in range(SIZE) for x in range(SIZE) if px[x, y][3] > 8]
    if not opaque:
        raise AssertionError("整张图是空的。")

    xs = [p[0] for p in opaque]
    ys = [p[1] for p in opaque]
    margin = min(min(xs), min(ys), SIZE - 1 - max(xs), SIZE - 1 - max(ys))
    coverage = 100.0 * len(opaque) / (SIZE * SIZE)
    print("  不透明占比 %.1f%%，最小边距 %d px" % (coverage, margin))
    if margin < 6:
        raise AssertionError("内容贴到画布边了（最小边距 %d px）。" % margin)

    # 描边取暗部**中位色**，不取单个最暗像素——LANCZOS 的负旁瓣会把
    # `#000818` 的蓝分量（只有 24）过冲削到 0，极值是错的统计量。
    dark = [px[x, y][:3] for x, y in opaque
            if px[x, y][3] > 240 and sum(px[x, y][:3]) < 140]
    mid = tuple(sorted(ch[i] for ch in dark)[len(dark) // 2] for i in range(3))
    print("  描边中位色 #%02x%02x%02x（期望 #%02x%02x%02x）" % (*mid, *OUTLINE))
    if mid[2] < mid[0] + 8:
        raise AssertionError("描边中位色 #%02x%02x%02x 漂成黑/灰了。" % mid)

    def is_red(p):
        r, g, b, a = p
        return a > 200 and r > 140 and g < 110 and b < 110

    # **这条是关键：确认画的是 × 不是 +。**
    # 两者在"有红色像素""覆盖率"这些指标上完全一样，报告里分不出来，
    # 只有沿对角线和沿正交轴分别采样才看得见区别。
    # **探测半径按叉臂长度定，不能拍一个比例。** 第一版写死 `c * 0.45`，
    # 那是距中心 115 px，而叉臂半长只有 `ARM_HALF * c` = 112 px——
    # 探测点扎在臂尖外面，于是报"对角线 0/4 红、叉臂没画到位"，
    # **而图其实是对的**。这种"自检比被检的东西还错"最费时间，
    # 所以半径必须从同一组几何常量推出来。
    probe_r = ARM_HALF * c * 0.55          # 臂长一半处，稳稳在臂身内
    d = probe_r / math.sqrt(2.0)
    diagonal = [(int(c + sx * d), int(c + sy * d)) for sx in (-1, 1) for sy in (-1, 1)]
    probe = int(probe_r)
    axis = [(c + probe, c), (c - probe, c), (c, c + probe), (c, c - probe)]
    diag_red = sum(1 for x, y in diagonal if is_red(px[x, y]))
    axis_red = sum(1 for x, y in axis if is_red(px[x, y]))
    print("  对角线 4 点红 %d/4，正交轴 4 点红 %d/4（× 应是 4 和 0）" % (diag_red, axis_red))
    if diag_red != 4:
        raise AssertionError("对角线上只有 %d/4 个点是红的，叉臂没画到位。" % diag_red)
    if axis_red != 0:
        raise AssertionError("正交轴上有 %d/4 个点是红的——画成了 + 不是 ×。" % axis_red)

    # 圆心必须是红的（两根臂交叉处），否则是两根不相交的斜杠。
    if not is_red(px[c, c]):
        raise AssertionError("圆心不是红的，两根叉臂没交叉在中心。")

    # 奶油底盘要露得出来：采样四个"对角线之间"的位置。
    cream = 0
    for ang in (0, 90, 180, 270):
        a = math.radians(ang)
        x = int(c + c * 0.62 * math.cos(a))
        y = int(c + c * 0.62 * math.sin(a))
        r, g, b, al = px[x, y]
        if al > 200 and r > 200 and b > 140 and r > b:
            cream += 1
    print("  奶油底盘可见 %d/4 处" % cream)
    if cream < 3:
        raise AssertionError("底盘几乎被叉盖满（只露 %d/4），叉臂太粗。" % cream)


def _import(path: str):
    try:
        import unreal
    except ImportError:
        print("[关闭图标] !! 不在编辑器里，只生成了 PNG、**没有导入**。")
        print("[关闭图标]    在编辑器控制台跑：py generate_close_icon.py")
        return

    task = unreal.AssetImportTask()
    task.set_editor_property("filename", path)
    task.set_editor_property("destination_path", DEST_DIR)
    task.set_editor_property("destination_name", TEXTURE_NAME)
    task.set_editor_property("automated", True)
    task.set_editor_property("replace_existing", True)
    task.set_editor_property("save", True)
    # 必须显式指定 factory，UE 5.8 默认走 Interchange、不读下面这些设置。
    task.factory = unreal.TextureFactory()
    unreal.AssetToolsHelpers.get_asset_tools().import_asset_tasks([task])

    texture = unreal.load_asset("%s/%s" % (DEST_DIR, TEXTURE_NAME))
    if not texture:
        unreal.log_error("[关闭图标] 导入后载不到；空产出多半是 factory 没生效。")
        return

    # UI 贴图三件套，漏任一项分别表现为：描边脏边 / 缩小糊成一团 / 整张发暗。
    texture.set_editor_property("lod_group", unreal.TextureGroup.TEXTUREGROUP_UI)
    texture.set_editor_property(
        "compression_settings", unreal.TextureCompressionSettings.TC_EDITOR_ICON)
    texture.set_editor_property(
        "mip_gen_settings", unreal.TextureMipGenSettings.TMGS_NO_MIPMAPS)
    texture.set_editor_property("srgb", True)
    unreal.EditorAssetLibrary.save_loaded_asset(texture, only_if_is_dirty=False)
    unreal.log("[关闭图标] 已导入 %s/%s" % (DEST_DIR, TEXTURE_NAME))


def run() -> str:
    os.makedirs(SOURCE_DIR, exist_ok=True)
    print("[关闭图标] 自检：")
    image = build()
    _check(image)
    image.save(OUTPUT)
    print("[关闭图标] 已写 %s" % OUTPUT)
    _import(OUTPUT)

    return OUTPUT


if __name__ == "__main__":
    run()
