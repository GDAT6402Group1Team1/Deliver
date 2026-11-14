"""创建二狗专属车资产，默认只读

-CreateCompanionRidePose 只创建独立可编辑坐姿，不修改源网格、骨架或居民动画
-SetupCompanionVehicle 在完整编译后创建专属车蓝图并绑定地图里的二狗
-CalibrateCompanionVehicle 显式备份后按实际车座、把手和脚踏调整独立坐姿
不启动 PIE，不调用运行时驾驶、交接或模型接口
"""
from datetime import datetime
import json
import math
from pathlib import Path
import shutil
import unreal

ROOT = '/Game/NPC/Companion'
RIDE = ROOT + '/Animation/A_ErgouRide'
BIKE = ROOT + '/Vehicle/BP_ErgouMotorbike'
BOY = ROOT + '/Appearance/SK_ErgouBoy'
MAP = '/Game/Level/TestForCharacter'
SOURCE_BIKE = '/Game/Vehicle/Motorbike/BP_Motorbike'


def load(path):
    value = unreal.load_asset(path)
    if not value:
        raise RuntimeError('缺少资产：' + path)
    return value


def save(asset):
    if not unreal.EditorAssetLibrary.save_loaded_asset(asset, only_if_is_dirty=False):
        raise RuntimeError('保存失败：' + asset.get_path_name())


def xyz(v):
    return [v.x, v.y, v.z]


def pose_positions(animation, mesh):
    options = unreal.AnimPoseEvaluationOptions()
    options.set_editor_property('evaluation_type', unreal.AnimDataEvalType.COMPRESSED)
    options.set_editor_property('optional_skeletal_mesh', mesh)
    pose = unreal.AnimPoseExtensions.get_anim_pose_at_time(animation, 0, options)
    return {str(bone): xyz(unreal.AnimPoseExtensions.get_bone_pose(pose, bone, unreal.AnimPoseSpaces.WORLD).translation)
            for bone in ['Hips', 'Head', 'LeftUpLeg', 'LeftLeg', 'LeftFoot', 'RightUpLeg', 'RightLeg', 'RightFoot',
                         'LeftArm', 'LeftForeArm', 'LeftHand', 'RightArm', 'RightForeArm', 'RightHand']}


