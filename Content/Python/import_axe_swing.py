"""Import the Mixamo source clip without changing the player's skeleton or map."""
import unreal

DEST = "/Game/Characters/Animations/Axe"
SOURCE = "C:/Users/26349/Downloads/Standing Melee Attack Downward.fbx"
assets = unreal.EditorAssetLibrary
existing = assets.list_assets(DEST, recursive=True) if assets.does_directory_exist(DEST) else []
if not existing:
    task = unreal.AssetImportTask()
    task.filename = SOURCE
    task.destination_path = DEST
    task.destination_name = "SK_MixamoAxe"
    task.automated = True
    task.save = True
    task.factory = unreal.FbxFactory()
    options = unreal.FbxImportUI()
    options.automated_import_should_detect_type = False
    options.mesh_type_to_import = unreal.FBXImportType.FBXIT_SKELETAL_MESH
    options.import_mesh = True
    options.import_as_skeletal = True
    options.import_animations = True
    options.import_materials = False
    options.import_textures = False
    options.create_physics_asset = False
    options.anim_sequence_import_data.set_editor_property("use_default_sample_rate", True)
    task.options = options
    unreal.AssetToolsHelpers.get_asset_tools().import_asset_tasks([task])
clips = []
for path in assets.list_assets(DEST, recursive=True):
    asset = assets.load_asset(path)
    if isinstance(asset, unreal.AnimSequence):
        clips.append(asset)
if len(clips) != 1:
    raise RuntimeError("Expected exactly one source animation; got %s" % clips)
clip = clips[0]
target = DEST + "/A_AxeDownward"
if clip.get_path_name().split(".")[0] != target:
    if not assets.rename_asset(clip.get_path_name(), target):
        raise RuntimeError("Could not rename source animation")
clip.set_editor_property("enable_root_motion", False)
assets.save_directory(DEST, only_if_is_dirty=False, recursive=True)
unreal.log("AXE_IMPORT_OK %s length=%s" % (clip.get_path_name(), clip.get_play_length()))
