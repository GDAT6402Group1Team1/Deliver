"""编辑器启动时注册项目自己的菜单。

UE 会自动执行项目 Content/Python 下的 init_unreal.py。
（PS2DEM 插件有自己的同名脚本，两边互不影响。）
"""

import unreal


OWNER = "DeliveryTools"


def _entry(name, label, tooltip, command):
    item = unreal.ToolMenuEntry(name=name, type=unreal.MultiBlockType.MENU_ENTRY)
    item.set_label(label)
    item.set_tool_tip(tooltip)
    item.set_string_command(unreal.ToolMenuStringCommandType.PYTHON, "", command)
    return item


def register_delivery_menu():
    menus = unreal.ToolMenus.get()
    try:
        menus.unregister_owner_by_name(OWNER)
    except Exception:
        pass

    main_menu = menus.extend_menu("LevelEditor.MainMenu")
    submenu = main_menu.add_sub_menu(
        OWNER,
        "Delivery",
        "Delivery.MainMenu",
        "Delivery",
        "Delivery 项目工具",
    )
    submenu.add_menu_entry(
        "Delivery",
        _entry(
            "Delivery.ImportTasks",
            "Import / Reimport Tasks",
            "从 Design/Tasks.csv 同步任务配置资产（按 TaskId 增量更新，不会重建已有资产）",
            "import importlib, delivery_task_import; "
            "importlib.reload(delivery_task_import); delivery_task_import.import_tasks()",
        ),
    )
    submenu.add_menu_entry(
        "Delivery",
        _entry(
            "Delivery.SetupMotorbike",
            "Setup Motorbike",
            "导入摩托车.fbx、建 BP_Motorbike、配好 F 键，并在当前关卡放一辆（幂等，可重复点）",
            "import importlib, setup_motorbike; "
            "importlib.reload(setup_motorbike); setup_motorbike.run()",
        ),
    )
    submenu.add_menu_entry(
        "Delivery",
        _entry(
            "Delivery.ReimportMotorbikeRider",
            "Reimport Motorbike Rider",
            "只重导骑手骨骼网格（整套重跑会因为车体已存在而跳过导入，改骨骼导入选项时用这个）",
            "import importlib, setup_motorbike; "
            "importlib.reload(setup_motorbike); setup_motorbike.reimport_rider()",
        ),
    )

    menus.refresh_all_widgets()


register_delivery_menu()
