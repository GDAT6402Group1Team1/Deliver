# -*- coding: utf-8 -*-
"""查"脚下这块地"的碰撞设置：为什么射线打得到、人却往下掉。

起因：TestForCharacter 里角色一出生就穿到地底下，而摩托车放置脚本的日志显示
地形（Landscape, Z=3097）**对 Visibility 通道是 Ignore**、只有 WorldStatic 查询才探得到。
两件事很可能是同一个病灶：那块地的碰撞被改过。

要分清的是两个**互相独立**的设置，它们坏的表现完全不同：

  * Collision Enabled —— Query Only 时射线照样打得到，但**模拟中的刚体会直接穿过去**。
    角色是全身常驻物理的 19 根刚体，靠物理碰撞站着，所以这一项一关人就坠穿，
    而任何射线诊断都看不出异常（这正是最迷惑人的地方）。
  * 各通道的 Response —— 某个通道是 Ignore 时，只有走那个通道的查询会落空。
    摩托车贴地走 ECC_Visibility，角色布娃娃走 WorldStatic/WorldDynamic 对象查询，
    所以"车飘着、人站得住"和"人坠穿、车正常"是两种不同的坏法。

只读，不改任何东西。结果写到 Saved/diag_ground_collision.txt。
用法：py diag_ground_collision.py
     想查别的位置：改 PROBE_XY，或者 USE_PLAYER_START=False 用视口镜头的 XY。
"""

import math
import traceback

import unreal

USE_PLAYER_START = True     # True: 用关卡里第一个 PlayerStart 的 XY
PROBE_XY = None             # 形如 (2630.0, 4390.0)，设了就优先用它
TRACE_UP = 2000.0
TRACE_DOWN = 5000.0

OUT = unreal.Paths.project_saved_dir() + "diag_ground_collision.txt"
lines = []


def w(s=""):
    lines.append(str(s))
    unreal.log("[ground] %s" % s)


def flush():
    try:
        with open(OUT, "w", encoding="utf-8") as fh:
            fh.write("\n".join(lines))
    except Exception:
        pass


def _enum(path, name, fallback=None):
    """取枚举值。取不到就返回 fallback，不让整个脚本因为一个名字挂掉
    （unreal.ObjectRedirector 那次就是直接 AttributeError 断在第一行）。"""
    holder = getattr(unreal, path, None)
    return getattr(holder, name, fallback) if holder else fallback


# 要逐个报告的通道。前两个决定角色站不站得住，Visibility 决定摩托车贴不贴得到。
CHANNELS = [
    ("WorldStatic", _enum("CollisionChannel", "ECC_WORLD_STATIC")),
    ("WorldDynamic", _enum("CollisionChannel", "ECC_WORLD_DYNAMIC")),
    ("Pawn", _enum("CollisionChannel", "ECC_PAWN")),
    ("PhysicsBody", _enum("CollisionChannel", "ECC_PHYSICS_BODY")),
    ("Visibility", _enum("CollisionChannel", "ECC_VISIBILITY")),
    ("Camera", _enum("CollisionChannel", "ECC_CAMERA")),
    ("Vehicle", _enum("CollisionChannel", "ECC_VEHICLE")),
]


def probe_xy(world):
    if PROBE_XY:
        return PROBE_XY[0], PROBE_XY[1], 0.0, u"PROBE_XY"
    eas = unreal.get_editor_subsystem(unreal.EditorActorSubsystem)
    if USE_PLAYER_START:
        for a in eas.get_all_level_actors():
            if isinstance(a, unreal.PlayerStart):
                loc = a.get_actor_location()
                return loc.x, loc.y, loc.z, u"PlayerStart %s" % a.get_actor_label()
    ues = unreal.get_editor_subsystem(unreal.UnrealEditorSubsystem)
    loc, _ = ues.get_level_viewport_camera_info()
    return loc.x, loc.y, loc.z, u"视口镜头"


