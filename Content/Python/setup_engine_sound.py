# -*- coding: utf-8 -*-
"""把发动机循环音接到摩托车上，并把三项容易漏的设置一次配好。

在编辑器控制台（Python 模式）跑：
    import importlib, setup_engine_sound as s; importlib.reload(s); s.run()

幂等，可重复跑。

—— 为什么要脚本，不手点 ——

手点漏任何一项的表现都很迷惑，而且三种漏法长得不一样：

  * **忘了勾 `Looping`** → "上车有声音，开着开着安静了"。最容易被当成
    C++ 的音频组件出了问题去查，实际只是 SoundWave 自己放完一遍就停了。
    音乐那边已经踩过一次（见 CLAUDE.md 背景音乐一节）。
  * **忘了填 `EngineLoopSound`** → `UpdateEngineAudio` 里 `!EngineLoopSound`
    直接 return，全程静音、不报错。
  * **变调范围没收窄** → 0.75~1.5 是整整一个八度，加速时"磁带快放"感很重。
    这条和素材好坏无关：真实发动机的谐波随转速走、而车体共振**不动**，
    `SetPitchMultiplier` 把整个频谱一起拉，共振也跟着跑，所以才像快放。
    范围压到半个八度能去掉大半。
"""

import unreal


SOUND_PATH = "/Game/Sound/SFX/Motor_loop"
BIKE_BP_PATH = "/Game/Vehicle/Motorbike/BP_Motorbike"

# 收窄后的变调范围。想更有冲劲就把 MAX 往上提，但别回到 1.5
IDLE_PITCH = 0.85
MAX_PITCH = 1.25


def run():
    wave = unreal.EditorAssetLibrary.load_asset(SOUND_PATH)
    if not wave:
        unreal.log_error("[发动机] 载不到 %s" % SOUND_PATH)

        return

    # —— 1. SoundWave 必须循环 ——
    if not wave.get_editor_property("looping"):
        wave.set_editor_property("looping", True)
        unreal.EditorAssetLibrary.save_loaded_asset(wave, only_if_is_dirty=False)
        unreal.log("[发动机] %s 已设为循环" % SOUND_PATH)
    else:
        unreal.log("[发动机] %s 本来就是循环的" % SOUND_PATH)

    # —— 2. 挂到摩托车蓝图的 CDO 上 ——
    bike_bp = unreal.EditorAssetLibrary.load_asset(BIKE_BP_PATH)
    if not bike_bp:
        unreal.log_error("[发动机] 载不到 %s" % BIKE_BP_PATH)

        return

    # 改默认值要写 CDO，不是写蓝图资产本身
    cdo = unreal.get_default_object(bike_bp.generated_class())
    if not cdo:
        unreal.log_error("[发动机] 拿不到 BP_Motorbike 的 CDO")

        return

    cdo.set_editor_property("engine_loop_sound", wave)
    cdo.set_editor_property("engine_idle_pitch", IDLE_PITCH)
    cdo.set_editor_property("engine_max_pitch", MAX_PITCH)

    unreal.EditorAssetLibrary.save_loaded_asset(bike_bp, only_if_is_dirty=False)

    # —— 3. 回读验证 ——
    #
    # set_editor_property 失败时不一定抛异常（枚举/受保护属性那几次都是静默的），
    # "调用过了"证明不了"写进去了"。回读是唯一的凭据
    back = cdo.get_editor_property("engine_loop_sound")
    unreal.log("[发动机] 回读 EngineLoopSound = %s" % (back.get_name() if back else "<空！>"))
    unreal.log("[发动机] 回读 变调范围 = %.2f ~ %.2f"
               % (cdo.get_editor_property("engine_idle_pitch"),
                  cdo.get_editor_property("engine_max_pitch")))
    unreal.log("[发动机] 回读 Looping = %s" % wave.get_editor_property("looping"))

    if not back:
        unreal.log_error(
            "[发动机] 插槽回读是空的。关卡里已摆的那辆车可能把这个值覆盖过——"
            "选中它在详情面板里看 Motorbike|Audio，有黄色回退箭头就点一下清掉覆盖。")


if __name__ == "__main__":
    run()
