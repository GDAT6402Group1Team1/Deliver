# -*- coding: utf-8 -*-
"""把被切成 _PartNN 的道路样条合并成一条，让交通线生成回到普通流程。

背景：队友把 5 条支路在路口处切成了多段（R024/R035/R036/R038/R039，
形如 BranchRoad_R035_Part01 / _Part02），两段端点之间有 537~1677cm 的
**物理缺口，里面没有任何源样条**。后果有两层：
  * 路口判不出来：被切段是「停在」路口外面而不是穿过去，
    端点到横穿路实测 442~1107cm，而 XY_CROSS 只有 400
  * 车道在切点处断开：缺口远大于 TARGET_GAP(150)，文档记着 250 时车就接不上了

为什么合并源样条、而不是去桥接生成出来的车道（bridge_part_gaps.py 那条路）：
  1. **高度**。桥接车道只能在两端点之间线性插值 Z，而切点正好在路口、
     地面常有起伏（实测源样条 Z 和真实路面差过 −546~+411cm）。
     合并源样条之后走的是生成器自己那套**逐点打射线重测**，这个问题不存在。
  2. 主生成器里那三处特殊逻辑（切点强制路口、PART_INTER_HALF、兄弟段排除）
     全都不再需要——路本来就是连续的，路口自然判得出来。
  3. 桥接车道是后置的，**车道一重新生成端点就挪位、必须跟着重跑**；
     合并是一次性的，之后和普通路没区别。

两条边界，都是本项目实测撞出来的：
  * **不能 spawn 裸 AActor 再加 SplineComponent**。样条的点数据存得住，
    但**组件的存在与否存不住**——实例上加的组件重载后按蓝图 SCS 重建，
    而裸 Actor 没有 SCS，组件直接消失。所以合并产物用
    BP_PS2DEMSplineActor_v3_C（旧道路那个类，两条河还在用它），
    它的 SplineComponent 来自 SCS，一定存得住。
  * **必须 mark_edited**，否则构造脚本一重跑就把点冲回蓝图默认的 2 个点，
    现象是写完立刻读回是好的、存盘重载就变回去。

不删队友的 _Part actor——源数据留着，队友重新同步 landscape spline 时
删掉合并产物重跑即可。改由生成器跳过带 _PartNN 的（road_spline_of 里加一条）。

分段顺序**不只信编号**：编号顺序未必等于几何顺序，猜错的话合出来的路会折回去。
所以每接一段都按端点邻近度决定要不要反向。

tag = ClaudeGenMergedRoad，可重复运行（先删同 tag 的再建）。
中文里一律用「」不用半角引号。

用法：py merge_part_roads.py
"""

import json
import os
import re
import traceback

import unreal

ROAD_FOLDERS = ["PS2DEM/Splines/MainRoad", "PS2DEM/Splines/BranchRoad"]
ROAD_BP = "/Game/PS2DEM/BP_PS2DEMSplineActor_v3"
TAG = "ClaudeGenMergedRoad"
WS = unreal.SplineCoordinateSpace.WORLD

GAP_STEP = 250.0     # 缺口处补点的间距。Z 随便——生成器会逐点打射线重测，
                     # 文档明确写了不能信源样条的 Z。
MAX_GAP = 4000.0     # 两段之间超过这个距离就不接：实测真切点缺口 537~1677，
                     # 而 R036_Part01↔Part03 相距 24915 是一条路的两个远端、
                     # 不是切点。不设这道闸的话会在空地上连出一条直路。

# **备份落成文本、而且进 git。** 合并产物是算出来的，算它依赖队友那些 _Part actor
# 还在；万一分段被删或重新同步掉，光保留关卡里的 actor 没用——actor 跟关卡一起丢，
# 重算也没了依据。所以把合并后的点列表导成 JSON 存进 Design/（这个目录是提交的，
# 和 Tasks.csv / Audio 一样）。选文本不选 .uasset 的理由和项目设置那条一样：
# **本工程被 git pull 的 autostash 冲掉过两次，二进制冲突只能整份取舍、文本能合并。**
BACKUP = unreal.Paths.project_dir() + "Design/MergedRoads.json"

# True = 不读关卡里的 _Part，直接按 BACKUP 里的点重建合并产物。
# 队友把分段删了/重新同步之后合并算不出来时走这条。
RESTORE_FROM_FILE = False

OUT = unreal.Paths.project_saved_dir() + "merge_part_roads.txt"
lines = []


def w(s=""):
    lines.append(str(s))


def base_road_name(label):
    return re.sub(r"_[Pp]art\d+$", "", str(label))


def part_index(label):
    m = re.search(r"_[Pp]art(\d+)$", str(label))
    return int(m.group(1)) if m else None


def dist2(a, b):
    return ((a.x - b.x) ** 2 + (a.y - b.y) ** 2) ** 0.5


def mark_edited(sp):
    for name in ("spline_has_been_edited", "b_spline_has_been_edited"):
        try:
            sp.set_editor_property(name, True)
            return True
        except Exception:
            continue
    return False