def create_ride_pose(calibration=None):
    existing = unreal.load_asset(RIDE)
    if existing and calibration is None:
        return existing
    mesh = load(BOY)
    modifier = unreal.SkeletonModifier()
    assert modifier.set_skeletal_mesh(mesh)

    def aim(bone, child, desired):
        world = modifier.get_bone_transform(bone, True)
        end = modifier.get_bone_transform(child, True).translation
        delta = unreal.MathLibrary.quat_find_between_vectors(end - world.translation, unreal.Vector(*desired))
        parent = modifier.get_parent_name(bone)
        parent_rotation = modifier.get_bone_transform(parent, True).rotation
        local = modifier.get_bone_transform(bone, False)
        local.rotation = parent_rotation.inversed() * (delta * world.rotation)
        assert modifier.set_bone_transform(bone, local, True)

    # 只在临时骨架副本调整方向，保留源骨长和缩放；最终写入独立动画的本地骨轨道
    def solve_limb(upper, lower, end, target, pole):
        start = modifier.get_bone_transform(upper, True).translation
        joint = modifier.get_bone_transform(lower, True).translation
        tip = modifier.get_bone_transform(end, True).translation
        length1, length2 = (joint - start).length(), (tip - joint).length()
        offset = target - start
        distance = offset.length()
        if not abs(length1 - length2) + .01 < distance < length1 + length2 - .01:
            raise RuntimeError('原骨长无法接触车辆挂点：' + end + ' distance=' + str(distance) + ' lengths=' + str((length1, length2)))
        direction = offset / distance
        pole = unreal.Vector(*pole)
        bend = pole - direction * sum(a * b for a, b in zip(xyz(pole), xyz(direction)))
        bend = bend / bend.length()
        forward = (length1 * length1 - length2 * length2 + distance * distance) / (2 * distance)
        height = math.sqrt(max(0, length1 * length1 - forward * forward))
        elbow = start + direction * forward + bend * height
        aim(upper, lower, xyz(elbow - start))
        aim(lower, end, xyz(target - modifier.get_bone_transform(lower, True).translation))

    hip = modifier.get_bone_transform('Hips', True).translation
    for side, sign in [('Left', -1), ('Right', 1)]:
        if calibration:
            seat = unreal.Vector(*calibration['rider_pivot'])
            hand = hip + unreal.Vector(*calibration[side.lower() + '_hand_grip']) - seat
            foot = hip + unreal.Vector(*calibration[side.lower() + '_foot_peg']) - seat
            solve_limb(side + 'UpLeg', side + 'Leg', side + 'Foot', foot, (1, 0, 0))
            solve_limb(side + 'Arm', side + 'ForeArm', side + 'Hand', hand, (0, 0, -1))
            aim(side + 'Hand', side + 'HandEnd', (1, 0, 0))
        else:
            aim(side + 'UpLeg', side + 'Leg', (1, .26 * sign, -.18))
            aim(side + 'Leg', side + 'Foot', (.12, .08 * sign, -1))
            aim(side + 'Arm', side + 'ForeArm', (.75, .10 * sign, -.66))
            aim(side + 'ForeArm', side + 'Hand', (1, -.06 * sign, .12))
        aim(side + 'Foot', side + 'ToeBase', (1, 0, 0))
    factory = unreal.AnimSequenceFactory()
    factory.set_editor_property('target_skeleton', mesh.get_editor_property('skeleton'))
    factory.set_editor_property('preview_skeletal_mesh', mesh)
    folder, name = RIDE.rsplit('/', 1)
    animation = existing or unreal.AssetToolsHelpers.get_asset_tools().create_asset(name, folder, unreal.AnimSequence, factory)
    if not animation:
        raise RuntimeError('坐姿动画创建失败')
    controller = animation.get_editor_property('controller')
    controller.open_bracket('二狗独立骑乘姿势', False)
    try:
        controller.remove_all_bone_tracks(False)
        controller.set_frame_rate(unreal.FrameRate(30, 1), False)
        controller.set_number_of_frames(unreal.FrameNumber(30), False)
        for bone in modifier.get_all_bone_names():
            transform = modifier.get_bone_transform(bone, False)
            controller.add_bone_curve(bone, False)
            if not controller.set_bone_track_keys(bone, [transform.translation] * 31,
                                                   [transform.rotation] * 31, [transform.scale3d] * 31, False):
                raise RuntimeError('写入骑乘骨轨道失败：' + str(bone))
    finally:
        controller.close_bracket(False)
    animation.set_editor_property('enable_root_motion', False)
    animation.set_retarget_source_asset(mesh)
    unreal.EditorAssetLibrary.set_metadata_tag(animation, 'DeliveryCompanionRide', 'ReferenceBoneLengths;SeatedPose;v1')
    save(animation)
    return animation


def calibrate_vehicle():
    data = inspect_vehicle()['points_in_mesh_root']
    # 二狗手臂短于玩家骑手，臀部在现有座垫上前移 3 cm，不拉伸骨骼或放大人物
    data['rider_pivot'][0] += 3
    create_ride_pose(data)
    bp = load(BIKE)
    from editor_toolset.toolsets.blueprint import BlueprintTools
    cdo = unreal.get_default_object(bp.generated_class())
    cdo.get_editor_property('companion_seat').set_relative_location(unreal.Vector(*data['rider_pivot']), False, False)
    cdo.get_editor_property('companion_cargo_anchor').set_relative_location(unreal.Vector(*data['cargo_pivot']), False, False)
    BlueprintTools.compile_blueprint(bp)
    save(bp)
    actors = unreal.get_editor_subsystem(unreal.EditorActorSubsystem).get_all_level_actors()
    bike = next(a for a in actors if isinstance(a, unreal.DeliveryCompanionMotorbike))
    bike.get_editor_property('companion_seat').set_relative_location(unreal.Vector(*data['rider_pivot']), False, False)
    bike.get_editor_property('companion_cargo_anchor').set_relative_location(unreal.Vector(*data['cargo_pivot']), False, False)
    if not unreal.get_editor_subsystem(unreal.LevelEditorSubsystem).save_current_level():
        raise RuntimeError('校准后地图保存失败')


