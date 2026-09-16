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

    menus.refresh_all_widgets()


register_delivery_menu()
