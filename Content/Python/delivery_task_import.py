"""把策划维护的任务表（CSV）导入成 UDeliveryTaskDefinition 资产。

为什么不用 DataTable：任务的解锁条件是内联的 UObject（可以继承出任意条件），
DataTable 只能存值，表达不了。所以保留 DataAsset，用这个脚本从表里同步数值。

同步策略是**按 TaskId 增量更新**：已存在的资产只改字段、不重建，
这样美术或程序在资产上手填的东西（来电语音、特殊事件倍率等表里没有的列）不会被冲掉。
表里没有的任务不会被删除，只在日志里提示。

CSV 列顺序（和策划表一致，第一行是表头，按列名匹配而不是按位置）：
    Task ID, Task Name, Delivery Item ID, Pickup Location ID, Delivery Location ID,
    Receiver NPC ID, Unlock Condition, Simple Task Description, Detailed Task Description,
    Time Limit, Base Reward, Time Rating, Special Event ID
"""

import csv
import io
import os

import unreal


CSV_RELATIVE_PATH = "Design/Tasks.csv"
ASSET_FOLDER = "/Game/Task/Definitions"

# 表头 -> 资产字段。用列名匹配，这样策划调整列顺序不会把数据串位。
COLUMN_TASK_ID = "Task ID"
COLUMN_TASK_NAME = "Task Name"
COLUMN_ITEM_ID = "Delivery Item ID"
COLUMN_PICKUP_ID = "Pickup Location ID"
COLUMN_DELIVERY_ID = "Delivery Location ID"
COLUMN_RECEIVER_ID = "Receiver NPC ID"
COLUMN_UNLOCK = "Unlock Condition"
COLUMN_SIMPLE_DESC = "Simple Task Description"
COLUMN_DETAIL_DESC = "Detailed Task Description"
COLUMN_TIME_LIMIT = "Time Limit"
COLUMN_BASE_REWARD = "Base Reward"
COLUMN_TIME_RATING = "Time Rating"
COLUMN_SPECIAL_EVENT = "Special Event ID"

REQUIRED_COLUMNS = [COLUMN_TASK_ID, COLUMN_TASK_NAME, COLUMN_TIME_LIMIT, COLUMN_BASE_REWARD]


def _log(message):
    unreal.log("[TaskImport] {}".format(message))


def _warn(message):
    unreal.log_warning("[TaskImport] {}".format(message))


def _error(message):
    unreal.log_error("[TaskImport] {}".format(message))


def _csv_path():
    project_dir = unreal.Paths.convert_relative_path_to_full(unreal.Paths.project_dir())
    return os.path.join(project_dir, CSV_RELATIVE_PATH)


def _parse_float(raw, default=0.0):
    try:
        return float(str(raw).strip())
    except (TypeError, ValueError):
        return default


def _parse_time_rating(raw):
    """`150,1.2|60,1.1|0,1|-60,0.9` -> [FDeliveryTimeGrade, ...]

    每段是"剩余秒数,倍率"。剩余为负表示超时，所以超时分档在这一列里就表达完了，
    不需要另配超时倍率。
    """
    grades = []
    text = (raw or "").strip()
    if not text:
        return grades

    for index, chunk in enumerate(text.split("|")):
        chunk = chunk.strip()
        if not chunk:
            continue

        parts = chunk.split(",")
        if len(parts) != 2:
            _warn("Time Rating 第 {} 段格式不对（应为 `剩余秒数,倍率`）：{}".format(index + 1, chunk))
            continue

        grade = unreal.DeliveryTimeGrade()
        grade.set_editor_property("remaining_seconds", _parse_float(parts[0]))
        grade.set_editor_property("multiplier", _parse_float(parts[1], 1.0))
        grades.append(grade)

    # 顺序必须是剩余时间从多到少，结算时取第一条够得着的档
    for i in range(1, len(grades)):
        current = grades[i].get_editor_property("remaining_seconds")
        previous = grades[i - 1].get_editor_property("remaining_seconds")
        if current > previous:
            _warn("Time Rating 的档位顺序不对：第 {} 段({}) 比上一段({}) 大，靠后的档永远命中不到".format(
                i + 1, current, previous))
            break

    return grades


def _parse_unlock_conditions(raw, task_id, owner_asset):
    """`time:15` / `task:Task_001` / 两者用 `|` 连接。留空表示开局解锁。

    策划表里那列原本写的是自然语言（"游戏开始15秒后"），没法可靠解析，
    所以约定成这种机器可读的写法。解析不了的内容会告警并当成"无条件"，
    不会静默生成一个错的条件。

    注意条件对象必须以资产为 outer 创建。Instanced 属性只序列化属于本包的子对象，
    用默认 outer（transient package）建出来的条件保存时会被静默丢掉。
    """
    conditions = []
    text = (raw or "").strip()
    if not text:
        return conditions

    for chunk in text.split("|"):
        chunk = chunk.strip()
        if not chunk:
            continue

        if ":" not in chunk:
            _warn("{}: 解锁条件 `{}` 无法解析，已忽略。支持的写法：time:15 / task:Task_001".format(task_id, chunk))
            continue

        kind, _, value = chunk.partition(":")
        kind = kind.strip().lower()
        value = value.strip()

        if kind == "time":
            condition = unreal.new_object(
                unreal.DeliveryTaskUnlockCondition_TimeSinceStart, outer=owner_asset)
            condition.set_editor_property("delay_seconds", _parse_float(value, 0.0))
            conditions.append(condition)
        elif kind == "task":
            # 前置任务引用的是另一份资产，按 TaskId 找。此时对方可能还没导入，
            # 所以整张表跑完之后再统一回填（见 _resolve_prerequisites）。
            conditions.append(("task", value))
        else:
            _warn("{}: 不认识的解锁条件类型 `{}`，已忽略".format(task_id, kind))

    return conditions


