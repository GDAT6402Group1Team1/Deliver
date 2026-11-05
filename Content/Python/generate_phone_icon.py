# -*- coding: utf-8 -*-
"""离线画一枚手机图标，风格和罗盘图钉/人物标记同一套。

    python Content/Python/generate_phone_icon.py

产物：`Content/UI/Phone/Source/T_IconPhone.png`（512x512，背景全透明）
导入走 `Content/Python/import_phone_icon.py`。

—— 调色板是实采的，别手改色号 ——

CLAUDE.md 立过这条，这里照办：所有颜色都是从 `T_CompassTaskTarget.png`
逐像素统计出来的主色，不是我挑的好看的颜色。

  * 描边 `#000818` —— **深海军蓝不是黑**。用纯黑画出来和图钉并排一看就不是一套，
    而单看那一枚完全正常，所以这种错只会在凑到一起时才暴露。
  * 蓝 `#003890` ~ `#0050b8` —— 图钉蓝环本身就是个渐变，这里当明暗两档用。
  * 奶油 `#f8e0bc` —— 屏幕。
  * 橙 `#f87800` —— 图钉箭头那个橙，这里只用在听筒和底部按钮上点一下，
    **面积要小**：橙是整套图标里最抢眼的颜色，铺大了手机会变成主角。

—— 为什么是 512 不是别的 ——

和图钉同画布尺寸。HUD 上这两种图标会同时出现，源图一样大，缩放比例才一致；
源图大小不一时需要在 UMG 里逐个调 size box 去配，那是个永远对不准的活。

—— 为什么要超采样 ——

`SS = 4` 画在 2048 上再缩回 512。PIL 的多边形/椭圆填充没有抗锯齿，
直接画 512 的斜边和圆角会有锯齿，而**锯齿在图标缩小显示时表现为描边发毛**，
看着像贴图参数没配对（mip / 压缩），实际是源图就毛的——会往错的方向查。
"""

from __future__ import annotations

import math
import os

from PIL import Image, ImageDraw, ImageFilter


HERE = os.path.dirname(os.path.abspath(__file__))
SOURCE_DIR = os.path.normpath(os.path.join(HERE, "..", "UI", "Phone", "Source"))
OUTPUT = os.path.join(SOURCE_DIR, "T_IconPhone.png")
# 放大版：同一份画法，只是把手机在画布里铺满。
OUTPUT_BIG = os.path.join(SOURCE_DIR, "T_IconPhoneBig.png")

# 放大版四周留的空白。**按投影能扩散多远算，不能拍一个比例。**
# 高斯模糊的可见尾巴大约到 2 倍半径，投影本身还整体偏移过；留白小于这个数
# 就会把投影硬切一刀，而且自检的边距那条会直接报 0——因为裁剪按 alpha>64
# 取外接框、边距却数 alpha>8，两个阈值不一致时虚边正好落在留白里。
# 这个值在模块底部按 SHADOW_* 自动算，改投影参数时不用手动跟着改。

SIZE = 512
SS = 4                      # 超采样倍数，见模块注释
S = SIZE * SS

# —— 实采色 ——
OUTLINE = (0, 8, 24)        # #000818
BLUE_LIT = (0, 80, 184)     # #0050b8
BLUE_MID = (0, 64, 164)     # #0040a4
BLUE_SHADE = (0, 56, 144)   # #003890
CREAM = (248, 224, 188)     # #f8e0bc
CREAM_DIM = (240, 216, 176) # #f0d8b0，屏幕下半压一档
ORANGE = (248, 120, 0)      # #f87800

# —— 几何（都是 512 画布下的像素）——
#
# 机身比例刻意**比真手机胖**（1 : 1.55，真机约 1 : 2）。细长的矩形缩到 HUD 上
# 那么大时会糊成一根竖条、认不出是手机；卡通图标靠的是"一眼认出轮廓"。
BODY_W = 246.0
BODY_H = 382.0
BODY_R = 46.0               # 机身圆角
OUTLINE_W = 9.0             # 描边粗细。图钉上实测 8~9px，取 9 配这个尺寸
CX, CY = 256.0, 252.0       # 机身中心。CY 比 256 略高，给底部投影留地方

