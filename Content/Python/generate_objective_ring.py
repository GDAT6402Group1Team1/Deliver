# -*- coding: utf-8 -*-
"""画任务目的地的地面圈贴图，并在编辑器里建好贴花材质。

离线只生成 PNG：
    python Content/Python/generate_objective_ring.py

在编辑器里（Cmd 模式）生成 + 导入 + 建材质：
    py generate_objective_ring.py

产物：
    Content/UI/Task/Source/T_ObjectiveRing.png   （512x512，透明底）
    /Game/UI/Task/T_ObjectiveRing                 （贴图）
    /Game/UI/Task/M_ObjectiveRing                 （贴花材质，C++ 默认就指它）

—— 为什么是贴花不是画线 ——

圈只在当前任务目的地显示，所以它是**玩家指引**不是调试辅助，
`DrawDebug` 在 Shipping 里是空的、打包版等于没做。贴花还顺带免费解决了
贴合地形/台阶/斜坡，不用每帧打几十条射线。

—— 图案为什么长这样 ——

  * **主体是一圈虚线段**，不是实心圆环。实心环在复杂地面上会糊成一条带子，
    分段之后即使压在路缘、花坛、草地上也认得出是"一个圈"。
  * 内侧另有一道**很淡的实线**，给出确切边界——虚线本身是模糊的，
    而"到底走到哪儿才算进范围"需要一个准确的边。
  * 中心到内圈之间**留空**。填充会盖住地面细节，而且玩家要走进去站着，
    脚下一片颜色很碍眼。
  * 外侧一圈**向外衰减的柔光**，让圈不像是贴上去的硬边贴纸。

颜色不画死在贴图里：贴图只出**亮度**（RGB 全白、信息在 Alpha 上），
实际颜色由材质参数 `RingColor` 给，C++ 按"进没进范围"在橙/绿之间插值。
把颜色烤进贴图的话就要两张图、还要在 C++ 里切贴图——那是第二处判断。
"""

from __future__ import annotations

import math
import os

from PIL import Image, ImageDraw, ImageFilter


HERE = os.path.dirname(os.path.abspath(__file__))
SOURCE_DIR = os.path.normpath(os.path.join(HERE, "..", "UI", "Task", "Source"))
OUTPUT = os.path.join(SOURCE_DIR, "T_ObjectiveRing.png")

DEST_DIR = "/Game/UI/Task"
TEXTURE_NAME = "T_ObjectiveRing"
MATERIAL_NAME = "M_ObjectiveRing"

SIZE = 512
SS = 4                      # 超采样，否则细圆弧边缘全是锯齿

# 半径都按"占贴图半宽的比例"写，改 SIZE 不用跟着改。
#
# **虚线外缘必须贴到贴图边（≈1.0）。** 贴花把这张图铺在 [-半径, +半径] 上，
# 所以"贴图半宽"就是真实交互半径；虚线往里缩多少，圈就比真实范围小多少。
# 第一版为了给外发光留地方把外缘缩到 0.94，那就得在 C++ 里再乘一个补偿系数
# 才能对上——同一个数写在两处，迟早改一处忘一处。
# 现在**发光只往内扩**，外缘留 0.99（给重采样留一点余量，免得被方形边裁出硬角）。
R_OUTER = 0.99              # 虚线段外缘 = 真实交互半径
R_INNER = 0.84              # 虚线段内缘
R_HAIRLINE = 0.79           # 内侧那道细实线
HAIRLINE_W = 0.010
DASH_COUNT = 32
DASH_FILL = 0.62            # 每段占一个周期的比例，剩下的是空隙
GLOW_INNER = 0.14           # 柔光从虚线内缘再往内渗多少
GLOW_ALPHA = 0.20           # 第一版给 0.33 且双向扩散，糊成一圈白雾
HAIRLINE_ALPHA = 0.55

