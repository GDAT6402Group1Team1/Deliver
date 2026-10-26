# -*- coding: utf-8 -*-
"""PIE/Simulate 期间采样 I08 路口的灯态和周围车的状态，看车到底在等什么。

为什么必须看运行时
------------------
`diag_stuck_intersection.py` 已经排除了静态原因（盒子罩住了、只有一个路口 actor、
端点切线正常、进得来出得去）。剩下的唯一嫌疑是灯态：那四段 `Inter_` 的
`LightNumber` **全是 0**（交叉路没有交通线，是个"只有一条路的路口"），
而车刹停后靠 `PollForGreen` 盯 `StoppedAtIntersection` 的 `StopAtInter`，
变 false 才重新起步。编辑器里读到的 `StopAtInter=True` 是序列化值、
**不等于运行时**，所以只能在跑的时候采。

这里要回答三个问题：
  1. 这四段的 `StopAtInter` 在跑的时候**到底有没有翻过**？
     一直 true → 车永远等不到绿灯，根因就是灯；
     规律地 true/false 交替 → 灯是好的，卡住另有原因。
  2. 卡住的那辆车自己处于什么状态：`CurrentSpeed` 是不是 0、
     `CurrentFollowSpline` 有没有效、`StoppedAtIntersection` 指向谁。
  3. 它等的那个路口段，和它自己记录的灯态是否一致
     （同一路口里**别的路**的 `Inter_` 也能通过 Cast，车可能读到横向车流的灯态）。

**用每帧回调而不是 sleep 轮询**：Python 跑在游戏线程上，轮询会把模拟本身卡死
（这条 CLAUDE.md 记过）。脚本注册回调后立刻返回，到时自己停、自己写报告。

只读：不改任何属性。报告写到 Saved/watch_stuck_intersection.txt
用法：先开 Simulate 或 PIE，再 py watch_stuck_intersection.py，等 DURATION 秒看报告。
"""

import traceback

import unreal

# 锚点取自 diag 报告里 Inter_MainRoad_R010_-0540_I08 的位置
ANCHOR = unreal.Vector(20864.0, -14182.0, 2655.0)
RADIUS = 2500.0          # 锚点多远以内的路口段算这个路口
CAR_RADIUS = 6000.0      # 采样多远以内的车
DURATION = 25.0          # 采样时长（秒）。要盖住好几个 6 秒灯周期
STALL_SPEED = 5.0        # 车速低于此值算"停着"
CHILD_PREFIX = "BP_TrafficLine1_IntersectionChild"
INTER_PREFIX = "BP_Intersection"
CAR_PREFIX = "BP_car_base"

OUT = unreal.Paths.project_saved_dir() + "watch_stuck_intersection.txt"


def prop(obj, name):
    """读蓝图变量。先试原名——蓝图变量要用原名，snake_case 只适用于原生 UPROPERTY。"""
    for n in (name, name.lower()):
        try:
            return obj.get_editor_property(n)
        except Exception:
            continue
    return None


def label_of(a):
    try:
        return a.get_actor_label()
    except Exception:
        return a.get_name()


