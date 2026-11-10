# -*- coding: utf-8 -*-
"""复制小孩原材质并增加 FadeOpacity，不修改原材质。"""
import unreal

pairs = (
    ('/Game/Characters/NPC/tripo_mat_286dda7d', '/Game/Characters/NPC/M_ChildBoy_Fade'),
    ('/Game/Characters/NPC/tripo_mat_76e32ca4', '/Game/Characters/NPC/M_ChildGirl_Fade'),
)
for src, dst in pairs:
    existing = unreal.EditorAssetLibrary.does_asset_exist(dst)
    mat = unreal.load_asset(dst) if existing else unreal.EditorAssetLibrary.duplicate_asset(src, dst)
    if not mat:
        raise RuntimeError('Cannot duplicate ' + src)
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
    unreal.log('[ChildFade] saved ' + dst)