def setup_vehicle(backup):
    from editor_toolset.toolsets.blueprint import BlueprintTools
    mesh, animation = load(BOY), load(RIDE)
    native = unreal.load_class(None, '/Script/Delivery.DeliveryCompanionMotorbike')
    if not native:
        raise RuntimeError('必须先完整编译同伴车辆类')
    bp = unreal.load_asset(BIKE)
    created = not bp
    if not bp:
        bp = unreal.EditorAssetLibrary.duplicate_asset(SOURCE_BIKE, BIKE)
        unreal.BlueprintEditorLibrary.reparent_blueprint(bp, native)
        BlueprintTools.compile_blueprint(bp)
    cdo = unreal.get_default_object(bp.generated_class())
    if not isinstance(cdo, unreal.DeliveryCompanionMotorbike):
        raise RuntimeError('已有同名车蓝图父类不符，保留原资产')
    if created:
        cdo.set_editor_property('ride_animation', animation)
        source = unreal.get_default_object(load(ROOT + '/BP_Companion_Ergou').generated_class()).get_editor_property('mesh')
        hip = pose_positions(animation, mesh)['Hips']
        source_transform = source.get_relative_transform()
        scaled_hip = unreal.Vector(*(hip[i] * xyz(source_transform.scale3d)[i] for i in range(3)))
        desired = source_transform.rotation.rotate_vector(scaled_hip) * -1
        cdo.set_editor_property('ride_mesh_offset', desired - source_transform.translation)
        cdo.set_editor_property('auto_possess_ai', unreal.AutoPossessAI.DISABLED)
        cdo.set_editor_property('auto_possess_player', unreal.AutoReceiveInput.DISABLED)
        unreal.EditorAssetLibrary.set_metadata_tag(bp, 'DeliveryCompanionVehicle', 'Ergou;OwnVehicle;v1')
        BlueprintTools.compile_blueprint(bp)
        save(bp)
    level = unreal.get_editor_subsystem(unreal.LevelEditorSubsystem)
    world = unreal.EditorLoadingAndSavingUtils.load_map(MAP)
    actor_system = unreal.get_editor_subsystem(unreal.EditorActorSubsystem)
    actors = actor_system.get_all_level_actors()
    companions = [a for a in actors if a.get_class() == load(ROOT + '/BP_Companion_Ergou').generated_class()]
    if len(companions) != 1:
        raise RuntimeError('二狗实例不唯一')
    bikes = [a for a in actors if isinstance(a, unreal.DeliveryCompanionMotorbike)]
    if len(bikes) > 1:
        raise RuntimeError('已有多辆同伴车，停止自动绑定')
    if bikes:
        bike = bikes[0]
    else:
        candidate = unreal.Vector(-5510, -20020, 3010)
        hit = unreal.SystemLibrary.line_trace_single(world, candidate + unreal.Vector(0, 0, 200),
            candidate - unreal.Vector(0, 0, 500), unreal.TraceTypeQuery.TRACE_TYPE_QUERY1,
            False, [], unreal.DrawDebugTrace.NONE, True)
        if not hit or not hit.to_dict().get('blocking_hit') or hit.to_dict()['impact_normal'].z < .9:
            raise RuntimeError('专属车停车点没有平缓地面，停止生成')
        position = hit.to_dict()['impact_point'] + unreal.Vector(0, 0, cdo.get_editor_property('hover_height'))
        bike = actor_system.spawn_actor_from_class(bp.generated_class(), position,
                                                    unreal.Rotator(pitch=0, yaw=155, roll=0))
        bike.set_actor_label('Vehicle_Companion_Ergou')
        bike.set_editor_property('tags', ['DeliveryCompanionVehicle'])
    component = companions[0].get_component_by_class(unreal.DeliveryCompanionComponent)
    if component.get_editor_property('errand_vehicle') not in (None, bike):
        raise RuntimeError('二狗已有其他专属车引用，保留人工配置')
    component.set_editor_property('errand_vehicle', bike)
    if not level.save_current_level():
        raise RuntimeError('地图保存失败')
    return created


