# -*- coding: utf-8 -*-
"""同步代码对应的英文提示到蓝图覆盖、控件模板和当前正式地图实例"""
import unreal,json,re
from pathlib import Path
ROOT=Path(unreal.Paths.project_dir())
TRANSLATIONS=json.loads((ROOT/'Content/Python/prompt_english_translations.json').read_text(encoding='utf-8'))
rows=[]
changed=set()
def update(obj):
    if not obj:return
    for name in dir(obj):
        if name.startswith('_'):continue
        try:value=obj.get_editor_property(name)
        except Exception:continue
        if not isinstance(value,unreal.Text):continue
        old=str(value)
        if old not in TRANSLATIONS:continue
        new=TRANSLATIONS[old]
        if old==new:continue
        obj.modify()
        obj.set_editor_property(name,unreal.Text(new))
        assert str(obj.get_editor_property(name))==new
        rows.append(dict(object=obj.get_path_name(),property=name,before=old,after=new))
        changed.add(obj.get_path_name().split('.')[0])
assets=[]
for folder in ['/Game/Blueprint','/Game/UI','/Game/Task','/Game/Vehicle/Motorbike']:
    assets.extend(unreal.EditorAssetLibrary.list_assets(folder,recursive=True))
for path in sorted(set(assets)):
    if not any(x in path.split('/')[-1] for x in ['BP_','DA_','DT_']):continue
    asset=unreal.load_asset(path)
    if not asset:continue
    update(asset)
    if isinstance(asset,unreal.Blueprint):
        cdo=unreal.get_default_object(asset.generated_class())
        update(cdo)
        if isinstance(cdo,unreal.Actor):
            for component in cdo.get_components_by_class(unreal.ActorComponent):update(component)
for widget in unreal.ObjectIterator(unreal.Widget):
    if widget.get_path_name().startswith(('/Game/UI/','/Game/Task/UI/')):update(widget)
for path in sorted(changed):
    if not unreal.EditorAssetLibrary.save_asset(path,only_if_is_dirty=False):raise RuntimeError('保存失败 '+path)
# 实例覆盖可能保留旧中文，按完全相同的文案匹配，不改其他参数
levels=unreal.get_editor_subsystem(unreal.LevelEditorSubsystem)
if not levels.load_level('/Game/Level/TestForCharacter'):raise RuntimeError('地图加载失败')
actor_subsystem=unreal.get_editor_subsystem(unreal.EditorActorSubsystem)
map_changed=False
for actor in actor_subsystem.get_all_level_actors():
    components=actor.get_components_by_class(unreal.ActorComponent)
    if not ('Delivery' in actor.get_class().get_name() or 'Motorbike' in actor.get_class().get_name() or any('Delivery' in c.get_class().get_name() for c in components)):continue
    before=len(rows)
    update(actor)
    for component in components:
        if 'Delivery' in component.get_class().get_name():update(component)
    map_changed |= len(rows)>before
if map_changed and not levels.save_current_level():raise RuntimeError('地图保存失败')
(ROOT/'Saved/english_prompt_changes.json').write_text(json.dumps(rows,ensure_ascii=False,indent=2),encoding='utf-8')
unreal.log('[EnglishPrompts] 已更新 %d 项提示，%d 个资源，地图修改=%s'%(len(rows),len(changed),map_changed))
