# -*- coding: utf-8 -*-
"""为交通车复制出入场专用材质；正常行驶仍使用原材质"""
import unreal

DEST = '/Game/Traffic/Fade/'
BASES = (
    '/Game/materials/plastic_white',
    '/Game/materials/glass_window',
    '/Game/Vehicle/Motorbike/Parts/材质_021',
    '/Game/Vehicle/Motorbike/材质_003',
    '/InterchangeAssets/gltf/Substrate/M_GLTF',
)
INSTANCES = (
    '/Game/materials/plastic_bluedark',
    '/Game/materials/plastic_dark',
    '/Game/materials/plastic_green',
    '/Game/materials/plastic_greendark',
    '/Game/materials/plastic_red',
    '/Game/materials/plastic_yellow',
    '/Game/Fab/Cars_for_Arcade_Demolition_Racing_Games/source/Materials/Light',
)


def asset_path(source):
    return DEST + source.rsplit('/', 1)[-1] + '_Fade'


def duplicate(source):
    destination = asset_path(source)
    if unreal.EditorAssetLibrary.does_asset_exist(destination):
        return unreal.load_asset(destination), True
    asset = unreal.EditorAssetLibrary.duplicate_asset(source, destination)
    if not asset:
        original = unreal.load_asset(source)
        if original:
            asset = unreal.AssetToolsHelpers.get_asset_tools().duplicate_asset(
                destination.rsplit('/', 1)[-1], DEST.rstrip('/'), original)
    if not asset:
        raise RuntimeError('Cannot duplicate %s' % source)
    return asset, False


def create_base(source):
    material, existing = duplicate(source)
    if not isinstance(material, unreal.Material):
        raise RuntimeError('Not a base Material: %s' % source)
    edit = unreal.MaterialEditingLibrary
    nodes = edit.get_material_expressions(material)
    fade = next((node for node in nodes
                 if isinstance(node, unreal.MaterialExpressionScalarParameter)
                 and str(node.get_editor_property('parameter_name')) == 'FadeOpacity'), None)
    if not fade:
        fade = edit.create_material_expression(material, unreal.MaterialExpressionScalarParameter, -480, 500)
        fade.set_editor_property('parameter_name', 'FadeOpacity')
        fade.set_editor_property('default_value', 1.0)
    if source.endswith('/glass_window'):
        # 玻璃原本就透明，保留原来的 Opacity 表达式，再乘出入场透明度
        if not existing:
            original_opacity = edit.get_material_property_input_node(material, unreal.MaterialProperty.MP_OPACITY)
            multiply = edit.create_material_expression(material, unreal.MaterialExpressionMultiply, -200, 450)
            assert edit.connect_material_expressions(original_opacity, '', multiply, 'A')
            assert edit.connect_material_expressions(fade, '', multiply, 'B')
            assert edit.connect_material_property(multiply, '', unreal.MaterialProperty.MP_OPACITY)
        material.set_editor_property('blend_mode', unreal.BlendMode.BLEND_TRANSLUCENT)
        material.set_editor_property('translucency_lighting_mode',
                                     unreal.TranslucencyLightingMode.TLM_SURFACE_PER_PIXEL_LIGHTING)
    else:
        # 实心车漆用时间抖动遮罩，过渡时也不会看穿外壳看到内部零件
        material.set_editor_property('blend_mode', unreal.BlendMode.BLEND_MASKED)
        dither = next((node for node in nodes
                       if isinstance(node, unreal.MaterialExpressionMaterialFunctionCall)
                       and node.get_editor_property('material_function')
                       and node.get_editor_property('material_function').get_name() == 'DitherTemporalAA'), None)
        if not dither:
            function = unreal.load_asset('/Engine/Functions/Engine_MaterialFunctions02/Utility/DitherTemporalAA')
            dither = edit.create_material_expression(material, unreal.MaterialExpressionMaterialFunctionCall,
                                                     -200, 500)
            assert dither.set_material_function(function)
        assert edit.connect_material_expressions(fade, '', dither, 'Alpha Threshold')
        assert edit.connect_material_property(dither, 'Result', unreal.MaterialProperty.MP_OPACITY_MASK)
    edit.recompile_material(material)
    assert unreal.EditorAssetLibrary.save_loaded_asset(material)
    unreal.log('[TrafficFade] base %s' % material.get_path_name())
    return material


for source in BASES:
    create_base(source)

for source in INSTANCES:
    material, existing = duplicate(source)
    if not isinstance(material, unreal.MaterialInstanceConstant):
        raise RuntimeError('Not a MaterialInstanceConstant: %s' % source)
    parent = unreal.load_asset(source).get_editor_property('parent')
    fade_parent = unreal.load_asset(asset_path(parent.get_path_name().split('.')[0]))
    if not fade_parent:
        raise RuntimeError('No fade parent for %s' % source)
    material.set_editor_property('parent', fade_parent)
    overrides = material.get_editor_property('base_property_overrides')
    overrides.set_editor_property('override_blend_mode', True)
    overrides.set_editor_property('blend_mode', unreal.BlendMode.BLEND_MASKED)
    material.set_editor_property('base_property_overrides', overrides)
    assert unreal.EditorAssetLibrary.save_loaded_asset(material)
    unreal.log('[TrafficFade] instance %s' % material.get_path_name())

unreal.log('[TrafficFade] saved %d base and %d instance fade materials' %
           (len(BASES), len(INSTANCES)))