def spline_points(sp):
    out = []
    try:
        n = sp.get_number_of_spline_points()
    except Exception:
        return out
    for i in range(n):
        try:
            out.append(sp.get_location_at_spline_point(i, WS))
        except Exception:
            pass
    return out


def build_actor(eas, cls, label, folder, chain):
    """按点列表建一个合并产物 actor。返回点数，失败返回 0。"""
    a = eas.spawn_actor_from_class(cls, chain[0])
    a.set_actor_label(label)
    a.tags = [TAG]
    try:
        # 文件夹必须和分段一致：classify() 判主次路靠文件夹，
        # 放错地方主路会被当成次路（4 车道变 2 车道）。
        a.set_folder_path(folder)
    except Exception as exc:
        w("  !! 设文件夹失败：%s —— 手动把它拖进 %s" % (str(exc)[:60], folder))

    sp = a.get_component_by_class(unreal.SplineComponent)
    if not sp:
        w("  !! 合并产物上没有 SplineComponent，%s 这个类不对" % ROAD_BP)
        return 0
    sp.clear_spline_points(False)
    for p in chain:
        sp.add_spline_point(p, WS, False)
    np_ = sp.get_number_of_spline_points()
    for i in range(np_):
        sp.set_spline_point_type(i, unreal.SplinePointType.CURVE_CLAMPED, False)
    # 首尾用 CURVE：CURVE_CLAMPED 会把端点切线归零，而车的朝向取
    # GetRotationAtDistanceAlongSpline（内部 MakeFromXZ(切线, Up)），
    # 切线为零时前向退化成世界 +X。
    for i in (0, np_ - 1):
        sp.set_spline_point_type(i, unreal.SplinePointType.CURVE, False)
    sp.update_spline()
    if not mark_edited(sp):
        w("  !! %s 的 spline_has_been_edited 没设上，点会被构造脚本冲掉" % label)
    w("  -> %s  %d 点  长度 %.0f  文件夹 %s"
      % (label, np_, sp.get_spline_length(), a.get_folder_path()))
    return np_


def restore_from_backup(eas, cls):
    """不读关卡里的 _Part，直接按备份文件重建。"""
    w("恢复模式：按 %s 重建" % BACKUP)
    if not os.path.isfile(BACKUP):
        w("备份文件不存在。先用正常模式跑一次生成它。")
        return 0
    with open(BACKUP, "r", encoding="utf-8") as f:
        data = json.load(f)
    made = 0
    for rec in data.get("roads", []):
        chain = [unreal.Vector(p[0], p[1], p[2]) for p in rec["points"]]
        if len(chain) < 2:
            w("  !! %s 只有 %d 点，跳过" % (rec.get("label"), len(chain)))
            continue
        if build_actor(eas, cls, rec["label"], rec.get("folder", ""), chain):
            made += 1
    w("")
    w("按备份恢复出 %d 条路。" % made)
    return made


