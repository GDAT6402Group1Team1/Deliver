"""导入翠花眉形到独立预览资产，检查通过后才允许显式替换正式女性模型

在已关闭项目编辑器的 UnrealEditor-Cmd Python 环境调用 main()
main(verify_only=True) 只检查已导入候选；main(promote=True) 备份并替换正式女性资产
不加载地图、不改蓝图，不处理男性资产；检查报告保存在 Saved/AIResearch/NpcWomanBrowsCandidate
"""
import argparse
from datetime import datetime, timezone
import hashlib
import json
from pathlib import Path
import shutil
import subprocess
import sys


ROOT = Path(__file__).resolve().parents[2]
WORK = ROOT / 'Saved/AIResearch/NpcWomanBrowsCandidate'
SOURCE = WORK / 'SK_NpcWomanFace.fbx'
FORMAL = '/Game/NPC/Residents/Appearance/SK_NpcWomanFace'
CANDIDATE = '/Game/NPC/Residents/Appearance/Preview/SK_NpcWomanBrowsCandidate'
FORMAL_FBX = ROOT / 'Design/Art/NPC/Residents/Appearance/SK_NpcWomanFace.fbx'
FACE_REPORT = FORMAL_FBX.parent / 'face-shapes.json'
RECEIPT = WORK / 'ue-import-latest.json'

# 使用本机 UE 5.8 的属性名，未知版本或字段变化会停止，避免漏掉用户设置
MESH_SETTINGS = (
    'Materials', 'PhysicsAsset', 'PostProcessAnimBlueprint', 'PostProcessAnimBPLODThreshold',
    'LODSettings', 'MinLod', 'MinQualityLevelLOD', 'DisableBelowMinLodStripping',
    'bOverrideLODStreamingSettings', 'bSupportLODStreaming', 'MaxNumStreamedLODs',
    'MaxNumOptionalLODs', 'RayTracingMinLOD', 'PositiveBoundsExtension', 'NegativeBoundsExtension',
    'DefaultMeshDeformer', 'DefaultAnimatingRig', 'OverlayMaterial', 'OverlayMaterialMaxDrawDistance',
)
LOD_SETTINGS = (
    'ScreenSize', 'LODHysteresis', 'BuildSettings', 'ReductionSettings',
    'BonesToRemove', 'BonesToPrioritize', 'SectionsToPrioritize', 'WeightOfPrioritization',
    'BakePose', 'BakePoseOverride', 'SkinCacheUsage', 'MorphTargetPositionErrorTolerance',
    'VertexAttributes', 'bAllowCPUAccess', 'bBuildHalfEdgeBuffers', 'bAllowMeshDeformer',
    'bSupportUniformlyDistributedSampling',
)
IMPORT_SETTINGS = ('ImportTranslation', 'ImportRotation', 'ImportUniformScale',
                   'bConvertScene', 'bForceFrontXAxis', 'bConvertSceneUnit',
                   'NormalImportMethod', 'NormalGenerationMethod', 'bPreserveSmoothingGroups',
                   'bImportMeshesInBoneHierarchy', 'ThresholdPosition', 'ThresholdTangentNormal',
                   'ThresholdUV', 'MorphThresholdPosition')