def _asset_path_for(task_id):
    return "{}/DA_{}".format(ASSET_FOLDER, task_id)


def _load_or_create(task_id):
    asset_path = _asset_path_for(task_id)

    existing = unreal.EditorAssetLibrary.load_asset(asset_path)
    if existing:
        return existing, False

    tools = unreal.AssetToolsHelpers.get_asset_tools()
    factory = unreal.DataAssetFactory()
    factory.set_editor_property("data_asset_class", unreal.DeliveryTaskDefinition)

    created = tools.create_asset(
        asset_name="DA_{}".format(task_id),
        package_path=ASSET_FOLDER,
        asset_class=unreal.DeliveryTaskDefinition,
        factory=factory,
    )
    return created, True


def _apply_row(asset, row):
    task_id = row[COLUMN_TASK_ID].strip()

    asset.set_editor_property("task_id", task_id)
    asset.set_editor_property("display_name", unreal.Text(row.get(COLUMN_TASK_NAME, "").strip()))
    asset.set_editor_property("simple_description", unreal.Text(row.get(COLUMN_SIMPLE_DESC, "").strip()))
    asset.set_editor_property("detailed_description", unreal.Text(row.get(COLUMN_DETAIL_DESC, "").strip()))

    # 这几个 ID 指向的系统还没做，先原样存着
    asset.set_editor_property("delivery_item_id", row.get(COLUMN_ITEM_ID, "").strip())
    asset.set_editor_property("pickup_location_id", row.get(COLUMN_PICKUP_ID, "").strip())
    asset.set_editor_property("delivery_location_id", row.get(COLUMN_DELIVERY_ID, "").strip())
    asset.set_editor_property("receiver_npc_id", row.get(COLUMN_RECEIVER_ID, "").strip())
    asset.set_editor_property("special_event_id", row.get(COLUMN_SPECIAL_EVENT, "").strip())

    asset.set_editor_property("time_limit_seconds", _parse_float(row.get(COLUMN_TIME_LIMIT), 300.0))
    asset.set_editor_property("base_reward", int(_parse_float(row.get(COLUMN_BASE_REWARD), 0.0)))
    asset.set_editor_property("time_grades", _parse_time_rating(row.get(COLUMN_TIME_RATING)))

    # 黄红阈值表里没有，保持资产上已有的值，不要覆盖成默认值

    return _parse_unlock_conditions(row.get(COLUMN_UNLOCK), task_id, asset)


def _resolve_prerequisites(pending, assets_by_id):
    """把 `task:XXX` 形式的前置条件回填成真正的条件对象。

    整张表导完再做，这样表里先出现的任务也能引用后出现的任务。
    """
    for task_id, raw_conditions in pending.items():
        asset = assets_by_id.get(task_id)
        if not asset:
            continue

        resolved = []
        prerequisites = []

        for item in raw_conditions:
            if isinstance(item, tuple) and item[0] == "task":
                target = assets_by_id.get(item[1])
                if target:
                    prerequisites.append(target)
                else:
                    _warn("{}: 前置任务 `{}` 在表里找不到，这条依赖被忽略".format(task_id, item[1]))
            else:
                resolved.append(item)

        if prerequisites:
            condition = unreal.new_object(
                unreal.DeliveryTaskUnlockCondition_TasksCompleted, outer=asset)
            condition.set_editor_property("required_tasks", prerequisites)
            resolved.append(condition)

        asset.set_editor_property("unlock_conditions", resolved)


def import_tasks():
    path = _csv_path()
    if not os.path.isfile(path):
        _error("找不到任务表：{}（把策划导出的 CSV 放到项目的 {} ）".format(path, CSV_RELATIVE_PATH))
        return

    # 策划表大概率是 Excel/飞书导出的，带 BOM，用 utf-8-sig 一并吃掉
    with io.open(path, "r", encoding="utf-8-sig", newline="") as handle:
        reader = csv.DictReader(handle)
        rows = [row for row in reader]
        headers = reader.fieldnames or []

    missing = [name for name in REQUIRED_COLUMNS if name not in headers]
    if missing:
        _error("表头缺少必需的列：{}。实际读到的表头：{}".format(", ".join(missing), headers))
        return

    assets_by_id = {}
    pending_conditions = {}
    created_count = 0
    updated_count = 0

    for line_number, row in enumerate(rows, start=2):
        task_id = (row.get(COLUMN_TASK_ID) or "").strip()
        if not task_id:
            _warn("第 {} 行没有 Task ID，跳过".format(line_number))
            continue

        asset, was_created = _load_or_create(task_id)
        if not asset:
            _error("第 {} 行：创建资产失败（{}）".format(line_number, task_id))
            continue

        pending_conditions[task_id] = _apply_row(asset, row)
        assets_by_id[task_id] = asset

        if was_created:
            created_count += 1
        else:
            updated_count += 1

    _resolve_prerequisites(pending_conditions, assets_by_id)

    for asset in assets_by_id.values():
        unreal.EditorAssetLibrary.save_loaded_asset(asset)

    _log("完成：新建 {} 个，更新 {} 个，共 {} 个任务".format(created_count, updated_count, len(assets_by_id)))
    _log("记得把这些资产加进 GameState 蓝图的 TaskDefinitions —— 导入工具不会替你改蓝图")
