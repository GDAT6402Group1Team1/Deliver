# -*- coding: utf-8 -*-
"""从取件图钉**改画**出送达图钉：抠掉箱子、在同一个奶油圈里画房子。

—— 为什么是"改画"而不是"重画一个" ——

上一版 `generate_compass_icons.py` 是从零用矢量画整枚图钉，结果和原图差很远：
蓝环被奶油圈挤掉只剩顶上一道、尖端拉成一根针、橙色箭头掉到尖上、而且全是平涂，
没有原图的立体阴影。图钉外壳那堆渐变、内阴影、投影，靠参数复刻代价极高、
还永远差一口气。

**而要换的只有圆盘正中那个图标。** 所以直接拿 `T_CompassTaskTarget.png` 当底，
只重绘 `r < INNER_SAFE_R` 的中心区——外圈描边、蓝环、奶油环、尖端、橙色箭头、
投影全是**字面意义上的同一份像素**，天然 1:1。

几何依据（都是从原图实测的，别手改）：
  * 圆盘中心 (281.0, 177.5)，奶油圈外半径约 96
  * 箱子离圆心最远 82 —— 所以 r∈[88,96] 是一圈**干净的奶油环**
  * 重绘半径取 87：大于箱子的 82（擦得干净）、小于 88（不碰那圈参考样本）

奶油底色不是单一色值，而是从那圈环**最小二乘拟合**出的线性渐变再外推到中心
（实测 R=246.5+0.2u−4.1v 等，u=dx/96、v=dy/96），所以补出来的底和周围连得上，
不会出现一块平涂的圆斑。

房子的配色全部取自箱子本身（描边近黑、三档暖色面），不引入新色系。

编辑器外重新生成 PNG：
    python Content/Python/generate_delivery_icon.py
生成完再在编辑器里跑 `import_compass_icons.py` 导入。
"""

from __future__ import annotations

import math
import os

from PIL import Image, ImageDraw, ImageFilter


HERE = os.path.dirname(os.path.abspath(__file__))
SOURCE_DIR = os.path.normpath(os.path.join(HERE, "..", "UI", "Minimap", "Source"))
REFERENCE = os.path.join(SOURCE_DIR, "T_CompassTaskTarget.png")
OUTPUT = os.path.join(SOURCE_DIR, "T_CompassTaskDelivery.png")

# —— 实测几何，别手改 ——
#
# **这组数第一版全是错的**：当时用"射线找奶油边界"量，而射线会被正中间的箱子
# 截断，量出来的中心比真值低了 14 像素、半径也偏大。后果是顶部一圈箱子描边
# 没被擦掉（留下两道淡弧）、房子整体偏右下、圆盘左上空一大块。
# 现在改成打一张分类图直接读：水平中线上奶油在 x 185..375、
# 竖直中线上在 y 70..257，两者的中点和半宽才是下面这组值。
CX, CY = 280.0, 163.5      # 奶油圆盘中心
DISC_R = 94.0              # 奶油圈外半径

# 图形自己的中心。参考图里箱子比圆盘中心低约 6px（箱子中心 y≈169.5），
# 房子跟着放一样的位置，两枚图钉并排看时图形的高低才一致
ICON_CY = 167.5

BOX_SCAN_R = 90.0          # 到这个半径为止找箱子像素（箱子实测最远 88）
ICON_MAX_R = 84.0          # 图形最远**顶点**的半径。箱子连描边实测到 88，
                           # 而描边是沿顶点外扩半个线宽（8/2=4）画的，
                           # 所以顶点要留到 84 才和箱子一样刚好贴住圈内。
                           # 填 88 的话描边会戳出奶油圈几个像素（自检会报）。
                           # **可以超过重绘区**——房子画在没动过的奶油上效果一样，
                           # 重绘只负责擦掉箱子。早先把两者绑在一起，house 被压到
                           # r<=80、只占奶油圈 65%，而箱子是撑满的
CLIP_R = 90.0              # 合成裁剪半径。**要比奶油圈小出一个模糊半径**：
                           # 裁剪掩码自己要羽化 2px，贴着 94 的话会糊到蓝环上

# 奶油渐变（圆盘内所有奶油像素的最小二乘拟合，u=dx/94, v=dy/94）
CREAM_FIT = (
    (248.1, -0.0, -3.2),
    (225.3, 0.2, -8.2),
    (187.5, 0.1, -10.7),
)

