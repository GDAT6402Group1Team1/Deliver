# -*- coding: utf-8 -*-
"""画手机**面板**（不是图标）：屏幕右侧那块能显示详情、能点的大手机外壳。

    python Content/Python/generate_phone_panel.py

产物：`Content/UI/Phone/Source/T_PhonePanel.png`（1024 x 1664，背景透明）

—— 和 `generate_phone_icon.py` 的分工 ——

那个是**图标**：512 方图，给 HUD 上的小标记用，整枚一起缩放。
这个是**面板**：竖长图，机身当边框，**屏幕区域是留给 UMG 放控件的**，
所以它的关键产出不只是图，还有脚本最后打印的那个**屏幕矩形**——
UMG 里要按那个矩形摆一个容器，手机壳和里面的内容才对得齐。

两者的调色板从图标那边 import 过来，**不在这里重抄一份**。抄一份的结果是
改一处忘另一处，表现是"面板和图标的蓝不是同一个蓝"，而两张单看都正常。

—— 为什么不做成方图 ——

手机是 1 : 1.68 的竖长物体，塞进方图要浪费四成面积，而且在 UMG 里还得
靠 size box 去配比例——那是个永远对不准的活。贴图按内容比例做，
摆的时候只给高度、宽度让它自己按比例走。

—— 为什么屏幕是**实心奶油**而不是挖空 ——

挖空的话内容直接透到背后的游戏画面上，文字压在乱七八糟的场景上读不清。
实心奶油底是个稳定的阅读背景，UMG 控件直接盖在上面。
代价是屏幕上那道高光会被内容盖住；真要高光就再出一张只有高光的透明图，
盖在内容**之上**——不过那是后面要不要的事，现在没做。
"""

from __future__ import annotations

import os

from PIL import Image, ImageDraw, ImageFilter

# 调色板只有一个来源，见模块注释
from generate_phone_icon import (
    OUTLINE, BLUE_LIT, BLUE_MID, BLUE_SHADE, CREAM, CREAM_DIM, ORANGE,
)


HERE = os.path.dirname(os.path.abspath(__file__))
SOURCE_DIR = os.path.normpath(os.path.join(HERE, "..", "UI", "Phone", "Source"))
OUTPUT = os.path.join(SOURCE_DIR, "T_PhonePanel.png")

W, H = 1024, 1664
SS = 2                      # 面板本来就大，2 倍超采样够了；4 倍要画 4096x6656

# —— 机身 ——
#
# 比图标那枚**瘦一点**（1 : 1.68，图标是 1 : 1.55）。图标要在很小的尺寸下
# 一眼认出轮廓，所以得胖；面板是大图，太胖会显得像平板。
BODY_W, BODY_H = 880.0, 1480.0
BODY_R = 110.0
OUTLINE_W = 16.0            # 按面板尺寸等比放大（图标上是 512 画布配 9px）
CX, CY = W / 2.0, 806.0     # 比正中略高，给底部投影留地方

# —— 屏幕 ——
#
# 上下边框不对称是**故意的**：下边框要塞 Home 键，上边框只有听筒。
# 对称的话要么 Home 键顶进屏幕（图标那版踩过），要么上边框空一大块。
SCREEN_INSET_X = 60.0
SCREEN_TOP = 150.0
SCREEN_BOTTOM = 190.0
SCREEN_R = 56.0

EAR_W, EAR_H = 190.0, 30.0  # 听筒
CAM_R = 17.0                # 摄像头，听筒右边那个小圆点
BTN_R = 56.0                # Home 键

# 侧键。图标上没有这两个——小尺寸下它们糊成两个点、只会让轮廓变脏；
# 面板这么大不画反而显得是个光板子。
SIDE_BTN_W, SIDE_BTN_H = 14.0, 150.0
SIDE_BTN_GAP = 40.0

SHADOW_OFFSET = (16.0, 26.0)
SHADOW_BLUR = 26.0


def _px(v: float) -> float:
    return v * SS


def _rect(cx: float, cy: float, w: float, h: float):
    return [_px(cx - w / 2), _px(cy - h / 2), _px(cx + w / 2), _px(cy + h / 2)]


def screen_rect() -> tuple:
    """屏幕区域，返回 (left, top, right, bottom)，单位是最终贴图的像素。"""
    left = CX - (BODY_W - SCREEN_INSET_X * 2) / 2.0
    right = CX + (BODY_W - SCREEN_INSET_X * 2) / 2.0
    top = CY - BODY_H / 2.0 + SCREEN_TOP
    bottom = CY + BODY_H / 2.0 - SCREEN_BOTTOM

    return (left, top, right, bottom)


def _vertical_gradient(size, top, bottom):
    grad = Image.new("RGB", (1, size[1]))
    load = grad.load()
    for y in range(size[1]):
        t = y / max(size[1] - 1, 1)
        load[0, y] = tuple(round(a + (b - a) * t) for a, b in zip(top, bottom))

    return grad.resize(size, Image.Resampling.BILINEAR)