class Watcher(object):
    def __init__(self, world):
        self.world = world
        self.lines = []
        self.t0 = None
        self.handle = None
        self.segs = []        # [(actor, 名字)]
        self.inter = None
        self.light_log = {}   # 名字 -> [(时间, StopAtInter)] 只记变化
        self.cars = {}        # 名字 -> 采样记录
        self.frames = 0

    def w(self, s=""):
        self.lines.append(str(s))
        unreal.log("[watch] %s" % s)

    def collect(self):
        for a in unreal.GameplayStatics.get_all_actors_of_class(self.world, unreal.Actor):
            cls = a.get_class().get_name()
            d = (a.get_actor_location() - ANCHOR).length()
            if cls.startswith(CHILD_PREFIX) and d < RADIUS:
                self.segs.append(a)
            elif cls.startswith(INTER_PREFIX) and "Child" not in cls and d < RADIUS * 2:
                if self.inter is None:
                    self.inter = a
        self.w(u"路口段 %d 段、路口 actor %s"
               % (len(self.segs), label_of(self.inter) if self.inter else u"没找到"))
        for a in self.segs:
            nm = label_of(a)
            self.w(u"    %-36s 灯组 %s  起始 StopAtInter %s"
                   % (nm, prop(a, "LightNumber"), prop(a, "StopAtInter")))
            self.light_log[nm] = []

    def tick(self, _dt):
        now = self.world.get_time_seconds()
        if self.t0 is None:
            self.t0 = now
        el = now - self.t0
        self.frames += 1

        # 灯态只记变化点，不然 25 秒 * 60 帧全是重复行
        for a in self.segs:
            if not a:
                continue
            nm = label_of(a)
            v = prop(a, "StopAtInter")
            hist = self.light_log.setdefault(nm, [])
            if not hist or hist[-1][1] != v:
                hist.append((el, v))

        # 车：记最低速、停了多久、在等谁
        for c in unreal.GameplayStatics.get_all_actors_of_class(self.world, unreal.Actor):
            if not c.get_class().get_name().startswith(CAR_PREFIX):
                continue
            if (c.get_actor_location() - ANCHOR).length() > CAR_RADIUS:
                continue
            nm = label_of(c)
            spd = prop(c, "CurrentSpeed")
            spd = 0.0 if spd is None else float(spd)
            rec = self.cars.setdefault(nm, {
                "stall": 0.0, "first": el, "last": el, "maxspd": 0.0,
                "waiting": set(), "ownstop": set(), "nospline": 0, "samples": 0})
            rec["samples"] += 1
            rec["last"] = el
            rec["maxspd"] = max(rec["maxspd"], spd)
            if spd < STALL_SPEED:
                rec["stall"] += 1
            si = prop(c, "StoppedAtIntersection")
            rec["waiting"].add(label_of(si) if si else u"None")
            rec["ownstop"].add(str(prop(c, "StopatInter")))
            if not prop(c, "CurrentFollowSpline"):
                rec["nospline"] += 1

        if el >= DURATION:
            self.finish(el)

    def finish(self, el):
        if self.handle is not None:
            unreal.unregister_slate_post_tick_callback(self.handle)
            self.handle = None
        self.w(u"")
        self.w(u"采样结束：%.1f 秒 / %d 帧" % (el, self.frames))

        self.w(u"")
        self.w(u"=" * 72)
        self.w(u"① 四段路口的 StopAtInter 变化（只列变化点）")
        self.w(u"=" * 72)
        for nm, hist in sorted(self.light_log.items()):
            if len(hist) <= 1:
                only = hist[0][1] if hist else u"?"
                self.w(u"  %-36s 全程没变过，恒为 %s" % (nm, only))
            else:
                self.w(u"  %-36s 翻了 %d 次：%s" % (nm, len(hist) - 1,
                       u"  ".join(u"%.1fs→%s" % (t, v) for t, v in hist[:10])))
        allstuck = all(len(h) <= 1 and (h and h[0][1]) for h in self.light_log.values())
        if allstuck:
            self.w(u"")
            self.w(u"  ！四段全程恒为 True = 这个路口从来不放绿灯。")
            self.w(u"    车刹停后 PollForGreen 会一直等下去，这就是卡死的直接原因。")
            self.w(u"    下一步看 BP_Intersection 怎么决定放行（dump 它的图表），")
            self.w(u"    重点是「只有一个灯组时它会不会永远等另一组」。")

        self.w(u"")
        self.w(u"=" * 72)
        self.w(u"② 这附近的车（%d 辆）" % len(self.cars))
        self.w(u"=" * 72)
        if not self.cars:
            self.w(u"  一辆都没采到。车不在这附近，或者 %s 不是实际类名。" % CAR_PREFIX)
        for nm, r in sorted(self.cars.items(), key=lambda kv: -kv[1]["stall"]):
            pct = 100.0 * r["stall"] / max(r["samples"], 1)
            self.w(u"  %-28s 停着 %5.1f%% 的时间  峰值速度 %6.0f  无样条 %d 帧"
                   % (nm, pct, r["maxspd"], r["nospline"]))
            self.w(u"      在等：%s     自记灯态：%s"
                   % (u",".join(sorted(r["waiting"]))[:70],
                      u",".join(sorted(r["ownstop"]))))
        self.w(u"")
        self.w(u"  停着接近 100% 且「在等」指向本路口的某段 → 它就是卡在这个灯上。")
        self.w(u"  停着接近 100% 但「在等」是 None → 不是灯的问题，")
        self.w(u"    看是不是前车避让死锁（MaxBlockedTime）或者接不上下一段。")

        try:
            with open(OUT, "w", encoding="utf-8") as fh:
                fh.write("\n".join(self.lines))
            unreal.log("[watch] 写入 %s" % OUT)
        except Exception:
            unreal.log_error("[watch] 写文件失败")

    def start(self):
        self.w(u"=== 路口灯态与车状态采样（只读）===")
        self.w(u"锚点 (%.0f, %.0f)  采样 %.0f 秒" % (ANCHOR.x, ANCHOR.y, DURATION))
        self.collect()
        if not self.segs:
            self.w(u"！锚点附近没找到 %s。" % CHILD_PREFIX)
            self.finish(0.0)
            return
        self.w(u"")
        self.w(u"开始采样，%.0f 秒后自动停止并写报告。期间别停 PIE。" % DURATION)
        self.handle = unreal.register_slate_post_tick_callback(self.tick)


_WATCHER = None


def run():
    global _WATCHER
    world = unreal.get_editor_subsystem(unreal.UnrealEditorSubsystem).get_game_world()
    if world is None:
        unreal.log_error(
            "[watch] 没有在跑的世界——先点 Simulate 或 PIE，再跑这个脚本。")
        return
    _WATCHER = Watcher(world)
    _WATCHER.start()


try:
    run()
except Exception:
    unreal.log_error("[watch] fail:" + chr(10) + traceback.format_exc())
