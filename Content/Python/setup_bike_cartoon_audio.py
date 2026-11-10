# -*- coding: utf-8 -*-
"""生成卡通电瓶车碰撞与持续喇叭音色；编辑器运行时导入并配置，不修改地图"""
import math
import random
import struct
import wave
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
DEST = '/Game/Sound/SFX'

def run(horn_only=False):
    out = ROOT / 'Design/Audio/GeneratedSFX'
    out.mkdir(parents=True, exist_ok=True)
    rng = random.Random(37)
    paths = []
    for name, seconds in [('SFX_Bike_CartoonCrash', .65), ('SFX_Bike_SoftBump', .35), ('SFX_Bike_HornLoop', .4), ('SFX_Punch_NPC', .25)]:
        samples = []
        phase = 0.
        for i in range(int(48000*seconds)):
            t = i/48000
            if 'Horn' in name:
                # 400/500 Hz 双音簧片，固定音高与丰富泛音，0.4 秒整周期闭合
                value = 0.
                for frequency, weight in ((400, .52), (500, .48)):
                    phase = 2*math.pi*frequency*t
                    reed = sum(amplitude*math.sin(harmonic*phase) for harmonic, amplitude in
                               ((1,1.), (2,.52), (3,.30), (4,.15), (5,.08), (6,.04)))
                    value += weight*reed
                value = .86*math.tanh(1.4*value)
            elif 'Crash' in name:
                phase += 2*math.pi*(140+360*math.exp(-t*14))/48000
                value = .33*math.sin(phase)*math.exp(-t*7) + .2*math.sin(2*math.pi*1370*t)*math.exp(-t*18) + rng.uniform(-.16,.16)*math.exp(-t*30)
                value *= min(1,t/.003)
            elif 'Punch' in name:
                phase += 2*math.pi*(170+400*math.exp(-t*35))/48000
                value = (.4*math.sin(phase)+rng.uniform(-.3,.3)*math.exp(-t*35))*math.exp(-t*22)*min(1,t/.002)
            else:
                phase += 2*math.pi*(75+200*math.exp(-t*25))/48000
                value = (.55*math.sin(phase)+rng.uniform(-.08,.08))*math.exp(-t*17)*min(1,t/.003)
            samples.append(max(-.95,min(.95,value)))
        path = out/(name+'.wav')
        with wave.open(str(path),'wb') as w:
            w.setparams((1,2,48000,0,'NONE','not compressed'))
            w.writeframes(struct.pack('<%dh'%len(samples), *(int(x*32767) for x in samples)))
        paths.append(path)
    try:
        import unreal
    except ImportError:
        return
    if horn_only:
        paths = [p for p in paths if 'HornLoop' in p.stem]
    tasks=[]
    for path in paths:
        task=unreal.AssetImportTask()
        for key,val in dict(filename=str(path),destination_path=DEST,destination_name=path.stem,automated=True,replace_existing=True,save=True).items():
            task.set_editor_property(key,val)
        tasks.append(task)
    unreal.AssetToolsHelpers.get_asset_tools().import_asset_tasks(tasks)
    cfg=unreal.load_asset('/Game/Sound/Config/DA_VehicleAudio')
    if not cfg:
        raise RuntimeError('缺少 DA_VehicleAudio')
    if horn_only:
        sound=unreal.load_asset(DEST+'/SFX_Bike_HornLoop')
        sound.set_editor_property('looping',True)
        unreal.EditorAssetLibrary.save_loaded_asset(sound)
        sounds=dict(cfg.get_editor_property('sounds'))
        settings=sounds[unreal.DeliveryEvent.VEHICLE_HORN]
        settings.set_editor_property('volume',1.6)
        settings.set_editor_property('pitch',1.)
        sounds[unreal.DeliveryEvent.VEHICLE_HORN]=settings
        cfg.set_editor_property('sounds',sounds)
        unreal.EditorAssetLibrary.save_loaded_asset(cfg)
        assert abs(cfg.get_editor_property('sounds')[unreal.DeliveryEvent.VEHICLE_HORN].get_editor_property('volume')-1.6)<.001
        unreal.log('[BikeHorn] 双音汽车喇叭已保存，音量1.6，循环开启，回读通过')
        return
    attenuation_asset=unreal.load_asset('/Game/Sound/Config/SA_BikeEffects')
    if not attenuation_asset:
        attenuation_asset=unreal.AssetToolsHelpers.get_asset_tools().create_asset('SA_BikeEffects','/Game/Sound/Config',unreal.SoundAttenuation,unreal.SoundAttenuationFactory())
    attenuation=unreal.SoundAttenuationSettings()
    attenuation.set_editor_property('attenuation_shape_extents',unreal.Vector(300,0,0))
    attenuation.set_editor_property('falloff_distance',1800.)
    attenuation_asset.set_editor_property('attenuation',attenuation)
    unreal.EditorAssetLibrary.save_loaded_asset(attenuation_asset)
    sounds=dict(cfg.get_editor_property('sounds'))
    for event,name in [(unreal.DeliveryEvent.VEHICLE_IMPACTED,'SFX_Bike_CartoonCrash'),(unreal.DeliveryEvent.VEHICLE_HIT_PERSON,'SFX_Bike_SoftBump'),(unreal.DeliveryEvent.VEHICLE_HORN,'SFX_Bike_HornLoop')]:
        sound=unreal.load_asset(DEST+'/'+name)
        sound.set_editor_property('looping', event==unreal.DeliveryEvent.VEHICLE_HORN)
        # 碰撞声近处清楚、远处衰减，不在全地图等音量播放
        sound.set_editor_property('attenuation_settings',attenuation_asset)
        unreal.EditorAssetLibrary.save_loaded_asset(sound)
        settings=unreal.DeliverySoundSettings()
        settings.set_editor_property('sound',sound)
        settings.set_editor_property('spatial',True)
        if event==unreal.DeliveryEvent.VEHICLE_HORN:
            settings.set_editor_property('volume',1.6)
        sounds[event]=settings
    cfg.set_editor_property('sounds',sounds)
    unreal.EditorAssetLibrary.save_loaded_asset(cfg)
    punch = unreal.load_asset(DEST+'/SFX_Punch_NPC')
    punch.set_editor_property('attenuation_settings',attenuation_asset)
    unreal.EditorAssetLibrary.save_loaded_asset(punch)
    bp = unreal.load_asset('/Game/Blueprint/Character/DeliverMan/BP_DeliveryMan')
    if not bp:
        raise RuntimeError('找不到 BP_DeliveryMan')
    cdo=unreal.get_default_object(bp.generated_class())
    component=cdo.get_component_by_class(unreal.DeliveryRagdollCombatComponent)
    component.set_editor_property('punch_npc_hit_sound',punch)
    assert component.get_editor_property('punch_npc_hit_sound')==punch
    unreal.EditorAssetLibrary.save_loaded_asset(bp)
    bike_bp=unreal.load_asset('/Game/Vehicle/Motorbike/BP_Motorbike')
    bike_cdo=unreal.get_default_object(bike_bp.generated_class())
    audio=bike_cdo.get_component_by_class(unreal.DeliveryVehicleAudioComponent)
    if audio.get_editor_property('config') != cfg:
        audio.set_editor_property('config',cfg)
        unreal.EditorAssetLibrary.save_loaded_asset(bike_bp)
    assert audio.get_editor_property('config')==cfg
    assert len({sounds[e].get_editor_property('sound').get_path_name() for e in (unreal.DeliveryEvent.VEHICLE_IMPACTED, unreal.DeliveryEvent.VEHICLE_HIT_PERSON, unreal.DeliveryEvent.VEHICLE_HORN)})==3
    unreal.log('[BikeAudio] 四种卡通音效已绑定且回读通过，未修改地图')

if __name__=='__main__':
    run()
