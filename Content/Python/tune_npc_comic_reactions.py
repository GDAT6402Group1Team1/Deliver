"""放大已有 NPC 倾听、歪头和眼神变化，只修改表情配置资产"""
import copy
from datetime import datetime, timezone
import json
from pathlib import Path


ASSET_PATH = '/Game/NPC/Residents/Appearance/DA_NpcExpressions'
TUNING = {
    'conversation_pose': True,
    'listening_lean': 9.0,
    'listening_head_tilt': 10.0,
    'thinking_head_tilt': -11.0,
    'head_turn_limit': 50.0,
    'posture_blend_speed': 11.0,
    'gaze_follow_speed': 14.0,
    'max_eye_yaw': 18.0,
    'max_eye_pitch': 14.0,
    'blend_speed': 14.0,
}
POSE_TUNING = {'Listening': {'FaceListening': 1.0, 'FaceSurprised': 0.25}}
SCALAR_FIELDS = (
    'blend_speed', 'blink_duration', 'max_eye_yaw', 'max_eye_pitch',
    'conversation_pose', 'listening_lean', 'listening_head_tilt',
    'thinking_head_tilt', 'head_turn_limit', 'posture_blend_speed', 'gaze_follow_speed',
)
NAME_FIELDS = ('blink_morph', 'look_left_morph', 'look_right_morph', 'look_up_morph', 'look_down_morph')


def main():
    import unreal

    def snapshot(asset):
        result = {field: asset.get_editor_property(field) for field in SCALAR_FIELDS}
        result.update({field: str(asset.get_editor_property(field)) for field in NAME_FIELDS})
        interval = asset.get_editor_property('blink_interval')
        result['blink_interval'] = [interval.x, interval.y]
        curve = asset.get_editor_property('blink_curve')
        result['blink_curve'] = curve.get_path_name() if curve else None
        result['poses'] = {
            str(name): {str(morph): float(weight) for morph, weight in pose.get_editor_property('morph_weights').items()}
            for name, pose in asset.get_editor_property('poses').items()
        }
        return result

    def set_poses(asset, values):
        poses = {}
        for name, weights in values.items():
            pose = unreal.DeliveryNpcExpressionPose()
            pose.set_editor_property('morph_weights', {unreal.Name(key): value for key, value in weights.items()})
            poses[unreal.Name(name)] = pose
        asset.set_editor_property('poses', poses)

    asset = unreal.load_asset(ASSET_PATH)
    if not isinstance(asset, unreal.DeliveryNpcExpressionSet):
        raise RuntimeError('没有找到现有 NPC 表情配置：'+ASSET_PATH)
    before = snapshot(asset)
    if 'Listening' not in before['poses'] or 'Angry' not in before['poses']:
        raise RuntimeError('表情配置缺少倾听或生气状态，先检查正式资产')
    expected = copy.deepcopy(before)
    expected.update(TUNING)
    for name, weights in POSE_TUNING.items():
        expected['poses'][name].update(weights)

    root = Path(unreal.Paths.project_dir()).resolve()/'Saved/AIResearch/NpcComicReactions'
    output = root/datetime.now(timezone.utc).strftime('%Y%m%dT%H%M%S%fZ')
    output.mkdir(parents=True, exist_ok=False)
    report = {
        'status': 'checked_before_update', 'asset': ASSET_PATH,
        'before': before, 'expected': expected, 'after': None,
        'sources': ['Source/Delivery/AI/Presentation/DeliveryNpcExpressionSet.h',
                    'Source/Delivery/AI/Presentation/DeliveryNpcConversationAnimInstance.cpp',
                    'Content/Python/generate_npc_face_shapes.py'],
        'notes': ['倾听时向前探身，疑惑时向另一边歪头',
                  '生气表情保持原权重，避免同时压缩眼睛造成形状翻转',
                  '保留眨眼曲线、声音、模型、动画图、攻击窗口和移动参数',
                  '这些数值需要在游戏画面中检查，保存配置不代表已经完成观感验收'],
    }

    def save_report():
        (output/'report.json').write_text(json.dumps(report, ensure_ascii=False, indent=2), encoding='utf-8')

    save_report()
    asset.modify()
    try:
        for field, value in TUNING.items():
            asset.set_editor_property(field, value)
        set_poses(asset, expected['poses'])
        report['after'] = snapshot(asset)
        if report['after'] != expected:
            raise RuntimeError('表情配置出现计划外的变化，停止保存')
    except Exception as error:
        for field in TUNING:
            asset.set_editor_property(field, before[field])
        set_poses(asset, before['poses'])
        report.update(status='failed_no_asset_saved', error=str(error))
        save_report()
        raise
    report.update(status='checked_before_save', unchanged_other_fields=True)
    save_report()
    if not unreal.EditorAssetLibrary.save_loaded_asset(asset):
        report.update(status='save_failed')
        save_report()
        raise RuntimeError('表情配置保存失败')
    report.update(status='saved', saved_assets=[asset.get_path_name()])
    save_report()
    (root/'latest.json').write_text(json.dumps(report, ensure_ascii=False, indent=2), encoding='utf-8')
    unreal.log('NPC_COMIC_REACTIONS_SAVED '+str(output/'report.json'))


if __name__ == '__main__':
    main()
