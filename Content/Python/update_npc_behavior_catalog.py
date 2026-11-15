"""创建或增补共享 NPC 行为表。人物表现由独立的显式安装入口配置。"""
from copy import deepcopy
import json
import math
from pathlib import Path
import re


SOURCE_PATH = Path('Design/AI/NpcBehaviorCatalog.json')
TAG_CONFIG_PATH = Path('Config/Tags/NpcBehaviors.ini')
TABLE_PATH = '/Game/NPC/Shared/AI/DT_NpcBehaviorCatalog'
ROW_TYPE = '/Script/Delivery.DeliveryNpcBehaviorCatalogRow'
TAG_PATTERN = re.compile(r'Npc\.Behavior\.[A-Z][A-Za-z0-9]*\.[A-Z][A-Za-z0-9]*')
PROFILE_ROW_TYPE = '/Script/Delivery.DeliveryNpcProfileRow'
PRESENTATION_FIELD = 'BehaviorPresentation'
PROFILE_PRESENTATIONS = (
    ('/Game/NPC/Companion/DT_CompanionProfiles', 'ergou', '/Game/NPC/Companion/BP_Companion_Ergou', (
        ('Npc.Behavior.Attention.LookAt', '/Game/NPC/Companion/Animation/A_ErgouSearchCrouchIdle', 0.0),
        ('Npc.Behavior.Movement.Pace', '/Game/NPC/Residents/Animation/A_ResidentWalk', 80.0),
        ('Npc.Behavior.Movement.MoveAside', '/Game/NPC/Residents/Animation/A_ResidentWalk', 80.0),
    )),
    ('/Game/NPC/Residents/Config/DT_NpcProfiles', 'xiumei', '/Game/NPC/Residents/BP_ResidentShrewd', (
        ('Npc.Behavior.Movement.Pace', '/Game/NPC/Residents/Animation/A_NpcWalk', 180.0),
        ('Npc.Behavior.Movement.MoveAside', '/Game/NPC/Residents/Animation/A_NpcWalk', 180.0),
    )),
)


def read_catalog(project):
    source = json.loads((project / SOURCE_PATH).read_text(encoding='utf-8'))
    rows = source.get('actions')
    if source.get('version') != 1 or not isinstance(rows, list) or not rows:
        raise RuntimeError('行为库源文件缺失或版本错误')
    tags = set()
    actions = set()
    for row in rows:
        fields = ('tag', 'action', 'display_name', 'category', 'description')
        if not isinstance(row, dict) or any(not isinstance(row.get(field), str) or not row[field].strip() for field in fields):
            raise RuntimeError('行为库包含空或无效字段')
        tag = row['tag'].casefold()
        action = row['action'].casefold()
        if not TAG_PATTERN.fullmatch(row['tag']) or tag in tags or action in actions:
            raise RuntimeError('行为标签格式错误、重复标签或重复执行动作：' + row['tag'])
        tags.add(tag)
        actions.add(action)
    return rows


def tag_config(rows):
    # 独立标签文件由 UE 直接读取，数组条目使用不带加号的键
    return '[/Script/GameplayTags.GameplayTagsList]\n' + ''.join(
        f'GameplayTagList=(Tag="{row["tag"]}",DevComment="")\n'
        for row in sorted(rows, key=lambda entry: entry['tag'])
    )


def check_tags(project, rows):
    if (project / TAG_CONFIG_PATH).read_text(encoding='utf-8') != tag_config(rows):
        raise RuntimeError('已注册的行为标签与行为库源文件不同步')


def exported_catalog_rows(exported):
    rows = json.loads(exported)
    if not isinstance(rows, list):
        raise RuntimeError('无法读回行为表')
    by_tag = {}
    seen_tags = set()
    for row in rows:
        if not isinstance(row, dict):
            raise RuntimeError('编译后的行为行不是对象')
        name = row.get('Name')
        action = row.get('Action')
        if (not isinstance(name, str) or not TAG_PATTERN.fullmatch(name) or name.casefold() in seen_tags
                or not isinstance(action, str) or action.rsplit('::', 1)[-1] in ('', 'None')
                or not row.get('DisplayName') or not row.get('Category')):
            raise RuntimeError(f'编译后的行为行内容无效：{name}')
        by_tag[name] = row
        seen_tags.add(name.casefold())
    return by_tag


def catalog_action(row):
    return row['Action'].rsplit('::', 1)[-1]