# 明暗分三档，和箱子一样：亮顶 / 中面 / 暗面。
# 第一版全用褐色档，整个房子糊成一团——箱子之所以立体，靠的是这三档拉得开
# **是深海军蓝不是黑**（实测 #020c1e，和图钉外圈的 #000c24 同一个色）。
# 用纯黑画出来会比参考图"硬"，并排看一眼就能看出两枚不是一套
OUTLINE = (2, 12, 30)
# 正面墙压暗一档。取箱子最亮那面的话，和奶油底 #f8dbb5 只差十几级，
# 大片正面墙会"化"进背景里——箱子那面之所以立得住，是因为它面积小、
# 四周全是中面和暗面围着
WALL_LIT = (232, 186, 112)
WALL_SHADE = (216, 132, 48)  # 箱子中面 #d88430 —— 侧面墙
ROOF_LIT = (192, 108, 36)    # 箱子暗面 #c06c24 —— 正面山墙
ROOF_SHADE = (160, 86, 28)   # 再暗一档 —— 侧面屋顶
DOOR = (107, 58, 18)
WINDOW = (247, 226, 194)

SS = 4                       # 超采样倍数，边缘才不锯齿
OUTLINE_W = 8.0              # 描边粗细。箱子实测约 8~9px，6.5 显得单薄


def cream_at(dx: float, dy: float):
    """圆盘在 (dx,dy) 处的底色。外推到中心，所以补出来的底和周围连得上。"""
    u, v = dx / DISC_R, dy / DISC_R

    return tuple(
        max(0, min(255, int(round(a + b * u + c * v))))
        for a, b, c in CREAM_FIT
    )


def erase_icon(im: Image.Image) -> None:
    """只把**箱子实际占的像素**换成奶油底，不重绘整个圆盘。

    早先是"整片 r<87 一律重绘"，于是补出来的底和外面真奶油之间留了一道接缝
    （拟合值再准也差几个色阶，圆盘上半能看出两道淡弧）。改成按掩码只补箱子那块，
    其余一个像素都不碰，接缝从源头就不存在。

    掩码判据不是"不是奶油色"——箱子的高光 #fccc90 会被当成奶油漏掉——
    而是"和这个位置的奶油模型差得够远"。
    """
    px = im.load()
    W, H = im.size
    x0, x1 = int(CX - BOX_SCAN_R - 2), int(CX + BOX_SCAN_R + 2)
    y0, y1 = int(CY - BOX_SCAN_R - 2), int(CY + BOX_SCAN_R + 2)

    mask = Image.new("L", im.size, 0)
    md = mask.load()
    for y in range(max(0, y0), min(H, y1 + 1)):
        for x in range(max(0, x0), min(W, x1 + 1)):
            dx, dy = x - CX, y - CY
            if math.hypot(dx, dy) > BOX_SCAN_R:
                continue
            c = px[x, y]
            ref = cream_at(dx, dy)
            # 阈值 12 不是 22：箱子顶角的抗锯齿边和奶油只差十几级，
            # 松阈值会把它漏下来，表现是圆盘上方留两道擦不掉的淡弧。
            # 收到 12 仍然安全——拟合值和真实奶油的偏差实测只有 4 级左右
            if max(abs(c[i] - ref[i]) for i in range(3)) > 12 or c[3] < 250:
                md[x, y] = 255

    # 膨胀几像素盖住抗锯齿边，再羽化，补出来的块才和周围融得上
    mask = mask.filter(ImageFilter.MaxFilter(9)).filter(ImageFilter.GaussianBlur(2.0))

    fill = Image.new("RGBA", im.size, (0, 0, 0, 0))
    fd = fill.load()
    md = mask.load()
    for y in range(max(0, y0 - 6), min(H, y1 + 7)):
        for x in range(max(0, x0 - 6), min(W, x1 + 7)):
            # **补色也要夹在奶油圈内**：掩码经过 MaxFilter(9) 膨胀 + 模糊 2px，
            # 会伸到 r≈96，而奶油圈只到 94——不夹的话会给深蓝环的内缘
            # 蒙上一层淡奶油（实测最大 8 级色差）
            if md[x, y] and math.hypot(x - CX, y - CY) <= DISC_R - 1.0:
                c = cream_at(x - CX, y - CY)
                fd[x, y] = (c[0], c[1], c[2], md[x, y])

    im.alpha_composite(fill)


def shade(color, amount: float):
    """按比例提亮/压暗，用来给每个面做渐变。amount>0 提亮。"""
    if amount >= 0:
        return tuple(int(round(c + (255 - c) * amount)) for c in color)

    return tuple(int(round(c * (1 + amount))) for c in color)