def build() -> Image.Image:
    sw, sh = W * SS, H * SS
    canvas = Image.new("RGBA", (sw, sh), (0, 0, 0, 0))

    # ———— 投影：只模糊 alpha 蒙版 ————
    #
    # 在 RGBA 上直接模糊会把透明区的 (0,0,0) 混进颜色、把海军蓝拖成纯黑。
    # 图标那边踩过，判据和修法都在 generate_phone_icon.py 里写着。
    shadow_mask = Image.new("L", (sw, sh), 0)
    ImageDraw.Draw(shadow_mask).rounded_rectangle(
        _rect(CX + SHADOW_OFFSET[0], CY + SHADOW_OFFSET[1],
              BODY_W + OUTLINE_W, BODY_H + OUTLINE_W),
        radius=_px(BODY_R), fill=120)
    shadow_mask = shadow_mask.filter(ImageFilter.GaussianBlur(_px(SHADOW_BLUR)))
    shadow = Image.new("RGBA", (sw, sh), OUTLINE + (0,))
    shadow.putalpha(shadow_mask)
    canvas = Image.alpha_composite(canvas, shadow)

    # ———— 侧键（画在机身底下，只露出一半）————
    side = Image.new("RGBA", (sw, sh), (0, 0, 0, 0))
    sd = ImageDraw.Draw(side)
    for sign, offsets in ((-1, (-260.0, -80.0)), (1, (-200.0,))):
        for oy in offsets:
            bx = CX + sign * (BODY_W / 2.0 + SIDE_BTN_W / 2.0 - 4.0)
            sd.rounded_rectangle(
                _rect(bx, CY + oy, SIDE_BTN_W + OUTLINE_W, SIDE_BTN_H + OUTLINE_W),
                radius=_px((SIDE_BTN_W + OUTLINE_W) / 2), fill=OUTLINE + (255,))
            sd.rounded_rectangle(
                _rect(bx, CY + oy, SIDE_BTN_W, SIDE_BTN_H),
                radius=_px(SIDE_BTN_W / 2), fill=BLUE_SHADE + (255,))
    canvas = Image.alpha_composite(canvas, side)

    # ———— 机身：描边用"大一圈的实心圆角矩形"，不用 outline= ————
    #
    # PIL 的 outline 宽度朝内外各长一半、圆角处还会变细，粗描边画出来
    # 四个角明显比直边细，看着像圆角画崩了。
    body = Image.new("RGBA", (sw, sh), (0, 0, 0, 0))
    ImageDraw.Draw(body).rounded_rectangle(
        _rect(CX, CY, BODY_W + OUTLINE_W * 2, BODY_H + OUTLINE_W * 2),
        radius=_px(BODY_R + OUTLINE_W), fill=OUTLINE + (255,))
    fill_mask = Image.new("L", (sw, sh), 0)
    ImageDraw.Draw(fill_mask).rounded_rectangle(
        _rect(CX, CY, BODY_W, BODY_H), radius=_px(BODY_R), fill=255)
    body.paste(_vertical_gradient((sw, sh), BLUE_LIT, BLUE_SHADE), (0, 0), fill_mask)
    canvas = Image.alpha_composite(canvas, body)

    draw = ImageDraw.Draw(canvas)

    # ———— 屏幕 ————
    left, top, right, bottom = screen_rect()
    scx, scy = (left + right) / 2.0, (top + bottom) / 2.0
    sw_, sh_ = right - left, bottom - top

    draw.rounded_rectangle(
        _rect(scx, scy, sw_ + OUTLINE_W * 1.6, sh_ + OUTLINE_W * 1.6),
        radius=_px(SCREEN_R + OUTLINE_W), fill=OUTLINE + (255,))

    screen_mask = Image.new("L", (sw, sh), 0)
    ImageDraw.Draw(screen_mask).rounded_rectangle(
        _rect(scx, scy, sw_, sh_), radius=_px(SCREEN_R), fill=255)
    screen_layer = Image.new("RGBA", (sw, sh), (0, 0, 0, 0))
    screen_layer.paste(_vertical_gradient((sw, sh), CREAM, CREAM_DIM), (0, 0), screen_mask)
    canvas = Image.alpha_composite(canvas, screen_layer)
    draw = ImageDraw.Draw(canvas)

    # ———— 听筒 + 摄像头 ————
    ear_cy = CY - BODY_H / 2.0 + SCREEN_TOP / 2.0
    draw.rounded_rectangle(
        _rect(CX - 20.0, ear_cy, EAR_W + OUTLINE_W, EAR_H + OUTLINE_W),
        radius=_px((EAR_H + OUTLINE_W) / 2), fill=OUTLINE + (255,))
    draw.rounded_rectangle(
        _rect(CX - 20.0, ear_cy, EAR_W, EAR_H),
        radius=_px(EAR_H / 2), fill=ORANGE + (255,))

    cam_cx = CX - 20.0 + EAR_W / 2.0 + 44.0
    draw.ellipse(_rect(cam_cx, ear_cy, (CAM_R + OUTLINE_W * 0.8) * 2,
                       (CAM_R + OUTLINE_W * 0.8) * 2), fill=OUTLINE + (255,))
    draw.ellipse(_rect(cam_cx, ear_cy, CAM_R * 2, CAM_R * 2), fill=BLUE_MID + (255,))

    # ———— Home 键 ————
    btn_cy = CY + BODY_H / 2.0 - SCREEN_BOTTOM / 2.0
    draw.ellipse(_rect(CX, btn_cy, (BTN_R + OUTLINE_W) * 2, (BTN_R + OUTLINE_W) * 2),
                 fill=OUTLINE + (255,))
    draw.ellipse(_rect(CX, btn_cy, BTN_R * 2, BTN_R * 2), fill=CREAM + (255,))

    # **缩放必须在预乘 alpha 下做**，否则透明区的黑会混进半透明边缘，
    # 表现是描边脏边、看着像贴图参数配错了。
    return (canvas.convert("RGBa")
            .resize((W, H), Image.Resampling.LANCZOS)
            .convert("RGBA"))


