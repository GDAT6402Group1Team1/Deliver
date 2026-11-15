# -*- coding: utf-8 -*-
"""将游戏蓝图、控件模板及正式地图里的已知英文文案改为中文"""
import json
import os
import re
from pathlib import Path

import unreal

ROOT = Path(unreal.Paths.project_dir())
TRANSLATIONS = json.loads(Path(__file__).with_name("prompt_chinese_translations.json").read_text(encoding="utf-8"))
AUDIT_ONLY = os.environ.get("DELIVERY_UI_AUDIT_ONLY") == "1"
ROWS = []


def record(obj, name, before, after):
    ROWS.append(dict(object=obj.get_path_name(), property=str(name), before=before, after=after))


def update_text_properties(obj):
    if not obj or obj.get_class().get_name().startswith(("SKEL_", "REINST_")):
        return
    for name in dir(obj):
        if name.startswith("_") or name in ("clip_to_bounds", "opacity", "visiblity"):
            continue
        try:
            value = obj.get_editor_property(name)
        except Exception:
            continue
        if not isinstance(value, unreal.Text):
            continue
        before = str(value)
        if before not in TRANSLATIONS:
            continue
        after = TRANSLATIONS[before]
        if not AUDIT_ONLY:
            obj.modify()
            obj.set_editor_property(name, unreal.Text(after))
            assert str(obj.get_editor_property(name)) == after, (obj.get_path_name(), name)
        record(obj, name, before, after)


def update_widget_graphs(blueprint):
    library = unreal.BlueprintEditorLibrary
    pin_api = unreal.BlueprintGraphPin
    if not hasattr(pin_api, "get_pin_value"):
        raise RuntimeError("BlueprintGraphPin.get_pin_value unavailable")
    initial_count = len(ROWS)
    for graph in library.list_graphs(blueprint) or []:
        editor = unreal.BlueprintGraphEditor.get_graph_editor_by_name(blueprint, graph.get_name())
        if not editor:
            continue
        for node in unreal.BlueprintGraphEditor.list_all_nodes(editor) or []:
            for pin in library.list_all_pins(node) or []:
                if pin_api.list_connected_pins(pin):
                    continue
                before = pin_api.get_pin_value(pin)
                if not isinstance(before, str):
                    continue
                source = before
                literal = re.search(r'("(?:[^"\\]|\\.)*")\)$', before)
                if before.startswith(("NSLOCTEXT(", "LOCTEXT(", "INVTEXT(")) and literal:
                    source = json.loads(literal[1])
                    if source not in TRANSLATIONS:
                        continue
                    after = before[:literal.start(1)] + json.dumps(TRANSLATIONS[source], ensure_ascii=False) + ")"
                elif before in TRANSLATIONS and str(pin_api.get_pin_type_display_string(pin)) in ("Text", "String", "文本", "字符串"):
                    after = TRANSLATIONS[before]
                else:
                    continue
                name = pin_api.get_pin_name(pin)
                if not AUDIT_ONLY:
                    node.modify()
                    assert pin_api.set_pin_value(pin, after), (node.get_path_name(), name)
                    assert TRANSLATIONS[source] in pin_api.get_pin_value(pin), (node.get_path_name(), name)
                record(node, name, before, after)
    if not AUDIT_ONLY and len(ROWS) != initial_count:
        library.compile_blueprint(blueprint)


def save_asset(path):
    if not unreal.EditorAssetLibrary.save_asset(path, only_if_is_dirty=False):
        raise RuntimeError("保存失败 " + path)


