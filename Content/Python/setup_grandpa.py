"""导入爷爷独立资源并创建行为树、角色蓝图；关卡放置单独执行以便复核位置"""
from pathlib import Path
import unreal

ROOT = Path(unreal.Paths.project_dir())
SOURCE = ROOT / 'Design/Art/NPC/Grandpa'
DEST = '/Game/NPC/Grandpa'
assets = unreal.EditorAssetLibrary
tools = unreal.AssetToolsHelpers.get_asset_tools()

def import_fbx(name, skeleton=None):
    path = DEST + '/Animation/' + name if skeleton else DEST + '/' + name
    if assets.does_asset_exist(path):
        existing = assets.load_asset(path)
        if skeleton or existing.skeleton: return existing
    task = unreal.AssetImportTask()
    task.filename = str(SOURCE / (name + '.fbx'))
    task.destination_path, task.destination_name = path.rsplit('/',1)
    task.automated = True
    task.save = True
    task.replace_existing = True
    task.factory = unreal.FbxFactory()
    options = unreal.FbxImportUI()
    options.automated_import_should_detect_type = False
    options.mesh_type_to_import = unreal.FBXImportType.FBXIT_ANIMATION if skeleton else unreal.FBXImportType.FBXIT_SKELETAL_MESH
    options.import_mesh = not skeleton
    options.import_as_skeletal = True
    options.import_animations = bool(skeleton)
    options.import_materials = False
    options.import_textures = False
    options.create_physics_asset = False
    if skeleton:
        options.skeleton = skeleton
        options.anim_sequence_import_data.set_editor_property('use_default_sample_rate', True)
    task.options = options
    tools.import_asset_tasks([task])
    result = assets.load_asset(path)
    if not result: raise RuntimeError('Import failed: '+path+' '+str(task.imported_object_paths))
    return result

mesh = import_fbx('SK_Grandpa')
assets.save_directory(DEST, only_if_is_dirty=False, recursive=True)
clips = {n:import_fbx('A_Grandpa'+n, mesh.skeleton) for n in ['Idle','Talk','Startled','Receive','IdleFoldArms','Surprised','Walk','Look']}
for clip in clips.values():
    clip.set_editor_property('enable_root_motion', False)
    print('GRANDPA_CLIP', clip.get_path_name(), clip.get_play_length())
texture_path = DEST + '/T_Grandpa_BaseColor'
if not assets.does_asset_exist(texture_path):
    task = unreal.AssetImportTask()
    task.filename = str(SOURCE / 'T_Grandpa_BaseColor.png')
    task.destination_path = DEST
    task.automated = True
    task.save = True
    tools.import_asset_tasks([task])
material = assets.load_asset(DEST + '/M_Grandpa') if assets.does_asset_exist(DEST + '/M_Grandpa') else None
if not material:
    material = tools.create_asset('M_Grandpa', DEST, unreal.Material, unreal.MaterialFactoryNew())
    sample = unreal.MaterialEditingLibrary.create_material_expression(material, unreal.MaterialExpressionTextureSample, -300, 0)
    sample.texture = assets.load_asset(texture_path)
    unreal.MaterialEditingLibrary.connect_material_property(sample, 'RGB', unreal.MaterialProperty.MP_BASE_COLOR)
    rough = unreal.MaterialEditingLibrary.create_material_expression(material, unreal.MaterialExpressionConstant, -300, 200)
    rough.r = .85
    unreal.MaterialEditingLibrary.connect_material_property(rough, '', unreal.MaterialProperty.MP_ROUGHNESS)
    unreal.MaterialEditingLibrary.recompile_material(material)
unreal.MaterialEditingLibrary.set_material_usage(material, unreal.MaterialUsage.MATUSAGE_SKELETAL_MESH)
materials = list(mesh.materials)
for slot in materials: slot.material_interface = material
mesh.materials = materials
if not unreal.DeliveryGrandpaAssetTools.create_behavior_assets():
    raise RuntimeError('Failed to create Blueprint BT tasks/tree')
if not unreal.DeliveryGrandpaAssetTools.upgrade_expressive_behavior():
    raise RuntimeError('Failed to upgrade expressive behavior')
bp = assets.load_asset(DEST + '/BP_Grandpa') if assets.does_asset_exist(DEST + '/BP_Grandpa') else None
if not bp:
    factory = unreal.BlueprintFactory()
    factory.set_editor_property('parent_class', unreal.DeliveryGrandpa)
    bp = tools.create_asset('BP_Grandpa', DEST, unreal.Blueprint, factory)
cdo = unreal.get_default_object(bp.generated_class())
cdo.body.set_skeletal_mesh_asset(mesh)
cdo.body.set_relative_scale3d(unreal.Vector(1.12,1.12,1.12))
cdo.body.override_animation_data(clips['IdleFoldArms'], True, True, 0.0, 1.0)
cdo.set_editor_property('behavior', assets.load_asset(DEST + '/AI/BT_Grandpa'))
for field, key in [('idle_animation','IdleFoldArms'),('talk_animation','Talk'),('startled_animation','Surprised'),('receive_animation','Receive'),('walk_animation','Walk'),('look_animation','Look')]:
    cdo.set_editor_property(field, clips[key])
cdo.delivery_target.set_editor_property('expected_task', assets.load_asset('/Game/Task/Definitions/DA_Task_001'))
unreal.BlueprintEditorLibrary.compile_blueprint(bp)
assets.save_directory(DEST, only_if_is_dirty=False, recursive=True)
print('GRANDPA_ASSETS_OK', mesh.get_bounds())