def house_polygons():
    """房子的多边形，返回 (相对圆盘中心的坐标列表, 填充色, 顶部色, 底部色)。

    斜二测：正面朝观众，右侧面向后退，屋顶一个正面山墙 + 一个右侧斜面。
    尺寸对着箱子的外接框（155×163）配，所以两枚图钉里的图形体量一致。
    """
    # **刻意做得扁而宽**。等比缩放是被最远顶点卡住的，而房子的最远顶点是
    # 底下两个角：角半径 = sqrt((w/2)^2+(h/2)^2)。高度压下来，同样的半径下
    # 宽度就能放出去——h=100 时 w 能到 150（接近箱子的 155），h=130 时只剩 134。
    # 第一版 104×... 的比例偏方，于是整个图形被压到只占奶油圈 65%
    fw, depth, ds = 112.0, 44.0, 0.50   # 正面宽、进深、进深的竖直压缩
    hw, hr = 58.0, 46.0                 # 墙高、屋顶举高
    ox, oy = depth, -depth * ds         # 进深方向的偏移向量

    y0 = 0.0
    hx = fw / 2.0
    fl_t, fr_t = (-hx, y0), (hx, y0)
    fl_b, fr_b = (-hx, y0 + hw), (hx, y0 + hw)
    apex = (0.0, y0 - hr)

    def off(p):
        return (p[0] + ox, p[1] + oy)

    parts = [
        # 右侧墙（背光，用暗面）
        ([fr_t, off(fr_t), off(fr_b), fr_b], WALL_SHADE, -0.10, -0.26),
        # 正面墙
        ([fl_t, fr_t, fr_b, fl_b], WALL_LIT, 0.08, -0.16),
        # 右侧屋顶斜面
        ([apex, off(apex), off(fr_t), fr_t], ROOF_SHADE, 0.02, -0.18),
        # 正面山墙
        ([apex, fr_t, fl_t], ROOF_LIT, 0.14, -0.10),
    ]

    # 整体居中：按所有顶点的外接框平移，免得手算偏移
    pts = [p for poly, _, _, _ in parts for p in poly]
    mx = (min(p[0] for p in pts) + max(p[0] for p in pts)) / 2.0
    my = (min(p[1] for p in pts) + max(p[1] for p in pts)) / 2.0
    parts = [([(p[0] - mx, p[1] - my) for p in poly], c, t, b) for poly, c, t, b in parts]

    # **按最远顶点等比缩放，而不是按外接框**。
    # 箱子是六边形，极值点落在正上下和正左右（实测 r=82），四个角上没有料；
    # 房子更接近矩形，同样的外接框下对角线会伸到 r≈109、戳出奶油圈。
    # 用外接框对齐两者只会让房子显得过大并越界，看半径才是对的量。
    pts = [p for poly, _, _, _ in parts for p in poly]
    far = max(math.hypot(p[0], p[1]) for p in pts)
    k = ICON_MAX_R / far

    return [([(p[0] * k, p[1] * k) for p in poly], c, t, b) for poly, c, t, b in parts]