SCREEN_INSET_X = 26.0       # 屏幕离机身左右边
SCREEN_TOP = 52.0           # 屏幕离机身上边（上面要塞听筒）
SCREEN_BOTTOM = 76.0        # 屏幕离机身下边。**必须装得下按钮连描边**
                            # （2*(BTN_R+OUTLINE_W)），不够的话按钮会顶进屏幕里。
                            # 下面 _check 有这条断言。
SCREEN_R = 22.0

BTN_R = 20.0                # 底部按钮半径

SHADOW_OFFSET = (10.0, 16.0)
SHADOW_BLUR = 14.0
BIG_MARGIN = 2.0 * SHADOW_BLUR


def _px(v: float) -> float:
    """512 画布坐标 -> 超采样画布坐标。"""
    return v * SS


def _rect(cx: float, cy: float, w: float, h: float):
    return [_px(cx - w / 2), _px(cy - h / 2), _px(cx + w / 2), _px(cy + h / 2)]


def _vertical_gradient(size, top, bottom):
    """竖向渐变图层。

    机身用渐变而不是平涂：图钉那几枚的立体感全来自渐变和内外阴影，
    平涂的新图标和它们并排时会"扁一档"——这点在单看时也察觉不到。
    """
    grad = Image.new("RGB", (1, size[1]))
    load = grad.load()
    for y in range(size[1]):
        t = y / max(size[1] - 1, 1)
        load[0, y] = tuple(round(a + (b - a) * t) for a, b in zip(top, bottom))

    return grad.resize(size, Image.Resampling.BILINEAR)