def trace_objects(world, x, y, z):
    """用对象查询往下扫，返回 [(高度, actor), ...]。对象查询比通道查询更"广"，
    Visibility 被关掉的地面也照样能扫到，正适合找"到底有没有地"。"""
    types = []
    for name in ("OBJECT_TYPE_QUERY1", "OBJECT_TYPE_QUERY2"):   # WorldStatic / WorldDynamic
        v = _enum("ObjectTypeQuery", name)
        if v is not None:
            types.append(v)
    res = unreal.SystemLibrary.line_trace_multi_for_objects(
        world, unreal.Vector(x, y, z + TRACE_UP), unreal.Vector(x, y, z - TRACE_DOWN),
        types, False, [], unreal.DrawDebugTrace.NONE, True)
    hits = res[-1] if isinstance(res, tuple) else res
    out = []
    for hit in (hits or []):
        try:
            d = hit.to_dict()
        except Exception:
            continue
        pt = d.get("impact_point") or d.get("location")
        actor = d.get("hit_actor")
        comp = d.get("hit_component")
        if pt is not None and actor is not None:
            out.append((pt.z, actor, comp))
    return out


def _comp_line(comp):
    """一个组件的碰撞设置，压成一行，方便做分组统计。"""
    try:
        enabled = str(comp.get_collision_enabled()).split(":")[0].split(".")[-1]
        profile = str(comp.get_collision_profile_name())
        objtype = str(comp.get_collision_object_type()).split(":")[0].split(".")[-1]
    except Exception as exc:
        return u"读不到（%s）" % exc
    parts = []
    for label, ch in CHANNELS:
        if ch is None:
            continue
        try:
            r = str(comp.get_collision_response_to_channel(ch)).split(":")[0].split(".")[-1]
        except Exception:
            r = u"?"
        parts.append(u"%s=%s" % (label, r.replace("ECR_", "")))
    return u"%s | %s | %s | %s" % (enabled, profile, objtype, u" ".join(parts))


def verdict(comp):
    try:
        name = str(comp.get_collision_enabled())
    except Exception:
        return
    if "NO_COLLISION" in name:
        w(u"       ★ 完全没有碰撞：射线打不到，人也站不住。")
    elif "QUERY_ONLY" in name:
        w(u"       ★ **Query Only：射线打得到，但模拟中的刚体会直接穿过去。**")
        w(u"          角色是全身物理刚体，这一项就足以让人坠穿地面。")
    elif "PHYSICS_ONLY" in name:
        w(u"       ★ Physics Only：人站得住，但所有射线（含摩托车贴地）都探空。")


def describe_hit(z, actor, comp):
    """报告**射线真正命中的那个组件**，不抽样——1898 个组件里抽 4 个等于没查。"""
    w(u"")
    w(u"── Z=%.0f 命中 %s（%s）" % (z, actor.get_actor_label(), actor.get_class().get_name()))
    if comp is None:
        w(u"   命中结果里没有组件信息。")
        return
    w(u"   命中组件：%s [%s]" % (comp.get_name(), comp.get_class().get_name()))
    w(u"   %s" % _comp_line(comp))
    verdict(comp)


def summarize(actor):
    """把这个 actor 的所有碰撞组件按配置分组。数量最多的那组是"正常"，
    落单的那几个往往就是病灶——1898 个组件靠肉眼翻是翻不出来的。"""
    prims = actor.get_components_by_class(unreal.PrimitiveComponent)
    if not prims:
        w(u"   没有任何 PrimitiveComponent。")
        return
    groups = {}
    for comp in prims:
        key = u"%s :: %s" % (comp.get_class().get_name(), _comp_line(comp))
        groups.setdefault(key, []).append(comp.get_name())
    w(u"")
    w(u"   %s 共 %d 个碰撞组件，按配置分成 %d 组："
      % (actor.get_actor_label(), len(prims), len(groups)))
    for key, names in sorted(groups.items(), key=lambda kv: -len(kv[1])):
        w(u"     [%d 个] %s" % (len(names), key))
        if len(names) <= 6:
            w(u"        %s" % u", ".join(names))


# 角色胶囊尺寸，和 DeliveryCharacter.cpp 的 InitCapsuleSize(42, 96) 一致。
# 检测必须用角色自己的尺寸，用别的值等于没检测。
CAPSULE_RADIUS = 42.0
CAPSULE_HALF_HEIGHT = 96.0