def draw_house(im: Image.Image) -> None:
    """在圆盘中心画房子。超采样 + 逐面竖直渐变 + 柔和投影。"""
    W, H = im.size
    big = (W * SS, H * SS)
    layer = Image.new("RGBA", big, (0, 0, 0, 0))
    draw = ImageDraw.Draw(layer)

    parts = house_polygons()

    def to_px(p):
        return ((CX + p[0]) * SS, (ICON_CY + p[1]) * SS)

    # 投影：把整个轮廓压扁放在房子底下，和箱子那圈软阴影一个意思
    shadow = Image.new("RGBA", big, (0, 0, 0, 0))
    sd = ImageDraw.Draw(shadow)
    for poly, _, _, _ in parts:
        sd.polygon([to_px((p[0], p[1] + 9)) for p in poly], fill=(26, 20, 34, 110))
    shadow = shadow.filter(ImageFilter.GaussianBlur(6 * SS))
    layer = Image.alpha_composite(layer, shadow)
    draw = ImageDraw.Draw(layer)

    # 每个面：先铺底色，再用水平条带做竖直渐变（条带足够细就看不出来）
    for poly, base, top_amt, bot_amt in parts:
        pts = [to_px(p) for p in poly]
        ys = [p[1] for p in pts]
        y_lo, y_hi = min(ys), max(ys)

        face = Image.new("RGBA", big, (0, 0, 0, 0))
        fd = ImageDraw.Draw(face)
        fd.polygon(pts, fill=base + (255,))

        grad = Image.new("RGBA", big, (0, 0, 0, 0))
        gd = ImageDraw.Draw(grad)
        steps = 96
        for i in range(steps):
            t = i / (steps - 1.0)
            y_a = y_lo + (y_hi - y_lo) * i / steps
            y_b = y_lo + (y_hi - y_lo) * (i + 1) / steps + 1
            col = shade(base, top_amt + (bot_amt - top_amt) * t)
            gd.rectangle([0, y_a, big[0], y_b], fill=col + (255,))

        face = Image.composite(grad, Image.new("RGBA", big, (0, 0, 0, 0)), face.split()[3])
        layer = Image.alpha_composite(layer, face)

    draw = ImageDraw.Draw(layer)

    # 门和窗：画在正面墙上。正面墙是 parts[1]
    front = parts[1][0]
    fl_t, fr_t, fr_b, fl_b = front
    def lerp(a, b, t):
        return (a[0] + (b[0] - a[0]) * t, a[1] + (b[1] - a[1]) * t)

    door = [
        lerp(lerp(fl_t, fr_t, 0.40), lerp(fl_b, fr_b, 0.40), 0.30),
        lerp(lerp(fl_t, fr_t, 0.68), lerp(fl_b, fr_b, 0.68), 0.30),
        lerp(fl_b, fr_b, 0.68),
        lerp(fl_b, fr_b, 0.40),
    ]
    draw.polygon([to_px(p) for p in door], fill=DOOR + (255,),
                 outline=OUTLINE + (255,), width=int(5.0 * SS))

    win = [
        lerp(lerp(fl_t, fr_t, 0.10), lerp(fl_b, fr_b, 0.10), 0.20),
        lerp(lerp(fl_t, fr_t, 0.30), lerp(fl_b, fr_b, 0.30), 0.20),
        lerp(lerp(fl_t, fr_t, 0.30), lerp(fl_b, fr_b, 0.30), 0.52),
        lerp(lerp(fl_t, fr_t, 0.10), lerp(fl_b, fr_b, 0.10), 0.52),
    ]
    draw.polygon([to_px(p) for p in win], fill=WINDOW + (255,),
                 outline=OUTLINE + (255,), width=int(5.0 * SS))

    # 描边最后画，压在所有面之上，和箱子一样是粗描边
    for poly, _, _, _ in parts:
        draw.line([to_px(p) for p in poly] + [to_px(poly[0])],
                  fill=OUTLINE + (255,), width=int(OUTLINE_W * SS), joint="curve")

    small = layer.resize(im.size, Image.LANCZOS)

    # **裁进圆盘再合成**。投影是高斯模糊出来的，会往外糊一大圈；
    # 不裁的话奶油环、蓝环甚至尖端都会被蒙上一层脏，而那正是要原样保留的部分。
    # 边缘羽化 2px，免得裁出一道硬边
    mask = Image.new("L", im.size, 0)
    ImageDraw.Draw(mask).ellipse(
        [CX - CLIP_R, CY - CLIP_R, CX + CLIP_R, CY + CLIP_R], fill=255)
    mask = mask.filter(ImageFilter.GaussianBlur(2))
    clipped = small.copy()
    alpha = clipped.split()[3].point(lambda v: v)
    clipped.putalpha(Image.eval(Image.merge("L", (alpha,)), lambda v: v))
    clipped.putalpha(Image.composite(alpha, Image.new("L", im.size, 0), mask))
    im.alpha_composite(clipped)


def run():
    if not os.path.isfile(REFERENCE):
        raise SystemExit("找不到参考图 %s" % REFERENCE)

    im = Image.open(REFERENCE).convert("RGBA")
    erase_icon(im)
    draw_house(im)
    im.save(OUTPUT)
    print("写出: %s (%d 字节)" % (OUTPUT, os.path.getsize(OUTPUT)))

    # 自检：重绘区以外必须和参考图逐像素一致，否则就是碰到了不该碰的地方
    ref = Image.open(REFERENCE).convert("RGBA")
    a, b = ref.load(), im.load()
    bad = 0
    worst = 0
    for y in range(im.size[1]):
        for x in range(im.size[0]):
            # 判据是"奶油圈以外"：圈内本来就要改，圈外（蓝环/尖端/投影）一个像素都不该动
            if math.hypot(x - CX, y - CY) <= DISC_R:
                continue
            d = max(abs(a[x, y][i] - b[x, y][i]) for i in range(4))
            if d:
                bad += 1
                worst = max(worst, d)

    # **要报幅度，不能只报个数。** 裁剪掩码自己带 2px 羽化，边界上必然有几十个
    # 像素差 1~2 级，那是看不出来的；只报个数的话每次都像出了问题，
    # 而真正的越界（投影糊到蓝环上）和它在计数上分不开
    print("自检：奶油圈外被改动 %d 像素，最大色差 %d 级 %s"
          % (bad, worst, "(羽化边，看不出来)" if worst <= 4 else "<< 偏大，检查 CLIP_R"))


if __name__ == "__main__":
    run()