def build() -> Image.Image:
    s = SIZE * SS
    c = s / 2.0
    half = s / 2.0

    # 只画 Alpha。RGB 全留白，颜色交给材质参数。
    alpha = Image.new("L", (s, s), 0)
    draw = ImageDraw.Draw(alpha)

    def box(radius_frac):
        r = half * radius_frac
        return [c - r, c - r, c + r, c + r]

    # —— 1. 向内的柔光：让圈不像硬边贴纸，但不往外溢 ——
    #
    # 只在虚线内缘往里铺一条窄带再模糊。往外扩的话会被贴图的方形边界裁出硬角，
    # 而且那一圈雾会把圈外的地面整个蒙住。
    glow = Image.new("L", (s, s), 0)
    gd = ImageDraw.Draw(glow)
    gd.ellipse(box(R_OUTER), fill=int(255 * GLOW_ALPHA))
    gd.ellipse(box(R_INNER - GLOW_INNER), fill=0)
    glow = glow.filter(ImageFilter.GaussianBlur(half * GLOW_INNER * 0.4))
    alpha = glow            # 柔光作底，后面的虚线和细线直接画在它上面

    draw = ImageDraw.Draw(alpha)

    # —— 2. 主体虚线段 ——
    #
    # 用 `pieslice` 画扇形、再把内侧挖掉，比逐段算圆弧端点简单且不会在接缝处开裂。
    period = 360.0 / DASH_COUNT
    for i in range(DASH_COUNT):
        start = i * period
        draw.pieslice(box(R_OUTER), start, start + period * DASH_FILL, fill=255)
    draw.ellipse(box(R_INNER), fill=0)

    # —— 3. 内侧细实线：给出确切边界 ——
    draw.ellipse(box(R_HAIRLINE + HAIRLINE_W), fill=int(255 * HAIRLINE_ALPHA))
    draw.ellipse(box(R_HAIRLINE - HAIRLINE_W), fill=0)

    rgb = Image.new("RGB", (s, s), (255, 255, 255))
    out = Image.merge("RGBA", (*rgb.split(), alpha))

    # **缩放必须在预乘 alpha 下做**，否则透明区的黑会混进半透明边缘，
    # 表现是圈的边缘发灰、看着像贴图压缩坏了。
    return (out.convert("RGBa")
            .resize((SIZE, SIZE), Image.Resampling.LANCZOS)
            .convert("RGBA"))