def check_spawn_blocked(world, x, y, z):
    """按角色胶囊查出生点被什么挡住。

    为什么单靠射线不够：射线是无限细的，可能正好从两堵墙的缝里穿过去，
    而 42cm 半径的胶囊会撞上旁边的东西。APawn 默认的生成策略是
    AdjustIfPossibleButDontSpawnIfColliding——挪不开就干脆不生成，
    日志里只会写一句 "SpawnActor failed because of collision"，不告诉你是谁挡的。
    """
    types = []
    for i in range(1, 8):
        v = _enum("ObjectTypeQuery", "OBJECT_TYPE_QUERY%d" % i)
        if v is not None:
            types.append(v)
    w(u"")
    w(u"=== 出生点胶囊重叠检测（半径 %.0f 半高 %.0f）===" % (CAPSULE_RADIUS, CAPSULE_HALF_HEIGHT))
    for label, dz in ((u"出生点原高度", 0.0), (u"抬高 100", 100.0), (u"抬高 300", 300.0)):
        try:
            res = unreal.SystemLibrary.capsule_overlap_actors(
                world, unreal.Vector(x, y, z + dz),
                CAPSULE_RADIUS, CAPSULE_HALF_HEIGHT, types, None, [])
            actors = res[-1] if isinstance(res, tuple) else res
        except Exception as exc:
            w(u"  %s：检测失败（%s）" % (label, exc))
            continue
        names = sorted({a.get_actor_label() for a in (actors or [])})
        if names:
            w(u"  %s (Z=%.0f)：压着 %d 个 —— %s"
              % (label, z + dz, len(names), u"、".join(names[:8])))
            # 直接问"哪个**组件**重叠了"，而不是按 actor 标签去查。
            # 上一版按标签打包围盒，结果打出来的中心离出生点 150 米——
            # 因为 UE 里 actor 标签**不唯一**，同名的另一个 actor 被查了。
            # 组件级查询没有这个歧义，顺带把组件自己的位置和包围盒一并打出来。
            try:
                cres = unreal.SystemLibrary.capsule_overlap_components(
                    world, unreal.Vector(x, y, z + dz),
                    CAPSULE_RADIUS, CAPSULE_HALF_HEIGHT, types, None, [])
                comps = cres[-1] if isinstance(cres, tuple) else cres
            except Exception as exc:
                w(u"      组件级重叠查询失败（%s）" % exc)
                comps = []
            for c in (comps or []):
                owner = c.get_owner()
                try:
                    cl = c.k2_get_component_location()
                except Exception:
                    cl = None
                w(u"      重叠组件 %s [%s]" % (c.get_name(), c.get_class().get_name()))
                w(u"        属于 %s" % (owner.get_path_name() if owner else u"?"))
                if cl is not None:
                    w(u"        组件位置 (%.0f, %.0f, %.0f)" % (cl.x, cl.y, cl.z))
                if owner is not None:
                    try:
                        al = owner.get_actor_location()
                        origin, extent = owner.get_actor_bounds(True)
                        w(u"        actor 位置 (%.0f, %.0f, %.0f)；碰撞包围盒中心 (%.0f, %.0f, %.0f) 半尺寸 (%.0f, %.0f, %.0f)"
                          % (al.x, al.y, al.z, origin.x, origin.y, origin.z,
                             extent.x, extent.y, extent.z))
                    except Exception as exc:
                        w(u"        actor 信息读不到（%s）" % exc)
                w(u"        %s" % _comp_line(c))
        else:
            w(u"  %s (Z=%.0f)：干净，这个高度能生成。" % (label, z + dz))


SEARCH_RINGS = (300.0, 600.0, 900.0, 1400.0, 2000.0)   # 向外找几圈
SEARCH_DIRS = 12                                        # 每圈取几个方向