def build(fill: bool = False, out_size: int = SIZE) -> Image.Image:
    canvas = Image.new("RGBA", (S, S), (0, 0, 0, 0))

    # ———— 1. 投影 ————
    #
    # 先画、单独一层再模糊。和机身画在同一层上模糊会把机身边缘也糊掉。
    #
    # **只能模糊 alpha 蒙版，不能直接模糊一张 RGBA 图。**
    # RGBA 是非预乘的，透明区域的 RGB 是 (0,0,0)；在 RGBA 上做高斯模糊会把
    # 这些黑色混进投影的颜色里，把海军蓝一路拖成纯黑。第一版就是这么写的，
    # 自检里「最暗像素蓝分量要高于红分量」那条当场抓到 `#000000`——
    # **而肉眼看那张图完全正常**（投影本来就该是暗的，"暗蓝"和"纯黑"单看
    # 分辨不出，只有和图钉并排才露馅）。
    # 画成 L 蒙版、模糊蒙版、再用它把纯色贴上去，RGB 全程不参与运算。
    shadow_mask = Image.new("L", (S, S), 0)
    ImageDraw.Draw(shadow_mask).rounded_rectangle(
        _rect(CX + SHADOW_OFFSET[0], CY + SHADOW_OFFSET[1],
              BODY_W + OUTLINE_W, BODY_H + OUTLINE_W),
        radius=_px(BODY_R), fill=110)
    shadow_mask = shadow_mask.filter(ImageFilter.GaussianBlur(_px(SHADOW_BLUR)))
    shadow = Image.new("RGBA", (S, S), OUTLINE + (0,))
    shadow.putalpha(shadow_mask)
    canvas = Image.alpha_composite(canvas, shadow)

    # ———— 2. 机身：先描边，再在里面填渐变 ————
    #
    # 描边不用 `outline=` 画线，而是**画一个大一圈的实心圆角矩形当底**。
    # PIL 的 outline 宽度是朝内外各长一半、且圆角处会变细，
    # 粗描边用它画出来四个角明显比直边细，看着像"圆角画崩了"。
    body = Image.new("RGBA", (S, S), (0, 0, 0, 0))
    bd = ImageDraw.Draw(body)
    bd.rounded_rectangle(
        _rect(CX, CY, BODY_W + OUTLINE_W * 2, BODY_H + OUTLINE_W * 2),
        radius=_px(BODY_R + OUTLINE_W), fill=OUTLINE + (255,))

    # 机身填色：渐变裁进圆角矩形
    fill_mask = Image.new("L", (S, S), 0)
    ImageDraw.Draw(fill_mask).rounded_rectangle(
        _rect(CX, CY, BODY_W, BODY_H), radius=_px(BODY_R), fill=255)
    grad = _vertical_gradient((S, S), BLUE_LIT, BLUE_SHADE)
    body.paste(grad, (0, 0), fill_mask)
    canvas = Image.alpha_composite(canvas, body)

    draw = ImageDraw.Draw(canvas)

    # ———— 3. 屏幕 ————
    screen_cy = CY - BODY_H / 2 + SCREEN_TOP \
        + (BODY_H - SCREEN_TOP - SCREEN_BOTTOM) / 2
    screen_w = BODY_W - SCREEN_INSET_X * 2
    screen_h = BODY_H - SCREEN_TOP - SCREEN_BOTTOM

    # 屏幕也先铺一层描边底，同上
    draw.rounded_rectangle(
        _rect(CX, screen_cy, screen_w + OUTLINE_W * 1.4, screen_h + OUTLINE_W * 1.4),
        radius=_px(SCREEN_R + OUTLINE_W), fill=OUTLINE + (255,))

    screen_layer = Image.new("RGBA", (S, S), (0, 0, 0, 0))
    screen_mask = Image.new("L", (S, S), 0)
    ImageDraw.Draw(screen_mask).rounded_rectangle(
        _rect(CX, screen_cy, screen_w, screen_h), radius=_px(SCREEN_R), fill=255)
    screen_layer.paste(_vertical_gradient((S, S), CREAM, CREAM_DIM), (0, 0), screen_mask)
    canvas = Image.alpha_composite(canvas, screen_layer)
    draw = ImageDraw.Draw(canvas)

    # ———— 4. 屏幕高光：左上到右下的一道斜切 ————
    #
    # 只占屏幕左上角一块。整屏盖一层半透明白会把奶油冲淡成灰白，
    # 和图钉的奶油圈对不上色。
    gloss = Image.new("RGBA", (S, S), (0, 0, 0, 0))
    gx0 = CX - screen_w / 2
    gy0 = screen_cy - screen_h / 2
    ImageDraw.Draw(gloss).polygon(
        [(_px(gx0), _px(gy0)),
         (_px(gx0 + screen_w * 0.62), _px(gy0)),
         (_px(gx0), _px(gy0 + screen_h * 0.62))],
        fill=(255, 255, 255, 56))
    gloss.putalpha(Image.composite(
        gloss.getchannel("A"), Image.new("L", (S, S), 0), screen_mask))
    canvas = Image.alpha_composite(canvas, gloss)
    draw = ImageDraw.Draw(canvas)

    # ———— 5. 听筒（机身顶部那道横槽）————
    ear_w, ear_h = 78.0, 15.0
    ear_cy = CY - BODY_H / 2 + SCREEN_TOP / 2
    draw.rounded_rectangle(
        _rect(CX, ear_cy, ear_w + OUTLINE_W, ear_h + OUTLINE_W),
        radius=_px((ear_h + OUTLINE_W) / 2), fill=OUTLINE + (255,))
    draw.rounded_rectangle(
        _rect(CX, ear_cy, ear_w, ear_h),
        radius=_px(ear_h / 2), fill=ORANGE + (255,))

    # ———— 6. 底部按钮 ————
    btn_cy = CY + BODY_H / 2 - SCREEN_BOTTOM / 2
    draw.ellipse(_rect(CX, btn_cy, (BTN_R + OUTLINE_W) * 2, (BTN_R + OUTLINE_W) * 2),
                 fill=OUTLINE + (255,))
    draw.ellipse(_rect(CX, btn_cy, BTN_R * 2, BTN_R * 2), fill=CREAM + (255,))

    if fill:
        canvas = _crop_to_content(canvas)

    # **缩放必须在预乘 alpha 下做。** 非预乘 RGBA 缩放时，透明区域的 RGB 是
    # (0,0,0)，重采样会把这些黑混进边缘的半透明像素——表现正是 CLAUDE.md 里
    # 记过的那种「描边脏边」，而且它看着像贴图参数没配对（压缩/mip），
    # 会往错的方向查。`RGBa`（小写 a）是 PIL 的预乘格式，转回 `RGBA` 时还原。
    return (canvas.convert("RGBa")
            .resize((out_size, out_size), Image.Resampling.LANCZOS)
            .convert("RGBA"))


