"""Register the PS2DEM menu when the Unreal Editor starts."""

import unreal


OWNER = "PS2DEMImporter"


def _entry(name: str, label: str, tooltip: str, command: str) -> unreal.ToolMenuEntry:
    item = unreal.ToolMenuEntry(name=name, type=unreal.MultiBlockType.MENU_ENTRY)
    item.set_label(label)
    item.set_tool_tip(tooltip)
    item.set_string_command(unreal.ToolMenuStringCommandType.PYTHON, "", command)
    return item


def register_ps2dem_menu() -> None:
    menus = unreal.ToolMenus.get()
    try:
        menus.unregister_owner_by_name(OWNER)
    except Exception:
        pass

    main_menu = menus.extend_menu("LevelEditor.MainMenu")
    submenu = main_menu.add_sub_menu(
        OWNER,
        "PS2DEM",
        "PS2DEM.MainMenu",
        "PS2DEM",
        "Import PS2DEM terrain and Photoshop building whiteboxes",
    )
    submenu.add_menu_entry(
        "PS2DEM",
        _entry(
            "PS2DEM.ImportBuildings",
            "Import / Reimport Buildings",
            "Replace generated A/B/C whiteboxes from buildings.json",
            "import importlib, ps2dem_buildings; "
            "importlib.reload(ps2dem_buildings); ps2dem_buildings.import_whiteboxes()",
        ),
    )
    submenu.add_menu_entry(
        "PS2DEM",
        _entry(
            "PS2DEM.ImportTerrain",
            "Import Terrain",
            "Validate the latest PS2DEM R16 terrain and open Landscape import mode",
            "import importlib, ps2dem_terrain; "
            "importlib.reload(ps2dem_terrain); ps2dem_terrain.import_terrain()",
        ),
    )
    submenu.add_menu_entry(
        "PS2DEM",
        _entry(
            "PS2DEM.ImportSplines",
            "Import / Reimport Splines",
            "Replace generated MainRoad, BranchRoad and River route splines",
            "import importlib, ps2dem_splines; "
            "importlib.reload(ps2dem_splines); ps2dem_splines.import_splines()",
        ),
    )
    submenu.add_menu_entry(
        "PS2DEM",
        _entry(
            "PS2DEM.ApplySelectedSplinesToLandscape",
            "Apply Selected Splines to Landscape",
            "Raise/lower the Landscape from selected PS2DEM route splines",
            "import importlib, ps2dem_landscape_splines; "
            "importlib.reload(ps2dem_landscape_splines); "
            "ps2dem_landscape_splines.apply_selected_splines_to_landscape()",
        ),
    )
    submenu.add_menu_entry(
        "PS2DEM",
        _entry(
            "PS2DEM.ResetLandscapeSplineLayer",
            "Reset Landscape Deformation Layer...",
            "Open Landscape mode and show how to reset the PS2DEM edit layer",
            "import importlib, ps2dem_landscape_splines; "
            "importlib.reload(ps2dem_landscape_splines); "
            "ps2dem_landscape_splines.show_reset_instructions()",
        ),
    )
    submenu.add_menu_entry(
        "PS2DEM",
        _entry(
            "PS2DEM.OpenExchange",
            "Show Exchange Folder",
            "Open this project's Saved/PS2DEMImporter exchange folder",
            "import importlib, ps2dem_common; "
            "importlib.reload(ps2dem_common); ps2dem_common.show_exchange_folder()",
        ),
    )
    menus.refresh_all_widgets()
    unreal.log("PS2DEM Importer menu registered")


register_ps2dem_menu()