def find_clear_spot(world, x, y, z):
    """在出生点周围一圈圈往外找"地面之上、胶囊放得下"的位置。

    只报坐标，不改关卡——挪 PlayerStart 要动 62MB 的共享 umap，
    那是队友也在改的文件，值不值得改由人来定。
    """
    types = []
    for i in range(1, 8):
        v = _enum("ObjectTypeQuery", "OBJECT_TYPE_QUERY%d" % i)
        if v is not None:
            types.append(v)

    def clear_at(px, py, pz):
        try:
            res = unreal.SystemLibrary.capsule_overlap_actors(
                world, unreal.Vector(px, py, pz),
                CAPSULE_RADIUS, CAPSULE_HALF_HEIGHT, types, None, [])
            actors = res[-1] if isinstance(res, tuple) else res
            return not actors, sorted({a.get_actor_label() for a in (actors or [])})
        except Exception:
            return False, [u"检测失败"]

    w(u"")
    w(u"=== 找一个能生成的位置 ===")
    found = []
    blocker_count = {}
    for radius in SEARCH_RINGS:
        for i in range(SEARCH_DIRS):
            ang = 2.0 * math.pi * i / SEARCH_DIRS
            px = x + radius * math.cos(ang)
            py = y + radius * math.sin(ang)
            hits = trace_objects(world, px, py, z)
            below = [(h, a) for h, a, _c in hits if h <= z + 200.0]
            if not below:
                continue
            # 每一层都试，不只试最高那层：最高的往往是楼的内部楼板，
            # 站上去照样在楼的碰撞体里，真正的地面是更低的那层。
            for gz, ground_actor in sorted(below, key=lambda t: -t[0]):
                pz = gz + CAPSULE_HALF_HEIGHT + 10.0
                ok, blockers = clear_at(px, py, pz)
                if ok:
                    found.append((radius, px, py, pz, ground_actor.get_actor_label()))
                    break
                for b in blockers:
                    blocker_count[b] = blocker_count.get(b, 0) + 1
            if found:
                break
        if found:
            break

    if not found:
        w(u"  向外找了 %.0f 米都没找到干净位置。" % (max(SEARCH_RINGS) / 100.0))
        if blocker_count:
            top = sorted(blocker_count.items(), key=lambda kv: -kv[1])
            w(u"  挡路者统计（次数从多到少）：")
            for name, n in top[:8]:
                w(u"     %-30s %d 次" % (name, n))
            w(u"  如果全是同一个 actor，那多半是它的碰撞体大得离谱，而不是位置选得不好。")
        return
    radius, px, py, pz, ground_name = found[0]
    w(u"  距出生点 %.0fcm 处可用：(%.0f, %.0f, %.0f)，站在 %s 上。"
      % (radius, px, py, pz, ground_name))
    w(u"  用法二选一：")
    w(u"    · 把 PlayerStart 挪到这个坐标（会改动 TestForCharacter.umap，62MB 共享大图）")
    w(u"    · 或者把编辑器视口镜头停在这附近再按 Play")
    w(u"      （当前 Play 设置是「在当前相机位置生成玩家」，不看 PlayerStart）")


def run():
    world = unreal.get_editor_subsystem(unreal.UnrealEditorSubsystem).get_editor_world()
    if world is None:
        w(u"！拿不到编辑器世界。")
        return
    w(u"=== 脚下地面碰撞诊断 ===")
    w(u"关卡：%s" % world.get_name())

    x, y, z, who = probe_xy(world)
    w(u"探测点：(%.0f, %.0f)，参考高度 %.0f，来自 %s" % (x, y, z, who))

    hits = trace_objects(world, x, y, z)
    if not hits:
        w(u"！这条竖线上 WorldStatic/WorldDynamic 一个都没扫到——这个位置下方是真的空的。")
        flush()
        return

    w(u"对象查询扫到 %d 层（由上到下）：" % len(hits))
    for h, a, c in sorted(hits, key=lambda t: -t[0]):
        w(u"   Z=%8.0f  %s / %s" % (h, a.get_actor_label(), c.get_name() if c else u"?"))

    for h, actor, comp in sorted(hits, key=lambda t: -t[0]):
        describe_hit(h, actor, comp)

    seen = set()
    for _, actor, _c in sorted(hits, key=lambda t: -t[0]):
        key = actor.get_actor_label()
        if key in seen:
            continue
        seen.add(key)
        summarize(actor)

    check_spawn_blocked(world, x, y, z)
    find_clear_spot(world, x, y, z)

    w(u"")
    w(u"对照：角色布娃娃贴地走 WorldStatic/WorldDynamic 对象查询，")
    w(u"      摩托车贴地走 ECC_Visibility 通道射线（DeliveryMotorbike.cpp:1159），")
    w(u"      而角色能不能站住看的是**物理**碰撞，和上面两种射线都无关。")


try:
    run()
except Exception:
    err = traceback.format_exc()
    unreal.log_error("[ground] fail:" + chr(10) + err)
    lines.append(u"")
    lines.append(u"！中途异常：")
    lines.append(err)
finally:
    flush()
    unreal.log("[ground] 写入 %s" % OUT)
