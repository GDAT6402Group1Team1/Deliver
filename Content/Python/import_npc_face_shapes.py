"""将眼部形态键副本导入独立目录，并创建可在材质编辑器调整的皮肤遮罩

不修改原 NPC 网格、骨架和纹理；参数、材质节点与形态键保留在 UE 资产中
"""
import unreal
import json
from pathlib import Path
ROOT=Path(unreal.Paths.project_dir())
FOLDER='/Game/NPC/Residents/Appearance'
report=json.loads((ROOT/'Design/Art/NPC/Residents/Appearance/face-shapes.json').read_text(encoding='utf-8'))
lib=unreal.MaterialEditingLibrary
tools=unreal.AssetToolsHelpers.get_asset_tools()
for info in report:
    name=info['mesh'];source_name=name.removesuffix('Face')
    original=unreal.load_asset('/Game/Characters/NPC/'+source_name)
    task=unreal.AssetImportTask();task.filename=str(ROOT/f'Design/Art/NPC/Residents/Appearance/{name}.fbx')
    task.destination_path=FOLDER;task.destination_name=name;task.automated=True;task.save=True;task.replace_existing=True
    options=unreal.FbxImportUI();options.mesh_type_to_import=unreal.FBXImportType.FBXIT_SKELETAL_MESH
    options.import_as_skeletal=True;options.import_mesh=True;options.import_animations=False
    options.import_materials=False;options.import_textures=False;options.create_physics_asset=False
    options.skeleton=original.skeleton
    data=options.get_editor_property('skeletal_mesh_import_data')
    data.set_editor_property('import_morph_targets',True)
    data.set_editor_property('import_uniform_scale',original.get_editor_property('asset_import_data').get_editor_property('import_uniform_scale'))
    data.set_editor_property('vertex_color_import_option',unreal.VertexColorImportOption.REPLACE)
    task.options=options;task.factory=unreal.FbxFactory()
    tools.import_asset_tasks([task])
    mesh=unreal.load_asset(FOLDER+'/'+name)
    if not mesh:raise RuntimeError('Mesh import failed: '+name)
    old=original.get_bounds().box_extent;new=mesh.get_bounds().box_extent
    if abs(old.z-new.z)>1 or abs(old.y-new.y)>1:raise RuntimeError('Imported dimensions changed: '+name+' '+str(new))
    source_material=original.materials[0].material_interface
    material_path=FOLDER+'/M_'+name+'_Face'
    existing=unreal.EditorAssetLibrary.does_asset_exist(material_path)
    material=unreal.load_asset(material_path) if existing else unreal.EditorAssetLibrary.duplicate_asset(source_material.get_path_name(),material_path)
    if not existing:
        base=lib.get_material_property_input_node(material,unreal.MaterialProperty.MP_BASE_COLOR)
        output=lib.get_material_property_input_node_output_name(material,unreal.MaterialProperty.MP_BASE_COLOR)
        if not base:raise RuntimeError('Original base color missing')
        color=lib.create_material_expression(material,unreal.MaterialExpressionVectorParameter,-200,200)
        color.set_editor_property('parameter_name','FaceSkinColor')
        rgba=info['skin_color'];color.set_editor_property('default_value',unreal.LinearColor(*rgba))
        mask=lib.create_material_expression(material,unreal.MaterialExpressionVertexColor,-200,350)
        blend=lib.create_material_expression(material,unreal.MaterialExpressionLinearInterpolate,100,0)
        lib.connect_material_expressions(base,output,blend,'A')
        lib.connect_material_expressions(color,'',blend,'B')
        lib.connect_material_expressions(mask,'R',blend,'Alpha')
        lib.connect_material_property(blend,'',unreal.MaterialProperty.MP_BASE_COLOR)
    # 角色使用骨骼动画和形态键，保存这两种材质用途后再编译
    lib.set_material_usage(material,unreal.MaterialUsage.MATUSAGE_SKELETAL_MESH)
    lib.set_material_usage(material,unreal.MaterialUsage.MATUSAGE_MORPH_TARGETS)
    lib.recompile_material(material);unreal.EditorAssetLibrary.save_loaded_asset(material)
    materials=list(original.materials);materials[0].material_interface=material
    mesh.set_editor_property('materials',materials);mesh.set_editor_property('physics_asset',original.get_editor_property('physics_asset'))
    unreal.EditorAssetLibrary.save_loaded_asset(mesh)
    unreal.log('NPC_FACE_IMPORTED '+name)
