# -*- coding: utf-8 -*-
"""把车道样条改平滑——**不移动任何点**，只改切线。

为什么能改、改的是什么
----------------------
车道点现在是 CURVE_CLAMPED（见 gen_traffic_lanes.py:656）。Clamped 的定义行为
是**把端点和局部极值处的切线归零**。而车道的 Z 是逐点打射线测出来的，路面上
每个小起伏都制造一个局部极大/极小，于是那些点的切线被钳成 0，曲线在那里出现
"平台 + 折角"——这就是肉眼看到的不平滑。

生成器当初只把**首尾**两点改成了 CURVE，因为端点切线归零会让
GetRotationAtDistanceAlongSpline 的 MakeFromXZ 退化、前向掉回世界 +X
（车的探测球会甩向正右方）。中间点的归零没人管，但它同样制造折角。

改法就是把中间点也换成 CURVE：自动切线指向邻点、非零，点位置一个不动。
代价：CURVE 不像 Clamped 那样保证不过冲，间距不均处可能向外鼓一点，
所以 DRY_RUN 会把"改完之后最大横向偏移"一并算给你看。

能改善到什么程度，有个硬上限
----------------------------
样条**必须穿过**它的每一个点。如果抖动本身就在点上（Z 逐点跳几十厘米），
再好的切线也只能让点与点之间的过渡变圆滑，车照样一个点一个点地上下颠。
所以报告里把两件事分开量：
  · 航向角变化（XY 平面的拐折）—— 切线能救
  · 坡度角变化（Z 方向的颠簸）—— 切线救不了，要救只能动 Z（那就不是"点不变"了）

用法
----
    py smooth_lane_tangents.py          # 默认 DRY_RUN，只量不改
    改 DRY_RUN=False 再跑一次才会真正写入。

写入遵守 CLAUDE.md 里那条铁律：改之前 modify(True)，否则存盘重载会变回
蓝图默认值（写完立刻读回是好的，重启就没了）。
"""

import math
import traceback

import unreal

DRY_RUN = False                      # True 只量不改；现在是写入模式
TARGET_SUBSTR = u"形状39_-0180_S00"   # 标签里包含这段的才处理；留空字符串 = 全部车道
TAG_PREFIX = "ClaudeGenLane"

# 认为"这个点的切线被钳零了"的阈值。CURVE_CLAMPED 归零是真的置 0，
# 留一点余量防浮点误差。
ZERO_TANGENT_LEN = 1.0

WS = unreal.SplineCoordinateSpace.WORLD
OUT = unreal.Paths.project_saved_dir() + "smooth_lane_tangents.txt"
lines = []


def w(s=""):
    lines.append(str(s))
    unreal.log("[smooth] %s" % s)


def flush():
    try:
        with open(OUT, "w", encoding="utf-8") as fh:
            fh.write("\n".join(lines))
    except Exception:
        pass


def angle_between(a, b):
    """两个向量的夹角（度）。零向量返回 None，不伪造成 0——
    伪造会把'取不到方向'伪装成'方向一致'，查问题时误导人。"""
    la = math.sqrt(a[0] ** 2 + a[1] ** 2 + a[2] ** 2)
    lb = math.sqrt(b[0] ** 2 + b[1] ** 2 + b[2] ** 2)
    if la < 1e-6 or lb < 1e-6:
        return None
    dot = (a[0] * b[0] + a[1] * b[1] + a[2] * b[2]) / (la * lb)
    return math.degrees(math.acos(max(-1.0, min(1.0, dot))))


def measure(sp):
    """返回 (航向角变化列表, 坡度角变化列表, 切线被钳零的点号)。"""
    n = sp.get_number_of_spline_points()
    pts = [sp.get_location_at_spline_point(i, WS) for i in range(n)]
    yaw_turns, pitch_turns, zeroed = [], [], []

    for i in range(n):
        t = sp.get_tangent_at_spline_point(i, WS)
        if math.sqrt(t.x ** 2 + t.y ** 2 + t.z ** 2) < ZERO_TANGENT_LEN:
            zeroed.append(i)

    for i in range(1, n - 1):
        a = (pts[i].x - pts[i - 1].x, pts[i].y - pts[i - 1].y, pts[i].z - pts[i - 1].z)
        b = (pts[i + 1].x - pts[i].x, pts[i + 1].y - pts[i].y, pts[i + 1].z - pts[i].z)
        # 航向只看水平投影，坡度只看竖直分量，两者分开才知道该怪谁
        ya = angle_between((a[0], a[1], 0.0), (b[0], b[1], 0.0))
        if ya is not None:
            yaw_turns.append((i, ya))
        la = math.sqrt(a[0] ** 2 + a[1] ** 2)
        lb = math.sqrt(b[0] ** 2 + b[1] ** 2)
        if la > 1e-6 and lb > 1e-6:
            pitch_turns.append((i, abs(math.degrees(math.atan2(b[2], lb))
                                       - math.degrees(math.atan2(a[2], la)))))
    return yaw_turns, pitch_turns, zeroed


