"""将生成的交谈图标导入 UMG 使用的纹理资产"""
from pathlib import Path
import unreal

root=Path(unreal.Paths.project_dir())
for name in ('T_Apology','T_Provoke'):
    task=unreal.AssetImportTask()
    task.filename=str(root/'Design/Art/UI/Conversation'/f'{name}.png')
    task.destination_path='/Game/UI/Conversation'
    task.destination_name=name
    task.automated=True
    task.replace_existing=True
    task.save=False
    unreal.AssetToolsHelpers.get_asset_tools().import_asset_tasks([task])
    texture=unreal.load_asset(f'/Game/UI/Conversation/{name}')
    if not texture: raise RuntimeError('Conversation icon import failed: '+name)
    texture.set_editor_property('compression_settings',unreal.TextureCompressionSettings.TC_EDITOR_ICON)
    texture.set_editor_property('lod_group',unreal.TextureGroup.TEXTUREGROUP_UI)
    texture.set_editor_property('mip_gen_settings',unreal.TextureMipGenSettings.TMGS_NO_MIPMAPS)
    texture.set_editor_property('max_texture_size',256)
    unreal.EditorAssetLibrary.save_loaded_asset(texture)
    unreal.log('CONVERSATION_ICON_IMPORTED '+name)