def verify_catalog_rows(actual, expected_actions):
    if {name: catalog_action(row) for name, row in actual.items()} != expected_actions:
        raise RuntimeError('读回的行为标签和执行枚举与目录不一致')


def catalog_import_row(row):
    # 显式文字键让临时预检表和正式表读回相同身份，已有行沿用原导出的键
    title = 'NSLOCTEXT("DeliveryNpcBehaviorCatalog", ' + json.dumps(row['tag']) + ', ' + json.dumps(row['display_name'], ensure_ascii=False) + ')'
    return {'Name': row['tag'], 'Action': row['action'], 'DisplayName': title,
            'Category': row['category'], 'Description': row['description'], 'bOfferToModel': True,
            'bPlayerRequestOnly': False, 'ExcludedProfiles': []}


def install_catalog():
    import unreal

    editor = unreal.get_editor_subsystem(unreal.UnrealEditorSubsystem)
    if not editor or editor.get_game_world():
        raise RuntimeError('只在普通编辑器中安装行为表')
    project = Path(unreal.Paths.convert_relative_path_to_full(unreal.Paths.project_dir()))
    rows = read_catalog(project)
    check_tags(project, rows)
    row_struct = unreal.load_object(None, ROW_TYPE)
    if not row_struct:
        raise RuntimeError('先构建新版 DeliveryEditor')
    data_table = unreal.DataTableFunctionLibrary
    expected_actions = {row['tag']: row['action'] for row in rows}

    def export(table):
        result = data_table.export_data_table_to_json_string(table)
        if isinstance(result, tuple):
            result = next((value for value in result if isinstance(value, str)), None)
        if not isinstance(result, str):
            raise RuntimeError('导出已加载的行为表失败')
        return result

    def fill(table, payload):
        if not data_table.fill_data_table_from_json_string(table, payload, row_struct):
            raise RuntimeError('行为表导入失败：'+table.get_path_name())

    table = unreal.load_asset(TABLE_PATH)
    if table is not None:
        if not isinstance(table, unreal.DataTable) or table.get_editor_property('row_struct') != row_struct:
            raise RuntimeError('已存在的行为表行类型不同')
        package_name = table.get_outermost().get_path_name()
        if any(package.get_path_name() == package_name
               for package in unreal.EditorLoadingAndSavingUtils.get_dirty_content_packages()):
            raise RuntimeError('行为表存在未保存的编辑，请先处理：'+package_name)
        original_json = export(table)
        original = exported_catalog_rows(original_json)
        for name, row in original.items():
            if expected_actions.get(name) != catalog_action(row):
                raise RuntimeError('已有行为标签缺失或执行枚举变化，保留原表：'+name)
        additions = [catalog_import_row(row) for row in rows if row['tag'] not in original]
        if not additions:
            unreal.log(f'NPC_BEHAVIOR_TABLE_EXISTING {table.get_path_name()} {len(original)}')
            return
        for row in additions:
            name = row['Name']
            tag = unreal.GameplayTag()
            tag.import_text(f'(TagName="{name}")')
            if str(tag.get_editor_property('tag_name')) != name:
                raise RuntimeError('新增行为标签尚未注册：'+name)

        # 先在临时表验证完整导入和旧字段读回，再改动正式表
        payload = json.dumps([*original.values(), *additions], ensure_ascii=False)
        preview = unreal.new_object(unreal.DataTable)
        fill(preview, payload)
        staged = exported_catalog_rows(export(preview))
        verify_catalog_rows(staged, expected_actions)
        if any(staged[name] != row for name, row in original.items()):
            raise RuntimeError('导入会改变已有行为字段，保留原表')

        save_attempted = False
        try:
            table.modify()
            fill(table, payload)
            if exported_catalog_rows(export(table)) != staged:
                raise RuntimeError('正式行为表读回与临时表不符')
            save_attempted = True
            if not unreal.EditorAssetLibrary.save_loaded_asset(table):
                raise RuntimeError('增补后的行为表未保存')
            if exported_catalog_rows(export(table)) != staged:
                raise RuntimeError('保存后的行为表内容变化')
        except Exception as failure:
            try:
                fill(table, original_json)
                if exported_catalog_rows(export(table)) != original:
                    raise RuntimeError('原始行为字段恢复后读回不符')
                if save_attempted and not unreal.EditorAssetLibrary.save_loaded_asset(table):
                    raise RuntimeError('原始行为表恢复后未能保存')
            except Exception as rollback_error:
                raise RuntimeError(f'增补行为表失败，原表恢复未确认：{rollback_error}') from failure
            raise
        unreal.log(f'NPC_BEHAVIOR_TABLE_UPDATED {table.get_path_name()} {len(staged)} added={len(additions)}')
        return
    factory = unreal.DataTableFactory()
    factory.set_editor_property('struct', row_struct)
    folder, name = TABLE_PATH.rsplit('/', 1)
    table = unreal.AssetToolsHelpers.get_asset_tools().create_asset(name, folder, unreal.DataTable, factory)
    if not table:
        raise RuntimeError('行为表导入失败')
    fill(table, json.dumps([catalog_import_row(row) for row in rows], ensure_ascii=False))
    loaded = exported_catalog_rows(export(table))
    verify_catalog_rows(loaded, expected_actions)
    if not unreal.EditorAssetLibrary.save_loaded_asset(table):
        raise RuntimeError('行为表未保存')
    unreal.log(f'NPC_BEHAVIOR_TABLE_SAVED {table.get_path_name()} {len(loaded)}')