def inspect_vehicle():
    bp = load(BIKE)
    cdo = unreal.get_default_object(bp.generated_class())
    if not isinstance(cdo, unreal.DeliveryCompanionMotorbike):
        raise RuntimeError('专属车没有使用服务器同伴车辆类')
    if cdo.get_editor_property('ride_animation') != load(RIDE):
        raise RuntimeError('专属车坐姿绑定丢失')
    unreal.EditorLoadingAndSavingUtils.load_map(MAP)
    actors = unreal.get_editor_subsystem(unreal.EditorActorSubsystem).get_all_level_actors()
    bikes = [a for a in actors if isinstance(a, unreal.DeliveryCompanionMotorbike)]
    companions = [a for a in actors if a.get_class() == load(ROOT + '/BP_Companion_Ergou').generated_class()]
    assert len(bikes) == len(companions) == 1
    bike, companion = bikes[0], companions[0]
    assert companion.get_component_by_class(unreal.DeliveryCompanionComponent).get_editor_property('errand_vehicle') == bike
    result = {'blueprint': bp.get_path_name(), 'vehicle': bike.get_path_name(),
              'companion': companion.get_path_name(), 'location': xyz(bike.get_actor_location()),
              'ride_mesh_offset': xyz(bike.get_editor_property('ride_mesh_offset')),
              'seat': str(bike.get_editor_property('companion_seat').get_relative_transform()),
              'seat_location': xyz(bike.get_editor_property('companion_seat').get_relative_transform().translation),
              'native_rider_pivot': xyz(bike.get_editor_property('rider_pivot_location')),
              'mesh_root': str(bike.get_editor_property('mesh_root').get_relative_transform()),
              'mesh_align': str(bike.get_editor_property('mesh_align').get_world_transform()),
              'left_hand_grip': xyz(bike.get_editor_property('left_hand_grip').get_world_location()),
              'right_hand_grip': xyz(bike.get_editor_property('right_hand_grip').get_world_location()),
              'collision_extent': xyz(bike.get_editor_property('collision_box').get_scaled_box_extent())}
    root_transform = bike.get_editor_property('mesh_root').get_world_transform()
    result['points_in_mesh_root'] = {}
    for name in ['rider_pivot', 'left_hand_grip', 'right_hand_grip', 'left_foot_peg', 'right_foot_peg', 'cargo_pivot']:
        position = bike.get_editor_property(name).get_world_location()
        result['points_in_mesh_root'][name] = xyz(unreal.MathLibrary.inverse_transform_location(root_transform, position))
    return result


def main():
    if unreal.get_editor_subsystem(unreal.UnrealEditorSubsystem).get_game_world():
        raise RuntimeError('只允许在编辑器静态世界执行')
    command = unreal.SystemLibrary.get_command_line()
    project = Path(unreal.Paths.project_dir()).resolve()
    output = project / 'Saved/AIResearch/CompanionVehicle'
    output.mkdir(parents=True, exist_ok=True)
    if '-CreateCompanionRidePose' in command:
        create_ride_pose()
    if '-SetupCompanionVehicle' in command:
        backup = output / datetime.now().strftime('%Y%m%d-%H%M%S')
        backup.mkdir()
        for asset in [BIKE, MAP]:
            src = project / 'Content' / (asset.removeprefix('/Game/') + ('.umap' if asset == MAP else '.uasset'))
            if src.exists():
                shutil.copy2(src, backup / src.name)
        if setup_vehicle(backup):
            calibrate_vehicle()
    if '-CalibrateCompanionVehicle' in command:
        backup = output / ('calibration-' + datetime.now().strftime('%Y%m%d-%H%M%S'))
        backup.mkdir()
        for asset in [RIDE, BIKE, MAP]:
            src = project / 'Content' / (asset.removeprefix('/Game/') + ('.umap' if asset == MAP else '.uasset'))
            if src.exists():
                shutil.copy2(src, backup / src.name)
        calibrate_vehicle()
    animation = load(RIDE)
    result = {'ride': animation.get_path_name(), 'duration': animation.get_play_length(),
              'positions': pose_positions(animation, load(BOY)), 'root_motion': animation.get_editor_property('enable_root_motion')}
    assert animation.get_editor_property('skeleton') == load(BOY).get_editor_property('skeleton')
    assert not result['root_motion'] and abs(result['duration'] - 1) < .001
    if unreal.EditorAssetLibrary.does_asset_exist(BIKE):
        result['vehicle'] = inspect_vehicle()
        points = result['vehicle']['points_in_mesh_root']
        seat = result['vehicle']['seat_location']
        positions = result['positions']
        result['contact_errors_cm'] = {}
        for bone, anchor in [('LeftHand', 'left_hand_grip'), ('RightHand', 'right_hand_grip'),
                             ('LeftFoot', 'left_foot_peg'), ('RightFoot', 'right_foot_peg')]:
            delta = [positions[bone][i] - positions['Hips'][i] + seat[i] - points[anchor][i] for i in range(3)]
            result['contact_errors_cm'][bone] = math.sqrt(sum(value * value for value in delta))
        # 压缩后骨骼点仍有约 2–3 cm 偏差，报告实际数值；手掌、鞋面接触还需画面检查
        result['contact_note'] = '静态骨骼点误差，不代表动态转向时手掌/鞋面已经验收'
        assert max(result['contact_errors_cm'].values()) < 4, result['contact_errors_cm']
    (output / 'assets.json').write_text(json.dumps(result, ensure_ascii=False, indent=2), encoding='utf-8')
    print('Companion vehicle asset inspection complete: ' + str(output / 'assets.json'))


if __name__ == '__main__':
    main()