def sha256(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def write_json(path, value):
    path = Path(path)
    temporary = path.with_name(path.name + '.tmp')
    temporary.write_text(json.dumps(value, ensure_ascii=False, indent=2), encoding='utf-8')
    temporary.replace(path)


def asset_file(asset_path):
    if not asset_path.startswith('/Game/'):
        raise RuntimeError('只允许处理工程 Content 下的资产：' + asset_path)
    return ROOT / 'Content' / (asset_path[6:].split('.')[0] + '.uasset')


def ensure_editor_closed():
    command = "Get-CimInstance Win32_Process | Where-Object { $_.Name -eq 'UnrealEditor.exe' -and $_.CommandLine -match 'Delivery\\.uproject' } | Select-Object -ExpandProperty ProcessId"
    result = subprocess.run(['powershell.exe', '-NoProfile', '-Command', command],
                            capture_output=True, text=True, check=True)
    if result.stdout.strip():
        raise RuntimeError('Delivery 编辑器正在运行，停止导入；请在独立命令行进程执行')


def describe(value):
    if value is None or isinstance(value, (str, bool, int, float)):
        return value
    if hasattr(value, 'get_path_name'):
        return value.get_path_name()
    if hasattr(value, 'export_text'):
        return value.export_text()
    if hasattr(value, 'items'):
        return {str(k): describe(v) for k, v in value.items()}
    if hasattr(value, '__iter__'):
        return [describe(v) for v in value]
    return str(value)


def copy_setting(value):
    # 只复制配置值，不复制 SourceModel 的模型数据，也不复制材质或骨架对象
    if value is None or isinstance(value, (str, bool, int, float)) or hasattr(value, 'get_path_name'):
        return value
    if hasattr(value, 'export_text'):
        return value.copy()
    if hasattr(value, 'items'):
        return {k: copy_setting(v) for k, v in value.items()}
    if hasattr(value, '__iter__'):
        return [copy_setting(v) for v in value]
    return value


def bounds(mesh):
    value = mesh.get_bounds()
    return {'origin': [value.origin.x, value.origin.y, value.origin.z],
            'extent': [value.box_extent.x, value.box_extent.y, value.box_extent.z],
            'radius': value.sphere_radius}


def capture_settings(mesh, subsystem):
    # UE 5.8 把每级 LOD 配置放在 SourceModels，不能读取旧版的 LODInfo 数组
    result = {'mesh': {name: copy_setting(mesh.get_editor_property(name)) for name in MESH_SETTINGS},
              'lods': [{name: copy_setting(model.get_editor_property(name)) for name in LOD_SETTINGS}
                       for model in mesh.get_editor_property('SourceModels')]}
    if subsystem.get_lod_count(mesh) != len(result['lods']):
        raise RuntimeError('源 LOD 数量与实际 LOD 不一致，停止处理')
    # LODMaterialMap 不向 Python 开放，使用编辑器接口核对每个 Section 实际使用的材质槽
    result['section_material_slots'] = [
        [subsystem.get_lod_material_slot(mesh, lod, section)
         for section in range(subsystem.get_num_sections(mesh, lod))]
        for lod in range(len(result['lods']))]
    return result


def restore_settings(mesh, settings, subsystem):
    # 先恢复全局 LOD 资源，再恢复这张网格单独覆盖的参数
    for name in ('LODSettings',) + tuple(n for n in MESH_SETTINGS if n != 'LODSettings'):
        mesh.set_editor_property(name, copy_setting(settings['mesh'][name]))
    models = mesh.get_editor_property('SourceModels')
    if len(models) != len(settings['lods']):
        raise RuntimeError('导入改变了 LOD 数量，停止保存')
    for index, (model, values) in enumerate(zip(models, settings['lods'])):
        for name, value in values.items():
            if name != 'BuildSettings':
                model.set_editor_property(name, copy_setting(value))
        # 用引擎的编辑器接口完成修改通知与网格重建
        subsystem.set_lod_build_settings(mesh, index, values['BuildSettings'])
    current = describe(capture_settings(mesh, subsystem))
    expected = describe(settings)
    if current != expected:
        write_json(WORK / 'settings-mismatch.json', {'mesh': mesh.get_path_name(), 'bounds': bounds(mesh),
                   'expected': expected, 'actual': current})
        raise RuntimeError('材料、物理、后处理动画或 LOD 配置未保留，停止保存')


def read_import_settings(unreal, mesh, notes):
    data = mesh.get_editor_property('AssetImportData')
    notes['record_class'] = data.get_class().get_path_name()
    if isinstance(data, unreal.FbxSkeletalMeshImportData):
        return {name: copy_setting(data.get_editor_property(name)) for name in IMPORT_SETTINGS}
    if not isinstance(data, unreal.InterchangeAssetImportData):
        raise RuntimeError('无法识别正式女性的导入设置，停止处理')
    pipeline = next((p for p in data.get_pipelines()
                     if isinstance(p, unreal.InterchangeGenericAssetsPipeline)), None)
    translator = data.get_translator_settings()
    if pipeline is None:
        raise RuntimeError('正式女性没有通用 Interchange 导入配置')
    notes['pipeline_class'] = pipeline.get_class().get_path_name()
    notes['translator_saved'] = isinstance(translator, unreal.InterchangeFbxTranslatorSettings)
    # 引擎允许旧资产不保存转换设置；官方转换器此时读取 FBX 转换器的默认对象
    if not notes['translator_saved']:
        translator = unreal.get_default_object(unreal.InterchangeFbxTranslatorSettings)
    notes['translator_source'] = 'saved' if notes['translator_saved'] else 'engine_default_object'
    common = pipeline.get_editor_property('CommonMeshesProperties')
    mesh_pipeline = pipeline.get_editor_property('MeshPipeline')
    # 按 UE 5.8 InterchangeFbxAssetImportDataConverter 的对应关系读取原有设置
    result = {old: copy_setting(pipeline.get_editor_property(new)) for old, new in (
        ('ImportTranslation', 'ImportOffsetTranslation'), ('ImportRotation', 'ImportOffsetRotation'),
        ('ImportUniformScale', 'ImportOffsetUniformScale'))}
    result.update({name: translator.get_editor_property(name)
                   for name in ('bConvertScene', 'bForceFrontXAxis', 'bConvertSceneUnit')})
    result.update({name: mesh_pipeline.get_editor_property(name)
                   for name in ('ThresholdPosition', 'ThresholdTangentNormal', 'ThresholdUV', 'MorphThresholdPosition')})
    result['NormalGenerationMethod'] = (unreal.FBXNormalGenerationMethod.MIKK_T_SPACE
        if common.get_editor_property('bUseMikkTSpace') else unreal.FBXNormalGenerationMethod.BUILT_IN)
    if common.get_editor_property('bRecomputeNormals'):
        result['NormalImportMethod'] = unreal.FBXNormalImportMethod.FBXNIM_COMPUTE_NORMALS
    elif common.get_editor_property('bRecomputeTangents'):
        result['NormalImportMethod'] = unreal.FBXNormalImportMethod.FBXNIM_IMPORT_NORMALS
    else:
        result['NormalImportMethod'] = unreal.FBXNormalImportMethod.FBXNIM_IMPORT_NORMALS_AND_TANGENTS
    result['bPreserveSmoothingGroups'] = True
    result['bImportMeshesInBoneHierarchy'] = common.get_editor_property('bConvertStaticsInBoneHierarchyToSkeletals')
    for old, new in (('bComputeWeightedNormals', 'bComputeWeightedNormals'),
                     ('bTransformVertexToAbsolute', 'bBakeMeshes'), ('bBakePivotInVertex', 'bBakePivotMeshes'),
                     ('bKeepSectionsSeparate', 'bKeepSectionsSeparate')):
        result[old] = common.get_editor_property(new)
    result['bImportVertexAttributes'] = mesh_pipeline.get_editor_property('bImportVertexAttributes')
    return result


def import_mesh(unreal, path, filename, original, import_settings):
    task = unreal.AssetImportTask()
    task.filename = str(filename)
    task.destination_path, task.destination_name = path.rsplit('/', 1)
    task.automated = True
    task.save = False
    task.replace_existing = True
    task.replace_existing_settings = True
    options = unreal.FbxImportUI()
    options.mesh_type_to_import = unreal.FBXImportType.FBXIT_SKELETAL_MESH
    options.import_as_skeletal = True
    options.import_mesh = True
    options.import_animations = False
    options.import_materials = False
    options.import_textures = False
    options.create_physics_asset = False
    options.skeleton = original.get_editor_property('Skeleton')
    data = options.get_editor_property('skeletal_mesh_import_data')
    for name, value in import_settings.items():
        data.set_editor_property(name, copy_setting(value))
    data.set_editor_property('import_morph_targets', True)
    data.set_editor_property('import_mesh_lods', False)
    data.set_editor_property('update_skeleton_reference_pose', False)
    data.set_editor_property('use_t0_as_ref_pose', False)
    data.set_editor_property('vertex_color_import_option', unreal.VertexColorImportOption.REPLACE)
    if unreal.EditorAssetLibrary.does_asset_exist(path):
        existing = unreal.load_asset(path)
        if isinstance(existing.get_editor_property('AssetImportData'), unreal.InterchangeAssetImportData):
            # 旧 FBX 重导器会用资产上的记录覆盖传入参数；先转换记录，避免退回默认缩放 1
            replacement = unreal.FbxSkeletalMeshImportData(outer=existing)
            for name, value in import_settings.items():
                replacement.set_editor_property(name, copy_setting(value))
            for name in ('import_morph_targets', 'import_mesh_lods', 'update_skeleton_reference_pose',
                         'use_t0_as_ref_pose', 'vertex_color_import_option'):
                replacement.set_editor_property(name, data.get_editor_property(name))
            replacement.scripted_add_filename(str(filename), 0, '')
            existing.set_editor_property('AssetImportData', replacement)
    task.options = options
    task.factory = unreal.FbxFactory()
    unreal.AssetToolsHelpers.get_asset_tools().import_asset_tasks([task])
    mesh = unreal.load_asset(path)
    if not mesh or not isinstance(mesh, unreal.SkeletalMesh):
        raise RuntimeError('候选骨骼网格导入失败：' + path)
    return mesh


def read_morph_stats(unreal, mesh, folder):
    # 直接检查 UE 导入后的位移，避免无渲染模式下 FBX 导出器创建材质烘焙组件
    report = json.loads(unreal.DeliveryNpcToolset.get_npc_morph_stats(mesh, 'BrowsAngry', 0))
    passed = (report.get('valid_data') and
              0 < report['nonzero_position_deltas'] < report['base_vertex_count'])
    report['passed'] = bool(passed)
    write_json(folder / 'morph-readback.json', report)
    if not passed:
        raise RuntimeError('UE 导入后的 BrowsAngry 没有有效位移：' + report.get('error', '顶点数量异常'))
    return report


def check_mesh(unreal, mesh, original_skeleton, original_bounds, settings, subsystem, folder):
    if mesh.get_editor_property('Skeleton') != original_skeleton:
        raise RuntimeError('候选骨架与正式女性骨架不一致')
    current_bounds = bounds(mesh)
    differences = [abs(a-b) for field in ('origin', 'extent')
                   for a, b in zip(original_bounds[field], current_bounds[field])]
    differences.append(abs(original_bounds['radius']-current_bounds['radius']))
    if max(differences) > 0.05:
        raise RuntimeError('网格 Bounds 变化超过 0.05 cm，停止保存')
    current_settings = describe(capture_settings(mesh, subsystem))
    if current_settings != describe(settings):
        raise RuntimeError('材料、物理、后处理动画或 LOD 配置不一致')
    morph = read_morph_stats(unreal, mesh, folder)
    return {'skeleton': original_skeleton.get_path_name(), 'bounds': current_bounds,
            'bounds_max_difference_cm': max(differences), 'settings': current_settings,
            'lod_count': subsystem.get_lod_count(mesh), 'morph_readback': morph}


def backup_files(paths, folder):
    result = []
    for path in paths:
        path = Path(path)
        if not path.is_file():
            continue
        relative = path.resolve().relative_to(ROOT.resolve())
        backup = folder / 'backup' / relative
        backup.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(path, backup)
        result.append({'source': str(path), 'backup': str(backup), 'sha256': sha256(path)})
    write_json(folder / 'backup.json', result)
    return result


def protected_hashes(extra=()):
    paths = [FORMAL_FBX, FACE_REPORT, asset_file(FORMAL),
             FORMAL_FBX.parent / 'SK_NpcManFace.fbx',
             asset_file('/Game/NPC/Residents/Appearance/SK_NpcManFace')]
    paths.extend(extra)
    return {str(path): sha256(path) for path in paths if path.is_file()}


def main(promote=False, verify_only=False):
    import unreal
    if promote and verify_only:
        raise RuntimeError('promote 与 verify_only 不能同时使用')
    ensure_editor_closed()
    review = json.loads((WORK / 'review-status.json').read_text(encoding='utf-8'))
    if review.get('status') != 'candidate_v3_ready_for_ue_preview' or sha256(SOURCE) != review['candidate_fbx_sha256']:
        raise RuntimeError('候选文件或检查状态发生变化，请先重新检查预览')
    original = unreal.load_asset(FORMAL)
    if not isinstance(original, unreal.SkeletalMesh):
        raise RuntimeError('找不到正式女性骨骼网格')
    subsystem = unreal.get_editor_subsystem(unreal.SkeletalMeshEditorSubsystem)
    original_skeleton = original.get_editor_property('Skeleton')
    original_bounds = bounds(original)
    settings = capture_settings(original, subsystem)
    # 目前候选只带 LOD0；遇到新加的 LOD 或额外蒙皮时停止，不丢弃用户后来添加的数据
    if len(settings['lods']) != 1 or list(original.get_editor_property('SkinWeightProfiles')):
        raise RuntimeError('正式女性具有额外 LOD 或蒙皮配置，需要专项导入检查；未修改任何资产')
    import_notes = {}
    import_settings = read_import_settings(unreal, original, import_notes)
    folder = WORK / 'UEImport' / datetime.now(timezone.utc).strftime('%Y%m%dT%H%M%S%fZ')
    folder.mkdir(parents=True, exist_ok=False)
    skeleton_file = asset_file(original_skeleton.get_path_name())
    before = protected_hashes((skeleton_file,))
    write_json(folder / 'before.json', {'protected_sha256': before, 'bounds': original_bounds,
               'skeleton': original_skeleton.get_path_name(), 'settings': describe(settings),
               'import_settings': describe(import_settings), 'import_record': import_notes})
    backups = []
    try:
        if promote:
            previous = json.loads(RECEIPT.read_text(encoding='utf-8'))
            if (previous.get('status') != 'candidate_imported_checked'
                    or previous.get('source_sha256') != sha256(SOURCE)
                    or previous.get('candidate_asset_sha256') != sha256(asset_file(CANDIDATE))
                    or previous.get('protected_sha256') != before):
                raise RuntimeError('候选或正式配置与上次导入检查不一致，请先重新导入候选')
            candidate = unreal.load_asset(CANDIDATE)
            check_mesh(unreal, candidate, original_skeleton, original_bounds, settings, subsystem, folder)
            ensure_editor_closed()
            package = asset_file(FORMAL)
            backups = backup_files([FORMAL_FBX, FACE_REPORT, package,
                                    package.with_suffix('.uexp'), package.with_suffix('.ubulk')], folder)
            rows = json.loads(FACE_REPORT.read_text(encoding='utf-8'))
            indices = [i for i, row in enumerate(rows) if row.get('mesh') == 'SK_NpcWomanFace']
            if len(indices) != 1:
                raise RuntimeError('正式 face-shapes.json 女性条目数量异常')
            write_json(folder / 'female-entry-before.json', rows[indices[0]])
            shutil.copy2(SOURCE, FORMAL_FBX)
            mesh = import_mesh(unreal, FORMAL, FORMAL_FBX, original, import_settings)
        elif verify_only:
            mesh = unreal.load_asset(CANDIDATE)
            if not isinstance(mesh, unreal.SkeletalMesh):
                raise RuntimeError('尚未导入独立候选')
        else:
            mesh = import_mesh(unreal, CANDIDATE, SOURCE, original, import_settings)
        if not verify_only:
            restore_settings(mesh, settings, subsystem)
        check_folder = folder / 'formal-readback' if promote else folder
        check_folder.mkdir(exist_ok=True)
        checked = check_mesh(unreal, mesh, original_skeleton, original_bounds, settings, subsystem, check_folder)
        write_json(folder / 'checked.json', checked)
        ensure_editor_closed()
        if not verify_only and not unreal.EditorAssetLibrary.save_loaded_asset(mesh, only_if_is_dirty=False):
            raise RuntimeError('网格检查通过，但保存失败')
        if promote:
            candidate_row = json.loads((WORK / 'face-shapes.json').read_text(encoding='utf-8'))[0]
            updated = dict(rows[indices[0]])
            for name in ('skin_color', 'eyes', 'separate_brows', 'morphs', 'vertex_count'):
                updated[name] = candidate_row[name]
            updated['brows_angry_vertices'] = candidate_row['moved_brow_vertices']
            updated['brow_selection'] = 'Design/Art/NPC/Residents/Appearance/WomanBrowSelection.json'
            rows[indices[0]] = updated
            write_json(FACE_REPORT, rows)
        after = protected_hashes((skeleton_file,))
        allowed = {str(FORMAL_FBX), str(FACE_REPORT), str(asset_file(FORMAL))} if promote else set()
        changed = [path for path, digest in before.items() if path not in allowed and after.get(path) != digest]
        if changed:
            raise RuntimeError('受保护文件发生变化：' + ', '.join(changed))
        report = {'status': 'promoted_checked' if promote else 'candidate_imported_checked',
                  'source_sha256': sha256(SOURCE), 'candidate_asset_sha256': sha256(asset_file(CANDIDATE)),
                  'protected_sha256': before, 'protected_after_sha256': after, 'folder': str(folder),
                  'checks': checked, 'mode': 'verify' if verify_only else ('promote' if promote else 'candidate'),
                  'gameplay_visual_review': 'pending'}
        write_json(folder / 'result.json', report)
        if not promote:
            write_json(RECEIPT, report)
        unreal.log('NPC_WOMAN_BROWS_REPORT=' + str(folder))
        return report
    except Exception as error:
        # 仅恢复本次备份过的正式女性文件；命令行进程结束后重新打开工程再检查
        restored = []
        pending_restore = []
        for item in backups:
            if Path(item['source']).is_file() and sha256(item['source']) == item['sha256']:
                continue
            try:
                shutil.copy2(item['backup'], item['source'])
                restored.append(item)
            except OSError as restore_error:
                pending_restore.append(dict(item, error=str(restore_error)))
        write_json(folder / 'failure.json', {'error': str(error), 'restored_backup_files': restored,
                   'restore_after_process_exit': pending_restore,
                   'restart_required': bool(backups), 'formal_assets_not_promoted': not pending_restore})
        raise


if __name__ == '__main__':
    args = sys.argv[sys.argv.index('--')+1:] if '--' in sys.argv else sys.argv[1:]
    parser = argparse.ArgumentParser()
    parser.add_argument('--promote', action='store_true')
    parser.add_argument('--verify-only', action='store_true')
    options = parser.parse_args(args)
    main(promote=options.promote, verify_only=options.verify_only)
