"""
源码目录整理：把拥挤的模块目录分成子目录

在仓库根目录运行，先关闭 UE 编辑器和 Live Coding
  python Tools/Reorganize/reorganize_sources.py           只预览，不改任何文件
  python Tools/Reorganize/reorganize_sources.py --apply   真正执行

做的事
  1 改写 Source 和 Plugins 里受影响的 #include，统一写成相对 Source/Delivery 的完整路径
  2 改写 CLAUDE.md、AGENTS.md、Document 和各 README 里的源码路径
  3 按下方清单用 git mv 搬文件，保留历史
类名、UCLASS 和蓝图引用都不变，不需要 CoreRedirects
执行后重新编译 DeliveryEditor
"""

import argparse
import posixpath
import re
import subprocess
import sys
from pathlib import Path

# 旧路径|新路径，均相对 Source/Delivery
MOVE_TEXT = """
AI/Companion/DeliveryCompanionCharacter.cpp|AI/Companion/Core/DeliveryCompanionCharacter.cpp
AI/Companion/DeliveryCompanionCharacter.h|AI/Companion/Core/DeliveryCompanionCharacter.h
AI/Companion/DeliveryCompanionComponent.cpp|AI/Companion/Core/DeliveryCompanionComponent.cpp
AI/Companion/DeliveryCompanionComponent.h|AI/Companion/Core/DeliveryCompanionComponent.h
AI/Companion/DeliveryCompanionTuning.h|AI/Companion/Core/DeliveryCompanionTuning.h
AI/Companion/DeliveryCompanionBodyAnimInstance.cpp|AI/Companion/Core/DeliveryCompanionBodyAnimInstance.cpp
AI/Companion/DeliveryCompanionBodyAnimInstance.h|AI/Companion/Core/DeliveryCompanionBodyAnimInstance.h
AI/Companion/DeliveryCompanionMemory.cpp|AI/Companion/Core/DeliveryCompanionMemory.cpp
AI/Companion/DeliveryCompanionWorldContext.cpp|AI/Companion/Core/DeliveryCompanionWorldContext.cpp
AI/Companion/DeliveryCompanionEncounter.cpp|AI/Companion/Encounter/DeliveryCompanionEncounter.cpp
AI/Companion/DeliveryCompanionEncounterScene.cpp|AI/Companion/Encounter/DeliveryCompanionEncounterScene.cpp
AI/Companion/DeliveryCompanionEncounterScene.h|AI/Companion/Encounter/DeliveryCompanionEncounterScene.h
AI/Companion/DeliveryCompanionHome.cpp|AI/Companion/Home/DeliveryCompanionHome.cpp
AI/Companion/DeliveryCompanionHomeChoices.cpp|AI/Companion/Home/DeliveryCompanionHomeChoices.cpp
AI/Companion/DeliveryCompanionHomeExecution.cpp|AI/Companion/Home/DeliveryCompanionHomeExecution.cpp
AI/Companion/DeliveryCompanionHomeGround.cpp|AI/Companion/Home/DeliveryCompanionHomeGround.cpp
AI/Companion/DeliveryCompanionHomeSeat.cpp|AI/Companion/Home/DeliveryCompanionHomeSeat.cpp
AI/Companion/DeliveryCompanionHomeTypes.h|AI/Companion/Home/DeliveryCompanionHomeTypes.h
AI/Companion/DeliveryCompanionErrand.cpp|AI/Companion/Errand/DeliveryCompanionErrand.cpp
AI/Companion/DeliveryCompanionItemErrand.cpp|AI/Companion/Errand/DeliveryCompanionItemErrand.cpp
AI/Companion/DeliveryCompanionItemPerception.cpp|AI/Companion/Errand/DeliveryCompanionItemPerception.cpp
AI/Companion/DeliveryCompanionItemTypes.h|AI/Companion/Errand/DeliveryCompanionItemTypes.h
AI/Companion/DeliveryCompanionInventory.cpp|AI/Companion/Errand/DeliveryCompanionInventory.cpp
AI/Companion/DeliveryCompanionPackage.cpp|AI/Companion/Errand/DeliveryCompanionPackage.cpp
AI/Companion/DeliveryCompanionPackagePlacement.cpp|AI/Companion/Errand/DeliveryCompanionPackagePlacement.cpp
AI/Companion/DeliveryCompanionCaterpillar.cpp|AI/Companion/Errand/DeliveryCompanionCaterpillar.cpp
AI/Companion/DeliveryCompanionCaterpillarExecution.cpp|AI/Companion/Errand/DeliveryCompanionCaterpillarExecution.cpp
AI/Companion/DeliveryCompanionVehicle.cpp|AI/Companion/Travel/DeliveryCompanionVehicle.cpp
AI/Companion/DeliveryCompanionTravel.cpp|AI/Companion/Travel/DeliveryCompanionTravel.cpp
AI/Companion/DeliveryCompanionCatchUp.cpp|AI/Companion/Travel/DeliveryCompanionCatchUp.cpp
AI/Companion/DeliveryCompanionAssist.cpp|AI/Companion/Assist/DeliveryCompanionAssist.cpp
AI/Presentation/DeliveryNpcSpeechPlayback.cpp|AI/Presentation/Speech/DeliveryNpcSpeechPlayback.cpp
AI/Presentation/DeliveryNpcSpeechGeneration.cpp|AI/Presentation/Speech/DeliveryNpcSpeechGeneration.cpp
AI/Presentation/DeliveryNpcSpeechTiming.h|AI/Presentation/Speech/DeliveryNpcSpeechTiming.h
AI/Presentation/DeliveryNpcVoiceCodec.cpp|AI/Presentation/Speech/DeliveryNpcVoiceCodec.cpp
AI/Presentation/DeliveryNpcVoiceCodec.h|AI/Presentation/Speech/DeliveryNpcVoiceCodec.h
AI/Presentation/DeliveryNpcVoiceSet.cpp|AI/Presentation/Speech/DeliveryNpcVoiceSet.cpp
AI/Presentation/DeliveryNpcVoiceSet.h|AI/Presentation/Speech/DeliveryNpcVoiceSet.h
AI/Presentation/DeliveryNpcBodyAnimInstance.cpp|AI/Presentation/Animation/DeliveryNpcBodyAnimInstance.cpp
AI/Presentation/DeliveryNpcBodyAnimInstance.h|AI/Presentation/Animation/DeliveryNpcBodyAnimInstance.h
AI/Presentation/DeliveryNpcConversationAnimInstance.cpp|AI/Presentation/Animation/DeliveryNpcConversationAnimInstance.cpp
AI/Presentation/DeliveryNpcConversationAnimInstance.h|AI/Presentation/Animation/DeliveryNpcConversationAnimInstance.h
AI/Presentation/DeliveryNpcExpression.cpp|AI/Presentation/Face/DeliveryNpcExpression.cpp
AI/Presentation/DeliveryNpcExpressionSet.h|AI/Presentation/Face/DeliveryNpcExpressionSet.h
AI/Presentation/DeliveryNpcLookComponent.cpp|AI/Presentation/Face/DeliveryNpcLookComponent.cpp
AI/Presentation/DeliveryNpcLookComponent.h|AI/Presentation/Face/DeliveryNpcLookComponent.h
AI/Presentation/DeliveryNpcReactions.cpp|AI/Presentation/Face/DeliveryNpcReactions.cpp
AI/Tests/DeliveryAiPIETests.cpp|AI/Tests/Resident/DeliveryAiPIETests.cpp
AI/Tests/DeliveryAiTests.cpp|AI/Tests/Resident/DeliveryAiTests.cpp
AI/Tests/DeliveryResidentPIETests.cpp|AI/Tests/Resident/DeliveryResidentPIETests.cpp
AI/Tests/DeliveryResidentTests.cpp|AI/Tests/Resident/DeliveryResidentTests.cpp
AI/Tests/DeliveryNpcNamingTests.cpp|AI/Tests/Resident/DeliveryNpcNamingTests.cpp
AI/Tests/DeliveryNpcProfileTests.cpp|AI/Tests/Resident/DeliveryNpcProfileTests.cpp
AI/Tests/DeliveryNpcSpacingTests.cpp|AI/Tests/Resident/DeliveryNpcSpacingTests.cpp
AI/Tests/DeliveryNpcTraversalTests.cpp|AI/Tests/Resident/DeliveryNpcTraversalTests.cpp
AI/Tests/DeliveryNpcBehaviorCatalogTests.cpp|AI/Tests/Resident/DeliveryNpcBehaviorCatalogTests.cpp
AI/Tests/DeliveryNpcDecisionTraceTests.cpp|AI/Tests/Resident/DeliveryNpcDecisionTraceTests.cpp
AI/Tests/DeliveryNpcLookTests.cpp|AI/Tests/Resident/DeliveryNpcLookTests.cpp
AI/Tests/DeliveryNpcBodyAnimationTests.cpp|AI/Tests/Resident/DeliveryNpcBodyAnimationTests.cpp
AI/Tests/DeliveryCompanionAssistTests.cpp|AI/Tests/Companion/DeliveryCompanionAssistTests.cpp
AI/Tests/DeliveryCompanionErrandTests.cpp|AI/Tests/Companion/DeliveryCompanionErrandTests.cpp
AI/Tests/DeliveryCompanionTests.cpp|AI/Tests/Companion/DeliveryCompanionTests.cpp
AI/Tests/DeliveryNpcConversationAimTests.cpp|AI/Tests/Conversation/DeliveryNpcConversationAimTests.cpp
AI/Tests/DeliveryNpcConversationContextTests.cpp|AI/Tests/Conversation/DeliveryNpcConversationContextTests.cpp
AI/Tests/DeliveryNpcConversationOcclusionTests.cpp|AI/Tests/Conversation/DeliveryNpcConversationOcclusionTests.cpp
AI/Tests/DeliveryNpcPlayerInterruptionTests.cpp|AI/Tests/Conversation/DeliveryNpcPlayerInterruptionTests.cpp
AI/Tests/DeliveryNpcModelSpeechSeparationTests.cpp|AI/Tests/Conversation/DeliveryNpcModelSpeechSeparationTests.cpp
AI/Tests/DeliveryNpcAudioStreamingTests.cpp|AI/Tests/Speech/DeliveryNpcAudioStreamingTests.cpp
AI/Tests/DeliveryNpcSentenceAudioProtocolTests.cpp|AI/Tests/Speech/DeliveryNpcSentenceAudioProtocolTests.cpp
AI/Tests/DeliveryNpcSpeechTests.cpp|AI/Tests/Speech/DeliveryNpcSpeechTests.cpp
AI/Tests/DeliveryNpcVoiceCodecTests.cpp|AI/Tests/Speech/DeliveryNpcVoiceCodecTests.cpp
AI/Tests/DeliveryNpcVoiceLinkTests.cpp|AI/Tests/Speech/DeliveryNpcVoiceLinkTests.cpp
AI/Tests/DeliveryNpcFightTests.cpp|AI/Tests/Fight/DeliveryNpcFightTests.cpp
AI/Tests/DeliveryNpcStrikeApproachTests.cpp|AI/Tests/Fight/DeliveryNpcStrikeApproachTests.cpp
AI/Tests/DeliveryNpcStrikeNavigationTests.cpp|AI/Tests/Fight/DeliveryNpcStrikeNavigationTests.cpp
Vehicle/DeliveryMotorbike.cpp|Vehicle/Motorbike/DeliveryMotorbike.cpp
Vehicle/DeliveryMotorbike.h|Vehicle/Motorbike/DeliveryMotorbike.h
Vehicle/DeliveryHornReceiver.h|Vehicle/Motorbike/DeliveryHornReceiver.h
Vehicle/DeliveryMotorbikeRider.cpp|Vehicle/Rider/DeliveryMotorbikeRider.cpp
Vehicle/DeliveryRiderAnimInstance.cpp|Vehicle/Rider/DeliveryRiderAnimInstance.cpp
Vehicle/DeliveryRiderAnimInstance.h|Vehicle/Rider/DeliveryRiderAnimInstance.h
Vehicle/DeliveryRiderPose.cpp|Vehicle/Rider/DeliveryRiderPose.cpp
Vehicle/DeliveryRiderPose.h|Vehicle/Rider/DeliveryRiderPose.h
Vehicle/DeliveryMotorbikeTrunk.cpp|Vehicle/Trunk/DeliveryMotorbikeTrunk.cpp
Vehicle/DeliveryMotorbikeCargo.cpp|Vehicle/Trunk/DeliveryMotorbikeCargo.cpp
Vehicle/DeliveryMotorbikePassenger.cpp|Vehicle/Trunk/DeliveryMotorbikePassenger.cpp
Vehicle/DeliveryTrunkPassengerComponent.cpp|Vehicle/Trunk/DeliveryTrunkPassengerComponent.cpp
Vehicle/DeliveryTrunkPassengerComponent.h|Vehicle/Trunk/DeliveryTrunkPassengerComponent.h
Vehicle/DeliveryCompanionMotorbike.cpp|Vehicle/Companion/DeliveryCompanionMotorbike.cpp
Vehicle/DeliveryCompanionMotorbike.h|Vehicle/Companion/DeliveryCompanionMotorbike.h
Vehicle/DeliveryVehicleSummonComponent.cpp|Vehicle/Summon/DeliveryVehicleSummonComponent.cpp
Vehicle/DeliveryVehicleSummonComponent.h|Vehicle/Summon/DeliveryVehicleSummonComponent.h
Vehicle/DeliveryBikeHornTests.cpp|Vehicle/Tests/DeliveryBikeHornTests.cpp
Vehicle/DeliveryBikeImpactPIETests.cpp|Vehicle/Tests/DeliveryBikeImpactPIETests.cpp
Vehicle/DeliveryBikePedestrianImpactTests.cpp|Vehicle/Tests/DeliveryBikePedestrianImpactTests.cpp
Vehicle/DeliveryMotorbikeObstacleTests.cpp|Vehicle/Tests/DeliveryMotorbikeObstacleTests.cpp
Vehicle/DeliveryMotorbikeTrunkTests.cpp|Vehicle/Tests/DeliveryMotorbikeTrunkTests.cpp
Vehicle/DeliveryRiderPoseTests.cpp|Vehicle/Tests/DeliveryRiderPoseTests.cpp
Vehicle/DeliveryTrunkPassengerPIETests.cpp|Vehicle/Tests/DeliveryTrunkPassengerPIETests.cpp
Vehicle/DeliveryTrunkPassengerTests.cpp|Vehicle/Tests/DeliveryTrunkPassengerTests.cpp
Vehicle/DeliveryVehicleSummonTests.cpp|Vehicle/Tests/DeliveryVehicleSummonTests.cpp
UI/DeliveryHUD.cpp|UI/HUD/DeliveryHUD.cpp
UI/DeliveryHUD.h|UI/HUD/DeliveryHUD.h
UI/DeliveryHotbarWidget.cpp|UI/HUD/DeliveryHotbarWidget.cpp
UI/DeliveryHotbarWidget.h|UI/HUD/DeliveryHotbarWidget.h
UI/DeliveryMinimapWidget.cpp|UI/HUD/DeliveryMinimapWidget.cpp
UI/DeliveryMinimapWidget.h|UI/HUD/DeliveryMinimapWidget.h
UI/DeliveryTaskHudComponent.cpp|UI/HUD/DeliveryTaskHudComponent.cpp
UI/DeliveryTaskHudComponent.h|UI/HUD/DeliveryTaskHudComponent.h
UI/DeliveryHintComponent.cpp|UI/HUD/DeliveryHintComponent.cpp
UI/DeliveryHintComponent.h|UI/HUD/DeliveryHintComponent.h
UI/DeliveryTrunkStatusWidget.cpp|UI/HUD/DeliveryTrunkStatusWidget.cpp
UI/DeliveryTrunkStatusWidget.h|UI/HUD/DeliveryTrunkStatusWidget.h
UI/DeliveryStartupWidget.cpp|UI/HUD/DeliveryStartupWidget.cpp
UI/DeliveryStartupWidget.h|UI/HUD/DeliveryStartupWidget.h
UI/DeliveryConversationWidget.cpp|UI/Conversation/DeliveryConversationWidget.cpp
UI/DeliveryConversationWidget.h|UI/Conversation/DeliveryConversationWidget.h
UI/DeliveryNpcCaptionWidget.cpp|UI/Conversation/DeliveryNpcCaptionWidget.cpp
UI/DeliveryNpcCaptionWidget.h|UI/Conversation/DeliveryNpcCaptionWidget.h
UI/DeliverySpeechStatusWidget.cpp|UI/Conversation/DeliverySpeechStatusWidget.cpp
UI/DeliverySpeechStatusWidget.h|UI/Conversation/DeliverySpeechStatusWidget.h
UI/DeliveryAiInspectorWidget.cpp|UI/Conversation/DeliveryAiInspectorWidget.cpp
UI/DeliveryAiInspectorWidget.h|UI/Conversation/DeliveryAiInspectorWidget.h
UI/DeliveryAiInspectorTests.cpp|UI/Tests/DeliveryAiInspectorTests.cpp
UI/DeliveryConversationFeedbackTests.cpp|UI/Tests/DeliveryConversationFeedbackTests.cpp
Combat/DeliveryAnimationArmPose.cpp|Combat/Pose/DeliveryAnimationArmPose.cpp
Combat/DeliveryAnimationArmPose.h|Combat/Pose/DeliveryAnimationArmPose.h
Combat/DeliveryBoxingPose.cpp|Combat/Pose/DeliveryBoxingPose.cpp
Combat/DeliveryBoxingPose.h|Combat/Pose/DeliveryBoxingPose.h
Combat/DeliveryBoxingPhysicsAsset.cpp|Combat/Pose/DeliveryBoxingPhysicsAsset.cpp
Combat/DeliveryHandPose.cpp|Combat/Pose/DeliveryHandPose.cpp
Combat/DeliveryHandPose.h|Combat/Pose/DeliveryHandPose.h
Combat/DeliveryPunchTrajectory.h|Combat/Pose/DeliveryPunchTrajectory.h
Combat/DeliveryCollisionChannels.h|Combat/Types/DeliveryCollisionChannels.h
Combat/DeliveryCombatEvents.cpp|Combat/Types/DeliveryCombatEvents.cpp
Combat/DeliveryCombatEvents.h|Combat/Types/DeliveryCombatEvents.h
Combat/DeliveryCombatInterface.h|Combat/Types/DeliveryCombatInterface.h
Combat/DeliveryCombatTypes.h|Combat/Types/DeliveryCombatTypes.h
Combat/DeliveryImpactReceiver.h|Combat/Types/DeliveryImpactReceiver.h
Combat/DeliveryNpcStrikeExecutor.h|Combat/Types/DeliveryNpcStrikeExecutor.h
Combat/DeliveryAxeAnimationTests.cpp|Combat/Tests/DeliveryAxeAnimationTests.cpp
Combat/DeliveryBoxingPoseTests.cpp|Combat/Tests/DeliveryBoxingPoseTests.cpp
Combat/DeliveryCombatTimingTests.cpp|Combat/Tests/DeliveryCombatTimingTests.cpp
Combat/DeliveryPunchRecoveryTests.cpp|Combat/Tests/DeliveryPunchRecoveryTests.cpp
World/DeliveryHome.cpp|World/Home/DeliveryHome.cpp
World/DeliveryHome.h|World/Home/DeliveryHome.h
World/DeliveryHomeBehaviorDefinition.cpp|World/Home/DeliveryHomeBehaviorDefinition.cpp
World/DeliveryHomeBehaviorDefinition.h|World/Home/DeliveryHomeBehaviorDefinition.h
World/DeliveryHomeClaimFeedback.cpp|World/Home/DeliveryHomeClaimFeedback.cpp
World/DeliveryHomeObjectComponent.cpp|World/Home/DeliveryHomeObjectComponent.cpp
World/DeliveryHomeObjectComponent.h|World/Home/DeliveryHomeObjectComponent.h
World/DeliveryHomeUseExecutor.h|World/Home/DeliveryHomeUseExecutor.h
World/DeliveryLocationComponent.cpp|World/Location/DeliveryLocationComponent.cpp
World/DeliveryLocationComponent.h|World/Location/DeliveryLocationComponent.h
World/DeliveryLocationMarker.cpp|World/Location/DeliveryLocationMarker.cpp
World/DeliveryLocationMarker.h|World/Location/DeliveryLocationMarker.h
World/DeliveryLocationRegistry.cpp|World/Location/DeliveryLocationRegistry.cpp
World/DeliveryLocationRegistry.h|World/Location/DeliveryLocationRegistry.h
World/DeliveryHomeObjectTests.cpp|World/Tests/DeliveryHomeObjectTests.cpp
Speech/DeliverySpeechAudioCapture.cpp|Speech/Capture/DeliverySpeechAudioCapture.cpp
Speech/DeliverySpeechAudioCapture.h|Speech/Capture/DeliverySpeechAudioCapture.h
Speech/DeliverySpeechAudioProcessor.cpp|Speech/Capture/DeliverySpeechAudioProcessor.cpp
Speech/DeliverySpeechAudioProcessor.h|Speech/Capture/DeliverySpeechAudioProcessor.h
Speech/DeliverySpeechRecognizer.cpp|Speech/Recognition/DeliverySpeechRecognizer.cpp
Speech/DeliverySpeechRecognizer.h|Speech/Recognition/DeliverySpeechRecognizer.h
Speech/DeliverySpeechSegmentation.h|Speech/Recognition/DeliverySpeechSegmentation.h
Speech/DeliverySpeechTurn.h|Speech/Recognition/DeliverySpeechTurn.h
Speech/DeliverySpeechPlayerContextTests.cpp|Speech/Tests/DeliverySpeechPlayerContextTests.cpp
Inventory/DeliveryCaterpillarItem.cpp|Inventory/Items/DeliveryCaterpillarItem.cpp
Inventory/DeliveryCaterpillarItem.h|Inventory/Items/DeliveryCaterpillarItem.h
Inventory/DeliveryHandheldItem.cpp|Inventory/Items/DeliveryHandheldItem.cpp
Inventory/DeliveryHandheldItem.h|Inventory/Items/DeliveryHandheldItem.h
Inventory/DeliveryPackageItem.cpp|Inventory/Items/DeliveryPackageItem.cpp
Inventory/DeliveryPackageItem.h|Inventory/Items/DeliveryPackageItem.h
Inventory/DeliveryTestAxe.cpp|Inventory/Items/DeliveryTestAxe.cpp
Inventory/DeliveryTestAxe.h|Inventory/Items/DeliveryTestAxe.h
Inventory/DeliveryCoreDebugCommands.cpp|Inventory/Debug/DeliveryCoreDebugCommands.cpp
Inventory/DeliveryInventoryDebugCommands.cpp|Inventory/Debug/DeliveryInventoryDebugCommands.cpp
NPC/BTTask_DeliveryGrandpaMoveOut.cpp|NPC/Grandpa/BTTask_DeliveryGrandpaMoveOut.cpp
NPC/BTTask_DeliveryGrandpaMoveOut.h|NPC/Grandpa/BTTask_DeliveryGrandpaMoveOut.h
NPC/DeliveryGrandpa.cpp|NPC/Grandpa/DeliveryGrandpa.cpp
NPC/DeliveryGrandpa.h|NPC/Grandpa/DeliveryGrandpa.h
NPC/DeliveryGrandpaHome.cpp|NPC/Grandpa/DeliveryGrandpaHome.cpp
NPC/DeliveryPedestrian.cpp|NPC/Pedestrian/DeliveryPedestrian.cpp
NPC/DeliveryPedestrian.h|NPC/Pedestrian/DeliveryPedestrian.h
NPC/DeliveryPedestrianRoute.cpp|NPC/Pedestrian/DeliveryPedestrianRoute.cpp
NPC/DeliveryPedestrianRoute.h|NPC/Pedestrian/DeliveryPedestrianRoute.h
NPC/DeliveryGrandpaTests.cpp|NPC/Tests/DeliveryGrandpaTests.cpp
Audio/DeliveryGameplayAudioDebugCommands.cpp|Audio/Debug/DeliveryGameplayAudioDebugCommands.cpp
Audio/DeliveryMusicDebugCommands.cpp|Audio/Debug/DeliveryMusicDebugCommands.cpp
Audio/DeliveryVoiceDebugCommands.cpp|Audio/Debug/DeliveryVoiceDebugCommands.cpp
Input/DeliveryInputTests.cpp|Input/Tests/DeliveryInputTests.cpp
Input/DeliveryRightMouseGestureTests.cpp|Input/Tests/DeliveryRightMouseGestureTests.cpp
Interaction/DeliveryInteractionGeometryTests.cpp|Interaction/Tests/DeliveryInteractionGeometryTests.cpp
"""