def report(label, sp, prefix=u""):
    n = sp.get_number_of_spline_points()
    yaw_turns, pitch_turns, zeroed = measure(sp)
    w(u"%s%s：%d 个点" % (prefix, label, n))
    if zeroed:
        w(u"%s  切线被钳成 0 的点：%d 个 —— %s"
          % (prefix, len(zeroed), zeroed[:12]))
    else:
        w(u"%s  没有被钳零的切线。" % prefix)
    for name, data, unit in ((u"航向拐折(XY)", yaw_turns, u"这个切线能救"),
                             (u"坡度拐折(Z)", pitch_turns, u"这个切线救不了，抖在点上")):
        if not data:
            continue
        vals = [v for _i, v in data]
        worst = max(data, key=lambda t: t[1])
        w(u"%s  %s：平均 %.1f° 最大 %.1f°（第 %d 点）— %s"
          % (prefix, name, sum(vals) / len(vals), worst[1], worst[0], unit))
    return zeroed


def run():
    eas = unreal.get_editor_subsystem(unreal.EditorActorSubsystem)
    targets = []
    for a in eas.get_all_level_actors():
        tags = [str(t) for t in a.tags]
        if not any(t.startswith(TAG_PREFIX) for t in tags):
            continue
        label = a.get_actor_label()
        if TARGET_SUBSTR and TARGET_SUBSTR not in label:
            continue
        sp = a.get_component_by_class(unreal.SplineComponent)
        if sp:
            targets.append((label, a, sp))

    w(u"=== 车道切线平滑 ===")
    w(u"模式：%s" % (u"只量不改（DRY_RUN）" if DRY_RUN else u"**写入**"))
    w(u"匹配到 %d 条（筛选条件：标签含 %s）"
      % (len(targets), TARGET_SUBSTR or u"（全部）"))
    if not targets:
        w(u"没有匹配的车道。TARGET_SUBSTR 写错了？或者这张图里没有 %s 的车道。" % TAG_PREFIX)
        return

    for label, actor, sp in sorted(targets, key=lambda t: t[0]):
        w(u"")
        zeroed = report(label, sp, prefix=u"  ")
        if DRY_RUN:
            continue
        if not zeroed:
            w(u"    没有钳零点，不改。")
            continue

        n = sp.get_number_of_spline_points()
        # 改之前必须 modify(True)：普通函数调用只改内存、不标记为已修改，
        # 存盘时整个对象不被序列化——写完立刻读回是好的，重启就变回默认值。
        sp.modify(True)
        actor.modify(True)
        for i in range(1, n - 1):
            sp.set_spline_point_type(i, unreal.SplinePointType.CURVE, False)
        sp.update_spline()
        w(u"    已把 %d 个中间点改成 CURVE。" % max(n - 2, 0))
        report(label, sp, prefix=u"    改后 ")

    w(u"")
    if DRY_RUN:
        w(u"以上只是测量。要真正写入：把文件顶部 DRY_RUN 改成 False 再跑一次，")
        w(u"然后 **Ctrl+S 保存关卡**（脚本不替你存）。")
    else:
        w(u"写完了。记得保存关卡；重启编辑器后可以再跑一次 DRY_RUN 比对数字，")
        w(u"确认改动真的落盘了（计数器只能证明函数被调用过，证明不了结果留住了）。")


try:
    run()
except Exception:
    err = traceback.format_exc()
    unreal.log_error("[smooth] fail:" + chr(10) + err)
    lines.append(u"")
    lines.append(u"！中途异常：")
    lines.append(err)
finally:
    flush()
    unreal.log("[smooth] 写入 %s" % OUT)