def _crop_to_content(canvas: Image.Image) -> Image.Image:
    """把超采样画布裁到内容外接的**正方形**，放大版靠这一步铺满画布。

    **在超采样尺寸下裁、裁完再缩到 512**，不是把已经做好的 512 图放大——
    后者是在已经丢过一次信息的像素上再插值，描边会软掉一圈，
    而那种糊看着很像贴图的 mip/压缩设错了，会往错的方向查。

    裁成正方形而不是贴着内容的长方形：手机是竖长的，按长方形裁会把宽高比
    写进贴图本身，之后在 UMG 里任何一个方形槽位里都会被拉扁。
    """
    # **按阈值化的 alpha 取外接框，不能直接 getbbox()。**
    # 投影是高斯模糊出来的，外面还有一圈 alpha 只有个位数的虚边；
    # `getbbox()` 把 alpha>0 全算成内容，于是"铺满画布"铺的是那圈看不见的虚边，
    # 手机本身几乎没变大（实测外接框只从 295x433 变到 307x452）。
    # 投影尾巴被裁掉一点是正常的，它本来就是软的。
    box = canvas.getchannel("A").point(lambda a: 255 if a > 64 else 0).getbbox()
    if not box:
        raise AssertionError("画布是空的，没东西可裁。")

    x0, y0, x1, y1 = box
    cx, cy = (x0 + x1) / 2.0, (y0 + y1) / 2.0
    half = max(x1 - x0, y1 - y0) / 2.0 + _px(BIG_MARGIN)

    # crop 超出画布的部分 PIL 会补透明，正是想要的，不用自己判边界。
    return canvas.crop((round(cx - half), round(cy - half),
                        round(cx + half), round(cy + half)))


# ───────────────────────── 自检 ─────────────────────────