def profile_rows(data_tables, table):
    result = data_tables.export_data_table_to_json_string(table)
    if isinstance(result, tuple):
        result = next((item for item in result if isinstance(item, str)), None)
    if not isinstance(result, str):
        raise RuntimeError('人物表导出失败：'+table.get_path_name())
    rows = json.loads(result)
    if (not isinstance(rows, list) or not rows or any(not isinstance(row, dict)
            or not isinstance(row.get('Name'), str) or not row['Name']
            or not isinstance(row.get(PRESENTATION_FIELD), dict) for row in rows)
            or len({row['Name'].casefold() for row in rows}) != len(rows)):
        raise RuntimeError('人物表现行结构或身份有误，请核对已构建的角色行：'+table.get_path_name())
    return {row['Name']: row for row in rows}


def exported_tag_name(key):
    # 数据表中的 Gameplay Tag 键使用 UE 结构体文本，比较时沿用标签身份
    match = re.fullmatch(r'\(TagName="?([^"()]+)"?\)', key)
    return match.group(1) if match else key


def normalized_profile_rows(rows):
    normalized = {}
    for name, row in rows.items():
        variants = row[PRESENTATION_FIELD]
        by_tag = {exported_tag_name(key): entry for key, entry in variants.items()}
        if len(by_tag) != len(variants):
            raise RuntimeError('人物表现包含重复行为标签：'+name)
        normalized[name] = {**row, PRESENTATION_FIELD: by_tag}
    return normalized