def _check(image: Image.Image) -> None:
    px = image.load()
    c = SIZE // 2

    # ① 中心必须是空的。填满的圈会盖住玩家脚下的地面。
    if px[c, c][3] > 8:
        raise AssertionError("圈心不透明（alpha=%d），中间被填上了。" % px[c, c][3])

    # ② 沿一条半径扫过去，必须出现"空 → 实 → 空"。
    #    只检查"有不透明像素"是不够的——画成实心圆盘同样满足，而那完全不是圈。
    row = [px[c + r, c][3] for r in range(c - 2)]
    peak = max(row)
    if peak < 200:
        raise AssertionError("半径方向上最高 alpha 只有 %d，圈没画出来。" % peak)
    first_solid = next(i for i, v in enumerate(row) if v > 200)
    if first_solid < c * 0.5:
        raise AssertionError(
            "半径 %d/%d 处就已经不透明了，实心区离圆心太近、这是个圆盘不是圈。"
            % (first_solid, c))

    # ③ 虚线必须真的有空隙。沿中间半径绕一圈数 alpha 的起落，
    #    段数不对说明 pieslice 的角度算错了——而那种图"看着也像个圈"。
    r = half_r = (R_OUTER + R_INNER) / 2.0 * (SIZE / 2.0)
    samples = []
    for deg in range(0, 360):
        a = math.radians(deg)
        x = int(c + r * math.cos(a))
        y = int(c + r * math.sin(a))
        if 0 <= x < SIZE and 0 <= y < SIZE:
            samples.append(px[x, y][3] > 128)
    runs = sum(1 for i in range(len(samples)) if samples[i] and not samples[i - 1])
    print("  圈心透明 / 半径剖面峰值 %d / 实心起点 %d%% / 数到 %d 段虚线（期望 %d）"
          % (peak, 100 * first_solid // c, runs, DASH_COUNT))
    if abs(runs - DASH_COUNT) > 2:
        raise AssertionError("数到 %d 段虚线，期望 %d 段。" % (runs, DASH_COUNT))


def _make_material():
    """建贴花材质。没有就建，有就只更新贴图引用，不覆盖手调过的参数。"""
    import unreal

    tex_path = "%s/%s" % (DEST_DIR, TEXTURE_NAME)
    texture = unreal.load_asset(tex_path)
    if not texture:
        unreal.log_error("[目标圈] 载不到贴图 %s，材质没建。" % tex_path)
        return

    mat_path = "%s/%s" % (DEST_DIR, MATERIAL_NAME)
    material = unreal.load_asset(mat_path)
    if material:
        unreal.log("[目标圈] 材质已存在，跳过：%s" % mat_path)
        return

    tools = unreal.AssetToolsHelpers.get_asset_tools()
    material = tools.create_asset(MATERIAL_NAME, DEST_DIR,
                                  unreal.Material, unreal.MaterialFactoryNew())
    if not material:
        unreal.log_error("[目标圈] 建材质失败。")
        return

    # **必须是 Deferred Decal 域**，否则它是个普通表面材质、贴花组件用了不显示，
    # 而那和"材质没填"长得一模一样。
    material.set_editor_property("material_domain", unreal.MaterialDomain.MD_DEFERRED_DECAL)
    material.set_editor_property("blend_mode", unreal.BlendMode.BLEND_TRANSLUCENT)

    lib = unreal.MaterialEditingLibrary
    sample = lib.create_material_expression(material, unreal.MaterialExpressionTextureSample, -400, 0)
    sample.set_editor_property("texture", texture)

    color = lib.create_material_expression(material, unreal.MaterialExpressionVectorParameter, -400, 250)
    color.set_editor_property("parameter_name", "RingColor")
    color.set_editor_property("default_value", unreal.LinearColor(1.0, 0.55, 0.08, 1.0))

    opacity = lib.create_material_expression(material, unreal.MaterialExpressionScalarParameter, -400, 400)
    opacity.set_editor_property("parameter_name", "RingOpacity")
    opacity.set_editor_property("default_value", 1.0)

    mul_a = lib.create_material_expression(material, unreal.MaterialExpressionMultiply, -150, 300)
    lib.connect_material_expressions(sample, "A", mul_a, "A")
    lib.connect_material_expressions(opacity, "", mul_a, "B")

    # 颜色接 Emissive 而不是 BaseColor：贴花的 BaseColor 会被场景光照压暗，
    # 圈是个 UI 指引、应该在阴影里也一样清楚。
    lib.connect_material_property(color, "", unreal.MaterialProperty.MP_EMISSIVE_COLOR)
    lib.connect_material_property(mul_a, "", unreal.MaterialProperty.MP_OPACITY)

    lib.recompile_material(material)
    unreal.EditorAssetLibrary.save_loaded_asset(material, only_if_is_dirty=False)
    unreal.log("[目标圈] 已建材质 %s（域=DeferredDecal，参数 RingColor / RingOpacity）" % mat_path)


def _import(path: str):
    try:
        import unreal
    except ImportError:
        print("[目标圈] !! 不在编辑器里，只生成了 PNG，**没有导入、也没建材质**。")
        print("[目标圈]    在编辑器控制台跑：py generate_objective_ring.py")
        return

    task = unreal.AssetImportTask()
    task.set_editor_property("filename", path)
    task.set_editor_property("destination_path", DEST_DIR)
    task.set_editor_property("destination_name", TEXTURE_NAME)
    task.set_editor_property("automated", True)
    task.set_editor_property("replace_existing", True)
    task.set_editor_property("save", True)
    # 必须显式指定 factory，UE 5.8 默认走 Interchange、不读这些设置。
    task.factory = unreal.TextureFactory()
    unreal.AssetToolsHelpers.get_asset_tools().import_asset_tasks([task])

    texture = unreal.load_asset("%s/%s" % (DEST_DIR, TEXTURE_NAME))
    if not texture:
        unreal.log_error("[目标圈] 导入后载不到贴图；空产出多半是 factory 没生效。")
        return

    # **这张不是 UI 贴图，是世界里的贴花**，所以不设 TEXTUREGROUP_UI：
    # 那个组会关掉 mip，而贴花在远处看会因此闪烁（aliasing）。
    texture.set_editor_property("srgb", False)   # 只存亮度/遮罩，不是颜色
    texture.set_editor_property(
        "compression_settings", unreal.TextureCompressionSettings.TC_EDITOR_ICON)
    unreal.EditorAssetLibrary.save_loaded_asset(texture, only_if_is_dirty=False)
    unreal.log("[目标圈] 已导入 %s/%s" % (DEST_DIR, TEXTURE_NAME))

    _make_material()


def run() -> str:
    os.makedirs(SOURCE_DIR, exist_ok=True)
    image = build()
    print("[目标圈] 自检：")
    _check(image)
    image.save(OUTPUT)
    print("[目标圈] 已写 %s" % OUTPUT)
    _import(OUTPUT)
    return OUTPUT


if __name__ == "__main__":
    run()
