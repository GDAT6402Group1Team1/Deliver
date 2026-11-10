# -*- coding: utf-8 -*-
"""为四种成人 NPC 复制渐隐材质，保留原始材质不变"""
import unreal

PAIRS = (
    ('tripo_mat_3f3a4485', 'M_Man_Fade'),
    ('tripo_mat_f74add70', 'M_Woman_Fade'),
    ('tripo_mat_9013ca7f', 'M_Grandpa_Fade'),
    ('tripo_mat_0407c969', 'M_Grandma_Fade'),
)
ROOT = '/Game/Characters/NPC/'

for source, target in PAIRS:
    src, dst = ROOT+source, ROOT+target
    existing = unreal.EditorAssetLibrary.does_asset_exist(dst)
    mat = unreal.load_asset(dst) if existing else unreal.EditorAssetLibrary.duplicate_asset(src, dst)
    if not isinstance(mat, unreal.Material):
        raise RuntimeError('Cannot duplicate material ' + src)
    mat.set_editor_property('blend_mode', unreal.BlendMode.BLEND_TRANSLUCENT)
    mat.set_editor_property('translucency_lighting_mode',
                            unreal.TranslucencyLightingMode.TLM_SURFACE_PER_PIXEL_LIGHTING)
    if not existing:
        param = unreal.MaterialEditingLibrary.create_material_expression(
            mat, unreal.MaterialExpressionScalarParameter, -350, 280)
        param.set_editor_property('parameter_name', 'FadeOpacity')
        param.set_editor_property('default_value', 1.0)
        if not unreal.MaterialEditingLibrary.connect_material_property(
                param, '', unreal.MaterialProperty.MP_OPACITY):
            raise RuntimeError('Cannot connect opacity on ' + dst)
    unreal.MaterialEditingLibrary.recompile_material(mat)
    if not unreal.EditorAssetLibrary.save_loaded_asset(mat):
        raise RuntimeError('Cannot save ' + dst)
    unreal.log('[AdultFade] saved ' + dst)