# ───────────────────────── 自检 ─────────────────────────

def _check(image: Image.Image) -> None:
    px = image.load()
    opaque = [(x, y) for y in range(H) for x in range(W) if px[x, y][3] > 8]
    if not opaque:
        raise AssertionError("整张图是空的。")

    xs = [p[0] for p in opaque]
    ys = [p[1] for p in opaque]
    margin = min(min(xs), min(ys), W - 1 - max(xs), H - 1 - max(ys))
    print("  内容外接框 x[%d,%d] y[%d,%d]，最小边距 %d px" %
          (min(xs), max(xs), min(ys), max(ys), margin))
    if margin < 4:
        raise AssertionError("内容贴到画布边了（最小边距 %d px）。" % margin)

    # 描边颜色取暗部**中位色**，不取单个最暗像素——LANCZOS 的负旁瓣会把
    # #000818 的蓝分量（只有 24）过冲削到 0，极值是错的统计量。
    dark = [px[x, y][:3] for x, y in opaque
            if px[x, y][3] > 240 and sum(px[x, y][:3]) < 140]
    mid = tuple(sorted(c[i] for c in dark)[len(dark) // 2] for i in range(3))
    print("  描边中位色 #%02x%02x%02x（期望 #%02x%02x%02x）" % (*mid, *OUTLINE))
    if mid[2] < mid[0] + 8:
        raise AssertionError("描边中位色 #%02x%02x%02x 漂成黑/灰了。" % mid)

    # **屏幕区域必须是干净的奶油底。** 这是整张图最要紧的一条：
    # UMG 控件要盖在这块上，有任何一层漏进来（描边越界、听筒画偏、Home 键顶进屏幕）
    # 都会在内容底下露出一道，而那种图**单看完全像是设计如此**。
    left, top, right, bottom = (round(v) for v in screen_rect())
    inset = round(SCREEN_R * 0.8)       # 躲开圆角
    bad = []
    for y in range(top + inset, bottom - inset, 7):
        for x in range(left + inset, right - inset, 7):
            r, g, b, a = px[x, y]
            # 奶油是暖色（R > B）且很亮；蓝/橙/描边都过不了这两条
            if a < 250 or b > r - 20 or r < 200:
                bad.append((x, y, (r, g, b, a)))
    if bad:
        raise AssertionError(
            "屏幕区域里有 %d 个采样点不是奶油底，第一个 %s。有图层漏进内容区了。"
            % (len(bad), bad[0]))
    print("  屏幕区域 %dx%d 全是干净奶油底" % (right - left, bottom - top))


def run() -> str:
    os.makedirs(SOURCE_DIR, exist_ok=True)
    print("[手机面板] 自检：")
    image = build()
    _check(image)
    image.save(OUTPUT)
    print("[手机面板] 已写 %s（%d x %d）" % (OUTPUT, W, H))

    left, top, right, bottom = screen_rect()
    print()
    print("—— UMG 摆放用的数字 ——")
    print("  贴图       %d x %d，宽高比 1 : %.3f" % (W, H, H / W))
    print("  屏幕矩形   左 %.0f  上 %.0f  右 %.0f  下 %.0f（贴图像素）"
          % (left, top, right, bottom))
    print("  屏幕归一化 左 %.4f  上 %.4f  宽 %.4f  高 %.4f"
          % (left / W, top / H, (right - left) / W, (bottom - top) / H))
    print()
    print("  用法：面板图放一个 Image，内容容器用**同一个父 Overlay**，")
    print("  锚点按上面那组归一化数字设，这样面板缩放时内容跟着缩，不用手配。")

    return OUTPUT


if __name__ == "__main__":
    run()