CODE_EXT = {".h", ".hpp", ".cpp", ".inl"}
DOC_EXT = {".md", ".html", ".json", ".txt", ".tex"}
INCLUDE_RE = re.compile(r'(#\s*include\s+")([^"]+)(")')


def read_text(path):
    data = path.read_bytes()
    bom = data.startswith(b"\xef\xbb\xbf")
    # newline="" 的效果：保留原来的 CRLF
    return data[3 if bom else 0:].decode("utf-8"), bom


def write_text(path, text, bom):
    path.write_bytes((b"\xef\xbb\xbf" if bom else b"") + text.encode("utf-8"))


def norm(rel):
    rel = posixpath.normpath(rel)
    return None if rel.startswith("..") or rel == "." else rel


def editor_running():
    if sys.platform != "win32":
        return False
    out = subprocess.run(["tasklist"], capture_output=True, text=True, errors="ignore").stdout
    return "UnrealEditor" in out


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--apply", action="store_true", help="真正执行；不加只预览")
    args = parser.parse_args()

    root = Path.cwd()
    if not (root / "Delivery.uproject").exists():
        sys.exit("请在仓库根目录（Delivery.uproject 所在目录）运行")
    if args.apply and editor_running():
        sys.exit("请先关闭 UE 编辑器再执行")
    mod = root / "Source" / "Delivery"

    moves = {}
    for line in MOVE_TEXT.strip().splitlines():
        old, new = line.split("|")
        moves[old] = new

    # 1 检查清单：旧文件都在，新位置都空着
    missing = [k for k in moves if not (mod / k).exists()]
    taken = [v for v in moves.values() if (mod / v).exists()]
    if len(missing) == len(moves):
        sys.exit("清单里的文件一个都不在，可能已经执行过")
    if taken:
        print("以下新位置已有文件：")
        print("\n".join("  " + t for t in taken))
        sys.exit("新位置被占用，停止")
    if missing:
        print("以下旧文件不存在，跳过（可能已改名或已搬过）：")
        print("\n".join("  " + m for m in missing))
        for m in missing:
            del moves[m]

    def rel(p):
        return p.relative_to(mod).as_posix()

    # 2 改写 #include：在搬家前按旧位置解析，所以相对路径也能找对
    mod_files = {rel(p) for p in mod.rglob("*") if p.is_file() and p.suffix in CODE_EXT}
    code_files = [p for p in mod.rglob("*") if p.is_file() and p.suffix in CODE_EXT]
    plugins = root / "Plugins"
    if plugins.exists():
        code_files += [p for p in plugins.rglob("*") if p.is_file() and p.suffix in CODE_EXT]

    inc_count = 0
    inc_files = 0
    for f in code_files:
        self_rel = rel(f) if mod in f.parents else None
        self_moved = self_rel in moves
        here = posixpath.dirname(self_rel) if self_rel is not None else None
        text, bom = read_text(f)
        count = 0

        def fix(m):
            nonlocal count
            inc = m.group(2).replace("\\", "/")
            target = None
            # 先按包含者所在目录解析，再按 Source/Delivery 根目录解析
            if here is not None:
                cand = norm(posixpath.join(here, inc))
                if cand in mod_files:
                    target = cand
            if target is None:
                cand = norm(inc)
                if cand in mod_files:
                    target = cand
            if target is None:
                return m.group(0)
            if target not in moves and not self_moved:
                return m.group(0)
            new_rel = moves.get(target, target)
            if new_rel == inc:
                return m.group(0)
            count += 1
            return m.group(1) + new_rel + m.group(3)

        new_text = INCLUDE_RE.sub(fix, text)
        if count:
            inc_count += count
            inc_files += 1
            if args.apply:
                write_text(f, new_text, bom)
            else:
                print(f"  include  {f.relative_to(root).as_posix()}  ({count} 处)")

    # 3 改写文档里的路径：先换带扩展名的完整路径，再换不带扩展名的写法，都按长度从长到短
    doc_files = [root / n for n in ("CLAUDE.md", "AGENTS.md", "README.md") if (root / n).exists()]
    if (root / "Document").exists():
        doc_files += [p for p in (root / "Document").rglob("*") if p.is_file() and p.suffix in DOC_EXT]
    doc_files += list((root / "Source").rglob("*.md"))

    full_pairs = sorted(moves.items(), key=lambda kv: -len(kv[0]))
    stems = {}
    for old, new in moves.items():
        stems.setdefault(re.sub(r"\.(h|cpp)$", "", old), re.sub(r"\.(h|cpp)$", "", new))
    stem_pairs = sorted(stems.items(), key=lambda kv: -len(kv[0]))

    doc_count = 0
    for f in doc_files:
        text, bom = read_text(f)
        t = text
        for old, new in full_pairs:
            t = t.replace("Source/Delivery/" + old, "Source/Delivery/" + new)
            t = t.replace("Source\\Delivery\\" + old.replace("/", "\\"), "Source\\Delivery\\" + new.replace("/", "\\"))
            t = re.sub(r"(?<![\w/\\])" + re.escape(old) + r"(?!\w)", lambda _m, n=new: n, t)
        for old, new in stem_pairs:
            t = re.sub(r"(?<![\w/\\])" + re.escape(old) + r"(?![\w])", lambda _m, n=new: n, t)
        if t != text:
            doc_count += 1
            if args.apply:
                write_text(f, t, bom)
            else:
                print(f"  文档     {f.relative_to(root).as_posix()}")

    # 4 搬文件，git mv 保留历史
    for old, new in moves.items():
        if args.apply:
            (mod / new).parent.mkdir(parents=True, exist_ok=True)
            r = subprocess.run(["git", "mv", "--", str(mod / old), str(mod / new)])
            if r.returncode != 0:
                sys.exit(f"git mv 失败：{old}")
        else:
            print(f"  搬       {old}  ->  {new}")

    mode = "已执行" if args.apply else "预览（加 --apply 才会执行）"
    print(f"\n{mode}：搬 {len(moves)} 个文件，改 {inc_files} 个源文件里的 {inc_count} 处 include，改 {doc_count} 个文档")
    if args.apply:
        print("下一步：编译 DeliveryEditor，然后 git status 检查改动")


if __name__ == "__main__":
    main()