def main():
    eas = unreal.get_editor_subsystem(unreal.EditorActorSubsystem)
    actors = eas.get_all_level_actors()

    # ------------------------------------------------------------ 先删旧产物
    old = 0
    for a in actors:
        try:
            if TAG in set(str(t) for t in a.tags):
                eas.destroy_actor(a)
                old += 1
        except Exception:
            continue
    w("清除旧合并产物 %d 个" % old)

    if RESTORE_FROM_FILE:
        cls = unreal.EditorAssetLibrary.load_asset(ROAD_BP)
        if not cls:
            w("加载不到 %s" % ROAD_BP)
            return
        restore_from_backup(eas, cls.generated_class())
        return

    # ------------------------------------------------------------ 收集分段
    parts = {}        # 基路名 -> [(编号, 标签, 点列表, 文件夹)]
    for a in actors:
        try:
            folder = str(a.get_folder_path()).replace("\\", "/")
        except Exception:
            continue
        if not any(f.lower() in folder.lower() for f in ROAD_FOLDERS):
            continue
        try:
            label = a.get_actor_label()
        except Exception:
            continue
        idx = part_index(label)
        if idx is None:
            continue                       # 没被切的路不用管
        try:
            sp = a.get_component_by_class(unreal.SplineComponent)
        except Exception:
            sp = None
        if not sp:
            continue
        pts = spline_points(sp)
        if len(pts) < 2:
            w("!! %s 只有 %d 个点，跳过" % (label, len(pts)))
            continue
        parts.setdefault(base_road_name(label), []).append((idx, label, pts, folder))

    w("找到被切的逻辑路 %d 条" % len(parts))
    if not parts:
        w("")
        w("一条都没有。确认道路在 %s 下、并且名字带 _PartNN。"
          % "、".join(ROAD_FOLDERS))
        return

    cls = unreal.EditorAssetLibrary.load_asset(ROAD_BP)
    if not cls:
        w("加载不到 %s —— 合并产物需要一个自带 SplineComponent 的蓝图类，"
          % ROAD_BP)
        w("裸 AActor 上加的组件重载后会消失（组件的存在与否存不住）。")
        return
    cls = cls.generated_class()

    made = 0
    backup = []          # 导出用：每条合并路的点列表，落成文本进 git
    for base in sorted(parts):
        segs = sorted(parts[base])
        w("")
        w("=" * 60)
        w("%s  %d 段" % (base, len(segs)))
        w("=" * 60)
        for idx, label, pts, _f in segs:
            w("  Part%02d %-30s %d 点" % (idx, label, len(pts)))

        # ---- 串起来：编号给顺序，端点邻近度决定每段要不要反向 ----
        # 只信编号会出错：编号顺序未必等于几何顺序，反了的话合出来的路会折回去。
        chain = list(segs[0][2])
        folder = segs[0][3]
        used = [segs[0][1]]
        ok = True
        for idx, label, pts, _f in segs[1:]:
            tail = chain[-1]
            d_head = dist2(tail, pts[0])
            d_tail = dist2(tail, pts[-1])
            # 下一段哪一头离当前末尾近，就从那头接
            nxt = list(pts) if d_head <= d_tail else list(reversed(pts))
            gap = min(d_head, d_tail)

            # 第一段本身也可能是反的：如果当前链的**头**离下一段更近，
            # 说明第一段方向反了，整条翻过来重新接。
            if len(used) == 1:
                d_from_head = min(dist2(chain[0], pts[0]), dist2(chain[0], pts[-1]))
                if d_from_head < gap:
                    w("  第一段方向反了，整条翻转（头端距 %.0f < 尾端距 %.0f）"
                      % (d_from_head, gap))
                    chain.reverse()
                    tail = chain[-1]
                    d_head = dist2(tail, pts[0])
                    d_tail = dist2(tail, pts[-1])
                    nxt = list(pts) if d_head <= d_tail else list(reversed(pts))
                    gap = min(d_head, d_tail)

            if gap > MAX_GAP:
                w("  !! Part%02d 离上一段 %.0f，超过 %.0f，**不接**"
                  % (idx, gap, MAX_GAP))
                w("     （这更像一条路的两个远端，硬接会在空地上连出一条直路）")
                ok = False
                break

            # 缺口处补点。Z 线性插值无所谓——生成器逐点打射线重测高度。
            n = max(1, int(gap / GAP_STEP))
            for i in range(1, n):
                t = float(i) / n
                chain.append(unreal.Vector(
                    tail.x + (nxt[0].x - tail.x) * t,
                    tail.y + (nxt[0].y - tail.y) * t,
                    tail.z + (nxt[0].z - tail.z) * t))
            chain.extend(nxt)
            used.append(label)
            w("  接上 Part%02d（缺口 %.0f，补 %d 点）%s"
              % (idx, gap, max(0, n - 1),
                 "" if d_head <= d_tail else "  [该段已反向]"))

        if not ok:
            w("  这条路没有合并。")
            continue

        # ---- 建合并产物 ----
        label = "%s_Merged" % base
        if build_actor(eas, cls, label, folder, chain):
            made += 1
            backup.append({
                "label": label,
                "base": base,
                "folder": folder,
                "from": used,
                "points": [[p.x, p.y, p.z] for p in chain],
            })

    # ------------------------------------------------------------ 写备份
    if backup:
        try:
            d = os.path.dirname(BACKUP)
            if d and not os.path.isdir(d):
                os.makedirs(d)
            with open(BACKUP, "w", encoding="utf-8") as f:
                json.dump({"roads": backup}, f, ensure_ascii=False, indent=1)
            w("")
            w("备份已写入 %s（%d 条路，共 %d 点）"
              % (BACKUP, len(backup), sum(len(r["points"]) for r in backup)))
            w("**把它一起提交**：合并产物在关卡里，而关卡被 git pull 的 autostash")
            w("冲掉过两次；有这份文本就能用 RESTORE_FROM_FILE=True 重建，")
            w("不需要队友的 _Part 分段还在。")
        except Exception as exc:
            w("")
            w("!! 备份写入失败：%s" % exc)

    w("")
    w("=" * 60)
    w("合并出 %d 条路" % made)
    w("=" * 60)
    w("队友的 _Part actor **一个都没删**，源数据留着。")
    w("生成器那边要跳过带 _PartNN 的（road_spline_of 里已加），")
    w("否则同一条路会被铺两遍车道。")
    w("")
    w("接下来：")
    w("  py rebuild_traffic.py      <- 之后就是普通流程了")
    w("  py fill_turn_splines.py")
    w("  py fix_t_junctions.py")
    w("  py gen_traffic_lights.py")
    w("")
    w("验证：重启编辑器后再跑一次本脚本，开头应打「清除旧合并产物 %d 个」——" % made)
    w("点存住了才算成，脚本说写入多少只证明函数被调用过。")
    w("队友重新同步 landscape spline 之后，删掉 tag=%s 的 actor 重跑本脚本。" % TAG)


try:
    main()
except Exception:
    w("抛异常：")
    w(traceback.format_exc())

with open(OUT, "w", encoding="utf-8") as f:
    f.write("\n".join(lines))

print("\n".join(lines))
print()
print("报告：" + OUT)