def install_profile_behavior_presentations(*, verify_only=False, include_xiumei=True, replace_existing=False):
    """显式执行正式人物表现绑定，读回核对全表以后才保存改动"""
    import unreal

    editor = unreal.get_editor_subsystem(unreal.UnrealEditorSubsystem)
    if not editor or editor.get_game_world():
        raise RuntimeError('只在普通编辑器中配置人物表现')
    row_type = unreal.load_object(None, PROFILE_ROW_TYPE)
    if not row_type:
        raise RuntimeError('先构建包含人物表现字段的 DeliveryEditor')
    assets = unreal.EditorAssetLibrary
    data_tables = unreal.DataTableFunctionLibrary
    dirty = {package.get_path_name() for package in unreal.EditorLoadingAndSavingUtils.get_dirty_content_packages()}
    pending = []
    bindings = []
    for table_path, row_name, blueprint_path, entries in PROFILE_PRESENTATIONS:
        if row_name == 'xiumei' and not include_xiumei:
            continue
        table = assets.load_asset(table_path)
        if not isinstance(table, unreal.DataTable) or table.get_editor_property('row_struct') != row_type:
            raise RuntimeError('缺少正式人物表或行类型不符：'+table_path)
        if table.get_outermost().get_path_name() in dirty:
            raise RuntimeError('人物表存在未保存的编辑，请先处理：'+table_path)
        npc_class = assets.load_blueprint_class(blueprint_path)
        if not npc_class:
            raise RuntimeError('缺少正式人物蓝图：'+blueprint_path)
        npc = unreal.get_default_object(npc_class)
        profile = npc.get_editor_property('profile')
        mesh = npc.get_editor_property('mesh').get_editor_property('skeletal_mesh_asset')
        if profile.get_editor_property('data_table') != table or str(profile.get_editor_property('row_name')) != row_name or not mesh:
            raise RuntimeError('正式人物配置或骨架绑定已经变化：'+blueprint_path)
        skeleton = mesh.get_editor_property('skeleton')
        before = profile_rows(data_tables, table)
        if row_name not in before:
            raise RuntimeError('人物表缺少正式角色：'+row_name)
        after = deepcopy(before)
        variants = after[row_name][PRESENTATION_FIELD]
        for tag_name, animation_path, reference_speed in entries:
            tag = unreal.GameplayTag()
            tag.import_text(f'(TagName="{tag_name}")')
            if str(tag.get_editor_property('tag_name')) != tag_name:
                raise RuntimeError('人物表现标签尚未注册：'+tag_name)
            animation = assets.load_asset(animation_path)
            if (not isinstance(animation, unreal.AnimSequence) or animation.get_path_name().split('.', 1)[0] != animation_path
                    or not skeleton or animation.get_editor_property('skeleton') != skeleton
                    or animation.get_editor_property('enable_root_motion')
                    or animation.get_editor_property('additive_anim_type') != unreal.AdditiveAnimationType.AAT_NONE):
                raise RuntimeError('正式动画必须匹配预期路径和骨架，并且没有根运动或叠加姿势：'+animation_path)
            duration = animation.get_play_length()
            if not math.isfinite(duration) or duration <= 0:
                raise RuntimeError('正式动画缺少有效时长：'+animation_path)
            if row_name == 'xiumei':
                presentation = npc.get_editor_property('presentation')
                if (presentation.get_editor_property('walk_animation') != animation
                        or not math.isclose(presentation.get_editor_property('walk_animation_speed'), reference_speed)):
                    raise RuntimeError('翠花的原步态或参考速度已经变化，请重新核对素材')
            key = next((value for value in variants if exported_tag_name(value) == tag_name), tag.export_text())
            animation_ref = f"{animation.get_class().get_path_name()}'{animation.get_path_name()}'"
            previous = variants.get(key, {})
            updated = {**previous, 'Animation': animation_ref, 'MovementReferenceSpeed': reference_speed, 'PlayRate': 1.0}
            has_existing_settings = (previous.get('Animation') not in (None, '', 'None')
                                     or previous.get('MovementReferenceSpeed', 0) != 0 or previous.get('PlayRate', 1) != 1)
            if has_existing_settings and previous != updated and not replace_existing:
                raise RuntimeError(f'保留已有不同的人物表现设置：{table_path} / {row_name} / {tag_name}，已有动画 {previous.get("Animation")}')
            variants[key] = updated
            bindings.append(dict(profile=row_name, tag=tag_name, animation=animation.get_path_name(),
                                 movement_reference_speed=reference_speed, play_rate=1.0, duration=duration))
        if normalized_profile_rows(before) != normalized_profile_rows(after):
            if verify_only:
                raise RuntimeError('正式人物表现尚未安装或已变化：'+table_path)
            pending.append((table, before, after))

    def apply_rows(table, rows):
        if not data_tables.fill_data_table_from_json_string(table, json.dumps(list(rows.values()), ensure_ascii=False), row_type):
            raise RuntimeError('人物表导入失败：'+table.get_path_name())
        if normalized_profile_rows(profile_rows(data_tables, table)) != normalized_profile_rows(rows):
            raise RuntimeError('人物表现或其他字段读回不符：'+table.get_path_name())

    written = []
    saved = []
    try:
        for table, before, after in pending:
            table.modify()
            written.append((table, before))
            apply_rows(table, after)
        for table, _, after in pending:
            if not assets.save_loaded_asset(table):
                raise RuntimeError('人物表保存失败：'+table.get_path_name())
            saved.append(table)
            if normalized_profile_rows(profile_rows(data_tables, table)) != normalized_profile_rows(after):
                raise RuntimeError('保存后的人物表内容变化：'+table.get_path_name())
    except Exception:
        for table, before in reversed(written):
            try:
                apply_rows(table, before)
                if table in saved and not assets.save_loaded_asset(table):
                    raise RuntimeError('原始人物表恢复保存失败：'+table.get_path_name())
            except Exception as error:
                unreal.log_error(str(error))
        raise

    report = dict(bindings=bindings, saved_tables=[table.get_path_name() for table in saved])
    unreal.log('NPC_PROFILE_BEHAVIOR_PRESENTATIONS '+json.dumps(report, ensure_ascii=False))
    return report


if __name__ == '__main__':
    install_catalog()