def localize_settlement_panel(blueprint):
    prefix = blueprint.get_path_name() + ":WidgetTree."
    widgets = {
        widget.get_name(): widget for widget in unreal.ObjectIterator(unreal.Widget)
        if widget.get_path_name().startswith(prefix)
    }
    background = widgets["Image_31"]
    brush = background.get_editor_property("brush")
    old_texture = unreal.load_asset("/Game/UI/Image/T_TaskSettlementLabels")
    if brush.get_editor_property("resource_object") != old_texture:
        return

    plain_texture = unreal.load_asset("/Game/UI/Task/Source/T_TaskSettlementBackground")
    chinese_font = unreal.load_asset("/Game/UI/Conversation/F_Conversation")
    if not plain_texture or not chinese_font:
        raise RuntimeError("缺少结算底图或中文字体")

    # 栏目沿用对应数值的纵向位置，让数值继续按原有顺序揭晓
    rows = (
        ("TimeGradeText", "TimeGradeLabel", "送达评级"),
        ("ElapsedText", "ElapsedLabel", "配送用时"),
        ("BaseRewardText", "BaseRewardLabel", "基础报酬"),
        ("TimeMultiplierText", "TimeMultiplierLabel", "时效加成"),
        ("SpecialMultiplierText", "SpecialMultiplierLabel", "特殊加成"),
        ("FinalRewardText", "FinalRewardLabel", "总收入"),
    )
    record(background, "brush", old_texture.get_path_name(), plain_texture.get_path_name())
    if AUDIT_ONLY:
        return

    blueprint.modify()
    background.modify()
    brush.set_editor_property("resource_object", plain_texture)
    background.set_brush(brush)
    canvas = background.get_parent()
    tree = background.get_outer()
    canvas.modify()
    tree.modify()
    background_slot = background.get_editor_property("slot")

    def add_label(name, caption, x, y, size, tint, centered=False):
        label = unreal.new_object(unreal.TextBlock, outer=tree, name=name)
        assert unreal.DeliveryWidgetAssetLibrary.register_widget(blueprint, label), name
        label.set_text(caption)
        font = widgets["TaskNameText"].get_editor_property("font")
        font.set_editor_property("font_object", chinese_font)
        font.set_editor_property("size", size)
        label.set_font(font)
        label.set_color_and_opacity(unreal.SlateColor(specified_color=tint))
        label.set_visibility(unreal.SlateVisibility.HIT_TEST_INVISIBLE)
        slot = canvas.add_child_to_canvas(label)
        slot.set_anchors(background_slot.get_anchors())
        slot.set_alignment(unreal.Vector2D(0.5 if centered else 0.0, 0.0))
        slot.set_position(unreal.Vector2D(x, y))
        slot.set_auto_size(True)

    add_label("SettlementTitle", "配送完成", 0, -305, 52, unreal.LinearColor(1, 0.93, 0.72), centered=True)
    ink = unreal.LinearColor(0.006, 0.016, 0.046)
    for value_name, label_name, caption in rows:
        value = widgets[value_name]
        slot = value.get_editor_property("slot")
        is_total = value_name == "FinalRewardText"
        add_label(label_name, caption, -355, slot.get_position().y + (18 if is_total else 0),
                  48 if is_total else 28, ink)

    for name in ("TaskNameText", "TimeGradeText"):
        widget = widgets[name]
        font = widget.get_editor_property("font")
        font.set_editor_property("font_object", chinese_font)
        widget.set_font(font)
    unreal.BlueprintEditorLibrary.compile_blueprint(blueprint)


def main():
    assets = set()
    for folder in ("/Game/Blueprint", "/Game/UI", "/Game/Task", "/Game/Vehicle/Motorbike"):
        assets.update(unreal.EditorAssetLibrary.list_assets(folder, recursive=True))
    changed = set()
    for path in sorted(assets):
        if not path.rsplit("/", 1)[-1].startswith(("BP_", "DA_", "WBP_")):
            continue
        asset = unreal.load_asset(path)
        if not asset:
            continue
        initial_count = len(ROWS)
        update_text_properties(asset)
        if isinstance(asset, unreal.Blueprint):
            default = unreal.get_default_object(asset.generated_class())
            update_text_properties(default)
            if isinstance(default, unreal.Actor):
                for component in default.get_components_by_class(unreal.ActorComponent):
                    update_text_properties(component)
            if isinstance(asset, unreal.WidgetBlueprint):
                update_widget_graphs(asset)
                if asset.get_path_name() == "/Game/UI/Task/WBP_Reward.WBP_Reward":
                    localize_settlement_panel(asset)
        if len(ROWS) != initial_count:
            changed.add(asset.get_path_name().split(".")[0])

    for widget in unreal.ObjectIterator(unreal.Widget):
        path = widget.get_path_name()
        if not path.startswith(("/Game/UI/", "/Game/Task/UI/")):
            continue
        initial_count = len(ROWS)
        update_text_properties(widget)
        if len(ROWS) != initial_count:
            changed.add(path.split(".")[0])

    failures = []
    if not AUDIT_ONLY:
        for path in sorted(changed):
            try:
                save_asset(path)
            except RuntimeError:
                failures.append(path)

    # 检查首单正式地图里覆盖了蓝图默认值的交互提示
    levels = unreal.get_editor_subsystem(unreal.LevelEditorSubsystem)
    if not levels.load_level("/Game/Level/TestForCharacter"):
        raise RuntimeError("地图加载失败")
    actor_subsystem = unreal.get_editor_subsystem(unreal.EditorActorSubsystem)
    initial_count = len(ROWS)
    for actor in actor_subsystem.get_all_level_actors():
        components = actor.get_components_by_class(unreal.ActorComponent)
        relevant = [component for component in components if "Delivery" in component.get_class().get_name()]
        if relevant or "Delivery" in actor.get_class().get_name():
            update_text_properties(actor)
            for component in relevant:
                update_text_properties(component)
    map_changed = len(ROWS) != initial_count
    if not AUDIT_ONLY and map_changed and not levels.save_current_level():
        failures.append("/Game/Level/TestForCharacter")

    report = "chinese_ui_audit.json" if AUDIT_ONLY else "chinese_ui_changes.json"
    (ROOT / "Saved" / report).write_text(json.dumps(ROWS, ensure_ascii=False, indent=2), encoding="utf-8")
    unreal.log(f"[ChineseUI] matched={len(ROWS)}, assets={len(changed)}, map={map_changed}")
    if failures:
        raise RuntimeError("Could not save: " + ", ".join(failures))


if __name__ == "__main__":
    main()
