# -*- coding: utf-8 -*-
u"""摘掉打在现有 Inter_* 路口段上的 Crosswalk tag。

为什么要摘
----------
"这个路口有斑马线"原来记在路口段 actor 的 tag 上。那是**改现有的交通 actor**，
而现有的路口和交通样条线不许碰（改一次就会和队友的地图改动撞上，而 .umap 是
二进制、只能整份取舍）。这个事实已经改存到项目设置
`UDeliveryTrafficSettings.CrosswalkStopPoints`（`Config/DefaultGame.ini`，
**文本、能合并**），所以 tag 不再需要。

只摘 tag，不动样条、不动位置、不删 actor。
"""

import re
import traceback

import unreal

DRY_RUN = False
LEVELS = [u"/Game/Level/TestForCharacter", u"/Game/Level/testfortraffic"]
STOP_TAG = u"Crosswalk"
INTER = re.compile(r'^Inter_(.+)_([+-]\d+)_I\d+$')


def run():
    les = unreal.get_editor_subsystem(unreal.LevelEditorSubsystem)
    eas = unreal.get_editor_subsystem(unreal.EditorActorSubsystem)
    for lv in LEVELS:
        if not les.load_level(lv):
            unreal.log(u"[cleartag] %s 加载不了" % lv)
            continue
        actors = [a for a in eas.get_all_level_actors() if a]
        hit = 0
        for a in actors:
            tags = [str(t) for t in a.tags]
            if STOP_TAG not in tags:
                continue
            if not INTER.match(a.get_actor_label()):
                # 只摘路口段上的；贴花自己带的是 ClaudeGenCrosswalk，不会命中这里
                continue
            if not DRY_RUN:
                a.tags = [unreal.Name(t) for t in tags if t != STOP_TAG]
            hit += 1
        unreal.log(u"[cleartag] %s：摘掉 %d 个 %s tag%s"
                   % (lv, hit, STOP_TAG, u"（DRY_RUN）" if DRY_RUN else u""))
        if hit and not DRY_RUN:
            les.save_current_level()
            unreal.log(u"[cleartag]   已保存关卡")


try:
    run()
except Exception:
    unreal.log_error(traceback.format_exc())