def _check(image: Image.Image, coverage_range=(18.0, 55.0)) -> None:
    """五项自检。每一项对应一种"报告里看不出来"的坏法。

    `coverage_range` 按版本给：放大版把手机铺满画布，占比天然高得多，
    沿用原版区间会误报。**但区间不能干脆取消**——它挡的是
    "坐标算错但仍然画出了东西"，那种图只有打开才看得出不对。
    """
    px = image.load()
    side = image.size[0]
    opaque = []
    for y in range(side):
        for x in range(side):
            if px[x, y][3] > 8:
                opaque.append((x, y))

    if not opaque:
        raise AssertionError("整张图是空的。")

    xs = [p[0] for p in opaque]
    ys = [p[1] for p in opaque]
    margin = min(min(xs), min(ys), side - 1 - max(xs), side - 1 - max(ys))
    coverage = 100.0 * len(opaque) / (side * side)

    print("  不透明像素占比 %.1f%%，内容外接框 x[%d,%d] y[%d,%d]，最小边距 %d px"
          % (coverage, min(xs), max(xs), min(ys), max(ys), margin))

    # ① 四边必须留透明边距。贴满边缘的图标在 UMG 里拉伸/缩放时边上会被切掉
    #    或和相邻控件糊在一起，而源图单看是"刚好填满"、很像是故意的。
    if margin < 6:
        raise AssertionError("内容贴到画布边了（最小边距 %d px），至少留 6 px。" % margin)

    # ② 占比区间。太低 = 画歪了或只画出一小块；太高 = 填满整张画布。
    #    这一项挡的是"坐标算错但仍然画出了东西"——那种图打开才看得出不对。
    if not coverage_range[0] <= coverage <= coverage_range[1]:
        raise AssertionError("不透明占比 %.1f%% 不在 %.0f~%.0f%% 区间，几何大概率算错了。"
                             % (coverage, *coverage_range))

    # ③ 描边必须是实采的那个海军蓝，不能漂成纯黑。
    #    纯黑单看完全正常，只有和图钉并排时才露馅，所以必须机器查。
    #
    #    **判据取暗部像素的中位色，不能取单个最暗像素。** 第一版取了极值，
    #    结果被 LANCZOS 的负旁瓣过冲骗了：`#000818` 蓝分量只有 24，过冲一下
    #    就削到 0，于是 10 万个不透明像素里 4 个纯黑就让自检整个失败。
    #    「描边用没用对颜色」问的是整条描边，极值是错的统计量。
    dark = [px[x, y][:3] for x, y in opaque
            if px[x, y][3] > 240 and sum(px[x, y][:3]) < 140]
    if not dark:
        raise AssertionError("图上找不到描边暗部，描边层大概率没画上。")
    mid = tuple(sorted(c[i] for c in dark)[len(dark) // 2] for i in range(3))
    print("  描边中位色 #%02x%02x%02x（期望接近 #%02x%02x%02x，共 %d 个暗部像素）"
          % (*mid, *OUTLINE, len(dark)))
    if mid[2] < mid[0] + 8:
        raise AssertionError(
            "描边中位色 #%02x%02x%02x 蓝分量没有明显高于红分量，漂成黑/灰了。" % mid)

    # ④ 底部按钮必须整个待在屏幕下方那条带子里，不能顶进屏幕。
    #    这是个**纯几何**约束，改任何一个尺寸常数都可能破坏它，而破坏之后
    #    图照样画得出来、自检其他几条也全过——只有打开图才看得见按钮压着屏幕。
    if 2.0 * (BTN_R + OUTLINE_W) > SCREEN_BOTTOM:
        raise AssertionError(
            "按钮连描边 %.0f px 高，而屏幕下方只留了 SCREEN_BOTTOM=%.0f px，会顶进屏幕。"
            % (2.0 * (BTN_R + OUTLINE_W), SCREEN_BOTTOM))

    # ⑤ 三种主色都要真的出现在图上。少了任何一种说明某一层被后面的层整个盖掉了
    #    （图层顺序写错时最常见），而那种图"看着也像个手机"，很容易放过。
    def _present(target, tol=36):
        for x, y in opaque[::7]:
            r, g, b, _ = px[x, y]
            if abs(r - target[0]) <= tol and abs(g - target[1]) <= tol \
                    and abs(b - target[2]) <= tol:
                return True

        return False

    for name, color in (("机身蓝", BLUE_MID), ("屏幕奶油", CREAM), ("听筒橙", ORANGE)):
        if not _present(color):
            raise AssertionError("图上找不到%s #%02x%02x%02x，这一层被盖掉了。"
                                 % (name, *color))
    print("  机身蓝 / 屏幕奶油 / 听筒橙 三层都在")


def run() -> list:
    os.makedirs(SOURCE_DIR, exist_ok=True)
    written = []
    # 放大版 = **铺满画布 + 1024 分辨率**，两件事一起做才有意义：
    # 手机是竖长的，在正方形画布里本来就占了 78% 的高度，光"铺满"最多再大两成；
    # 真正让它在大尺寸下还清楚的是分辨率。
    for label, fill, path, out, cov in (
            ("标准版 512", False, OUTPUT, SIZE, (18.0, 55.0)),
            ("放大版 1024", True, OUTPUT_BIG, SIZE * 2, (30.0, 88.0))):
        print("[手机图标] %s 自检：" % label)
        image = build(fill=fill, out_size=out)
        _check(image, coverage_range=cov)
        image.save(path)
        print("[手机图标] 已写 %s" % path)
        written.append(path)

    return written


if __name__ == "__main__":
    run()
