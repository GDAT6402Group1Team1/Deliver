# -*- coding: utf-8 -*-
"""通话结束后收起面板，并统一手机状态与光标管理。"""
from pathlib import Path
import unreal

BP = '/Game/UI/Task/WBP_TaskHUD'
BEL = unreal.BlueprintEditorLibrary
BGE = unreal.BlueprintGraphEditor
BGP = unreal.BlueprintGraphPin
bp = unreal.EditorAssetLibrary.load_asset(BP)
if not bp:
    raise RuntimeError('Phone HUD not found')
backup = Path(unreal.Paths.project_saved_dir()) / 'WBP_TaskHUD_before_autoclose.uasset'
source = Path(unreal.Paths.project_content_dir()) / 'UI/Task/WBP_TaskHUD.uasset'
if not backup.exists():
    backup.write_bytes(source.read_bytes())

def nodes(editor):
    return list(BGE.list_all_nodes(editor))

def title(node):
    return str(BEL.get_node_title(node))

def pin(node, name):
    found = next((p for p in BEL.list_all_pins(node) if str(BGP.get_pin_name(p)) == name), None)
    if found is None:
        raise RuntimeError('Missing pin ' + name + ' on ' + title(node))
    return found

def links(p):
    return list(BGP.list_connected_pins(p))

def unlink(p):
    try:
        BGP.break_pin_links(p)
    except TypeError:
        BGP.break_pin_links(p, True)

def wire(a, b):
    BGP.try_create_connection(a, b)
    expected = (BGP.get_owning_node(b).get_path_name(), str(BGP.get_pin_name(b)))
    actual = [(BGP.get_owning_node(p).get_path_name(), str(BGP.get_pin_name(p))) for p in links(a)]
    if expected not in actual:
        raise RuntimeError('Connection failed: ' + str(expected) + ' actual=' + str(actual))

def create_call(editor, candidates, required):
    for candidate in candidates:
        node = editor.add_call_function_node(candidate)
        if not node:
            continue
        names = {str(BGP.get_pin_name(p)) for p in BEL.list_all_pins(node)}
        if set(required).issubset(names):
            return node
        editor.remove_nodes([node])
    raise RuntimeError('Cannot create function call: ' + str(candidates))

editor = BGE.get_graph_editor_by_name(bp, 'SetPhoneOpen')
all_nodes = nodes(editor)
setter = next(n for n in all_nodes if title(n) == 'Set IsPhoneOpen')
entry = next(n for n in all_nodes if n.get_class().get_name() == 'K2Node_FunctionEntry')
self_node = next(n for n in all_nodes if n.get_class().get_name() == 'K2Node_Self')
existing = next((n for n in all_nodes if any(str(BGP.get_pin_name(p)) == 'PhoneWidget' for p in BEL.list_all_pins(n))), None)
if existing is None:
    following = links(pin(setter, 'then'))
    if len(following) != 1:
        raise RuntimeError('Unexpected SetPhoneOpen execution chain')
    sync = create_call(editor, ['/Script/Delivery.DeliveryPlayerController:ReportPhoneUIOpen',
                               '/Script/Delivery.DeliveryPlayerController.ReportPhoneUIOpen',
                               'ReportPhoneUIOpen'], ['PhoneWidget', 'bOpen', 'execute', 'then'])
    wire(pin(self_node, 'self'), pin(sync, 'PhoneWidget'))
    wire(pin(entry, 'bOpen'), pin(sync, 'bOpen'))
    unlink(pin(setter, 'then'))
    wire(pin(setter, 'then'), pin(sync, 'execute'))
    wire(pin(sync, 'then'), following[0])

legacy = []
for n in nodes(editor):
    key = title(n).lower().replace(' ', '').replace('_', '')
    if key in ['setbshowmousecursor', 'setinputmodegameandui', 'setinputmodegameonly']:
        legacy.append(n)
if legacy:
    editor.remove_nodes(legacy)
unreal.log('PHONE_AUTOCLOSE: removed legacy cursor/input nodes=' + str(len(legacy)))

editor = BGE.get_graph_editor_by_name(bp, 'RefreshPhone')
switch = next(n for n in nodes(editor) if n.get_class().get_name() == 'K2Node_SwitchEnum')
# 纠正旧版本：接通时保持展开，通话结束 Idle 时才收起。
incall = pin(switch, 'InCall')
following = links(incall)
if len(following) != 1:
    raise RuntimeError('Unexpected InCall execution chain')
old_close = BGP.get_owning_node(following[0])
if title(old_close) == 'SetPhoneOpen':
    after = links(pin(old_close, 'then'))
    if len(after) != 1:
        raise RuntimeError('Unexpected old auto-close chain')
    unlink(incall)
    editor.remove_nodes([old_close])
    wire(incall, after[0])
idle = pin(switch, 'Idle')
following = links(idle)
if len(following) != 1:
    raise RuntimeError('Unexpected Idle execution chain')
next_node = BGP.get_owning_node(following[0])
if title(next_node) != 'SetPhoneOpen':
    close = create_call(editor, ['SetPhoneOpen', BP + '.WBP_TaskHUD_C:SetPhoneOpen'], ['bOpen', 'execute', 'then'])
    BGP.set_pin_value(pin(close, 'bOpen'), 'false')
    unlink(idle)
    wire(idle, pin(close, 'execute'))
    wire(pin(close, 'then'), following[0])
BEL.compile_blueprint(bp)
status = str(bp.get_editor_property('status'))
if 'UP_TO_DATE' not in status:
    raise RuntimeError('Phone HUD compile failed: ' + status)
if not unreal.EditorAssetLibrary.save_loaded_asset(bp, only_if_is_dirty=False):
    raise RuntimeError('Phone HUD save failed')
unreal.log('PHONE_AUTOCLOSE_OK: ' + status)
