#include "AI/Presentation/DeliveryNpcPresentationComponent.h"
#include "AI/Presentation/Face/DeliveryNpcExpressionSet.h"
#include "AI/Presentation/Animation/DeliveryNpcConversationAnimInstance.h"
#include "AI/Presentation/Face/DeliveryNpcLookComponent.h"
#include "AI/Actions/DeliveryNpcCombatComponent.h"
#include "AI/Characters/DeliveryNpcCharacter.h"
#include "AI/Decision/DeliveryNpcAgentComponent.h"
#include "Components/SkeletalMeshComponent.h"
#include "Curves/CurveFloat.h"
#include "GameFramework/Pawn.h"

// ── 表情 ──
// TickExpression 每帧决定 NPC 现在用哪个表情，把表情对应的 MorphWeights（脸部形变权重）平滑过渡到模型上
// 同时让眼睛跟着动画蓝图看的方向转，并定时眨眼；表情配置来自 Expressions（UDeliveryNpcExpressionSet）

/// @brief NPC 刚开始倾听的 ListeningReactionDuration 秒内返回 true，这段时间显示 Listening 表情
/// 被打倒、正在起身、出拳、受击后仰、拽车时返回 false
bool UDeliveryNpcPresentationComponent::IsShowingListeningReaction() const
{
    const auto* Npc=Cast<ADeliveryNpcCharacter>(GetOwner());
    if (!Npc || !Npc->Agent || !Expressions || !Npc->Agent->IsListening() || Npc->IsKnockedDown() ||
        Npc->Combat->IsRecovering() || Npc->Combat->IsStriking() || Npc->Combat->IsReactingToHit() || Npc->GetHeldVehicle()) return false;
    const double Started=Npc->Agent->GetListeningStartTime();
    return FMath::IsFinite(Started) && Started>=0 &&
        ServerTime()-Started<FMath::Clamp(Expressions->ListeningReactionDuration,.1f,2.f);
}

/// @brief 正在播的台词声音没收完、播放暂停等后面的数据（bRebuffering）时返回 true
/// 要求本机第一段声音和屏幕上显示的台词是同一句、同一个回复，而且这个回复没有被取消或播完
bool UDeliveryNpcPresentationComponent::IsWaitingForMoreAudio() const
{
    if (!DisplayedSpeech.bActive || !DisplayedSpeech.bRecordedVoice || LocalVoices.IsEmpty()) return false;
    const auto& Local=LocalVoices[0];
    return Local.bRebuffering && Local.State.bActive && Local.State.bRecordedVoice &&
        Local.State.TurnId==DisplayedSpeech.TurnId && Local.State.ActionId==DisplayedSpeech.ActionId &&
        !CancelledVoiceActions.Contains(Local.State.ActionId) && !FinishedVoiceTurns.Contains(Local.State.TurnId);
}

/// @brief 返回当前动作带的表情 Emotion：动作已接受或正在执行时直接返回
/// 动作结束后，只有它的回复还在等声音或正要播放时才继续返回，否则返回 NAME_None
FName UDeliveryNpcPresentationComponent::GetActionEmotion() const
{
    const auto* Npc=Cast<ADeliveryNpcCharacter>(GetOwner());
    if (!Npc || !Npc->Agent || Npc->IsKnockedDown()) return NAME_None;
    const auto State=Npc->Agent->GetPublicState();
    if (State.Status==EDeliveryNpcActionStatus::Accepted || State.Status==EDeliveryNpcActionStatus::Running) return State.Emotion;
    if (State.Emotion.IsNone() || State.Emotion==TEXT("Calm") || !IsValid(State.SpeechTarget)) return NAME_None;

    // 声音还在后端合成、本机还没收到时，用服务器同步的 GetReplyId 和 ReplyAction 比较；回复已经在 CancelledVoiceActions 里就不用这个表情
    const bool bThisReply=State.GetReplyId().IsValid() && ReplyAction==State.GetReplyId() &&
        !CancelledVoiceActions.Contains(State.GetReplyId());
    return bThisReply || IsWaitingForSpeechPlayback()?State.Emotion:NAME_None;
}

/// @brief 每帧选表情并更新脸部形变
/// 表情按顺序选：被打倒 Down、受击 HitExpression、说台词时用台词的表情、刚开始倾听 Listening、被吓到 StartleExpression
/// 然后是动作带的表情，最后是倾听中、等模型回复、等声音时的 Thinking；都不满足就是 Calm
void UDeliveryNpcPresentationComponent::TickExpression(float DeltaTime)
{
    auto* Npc=Cast<ADeliveryNpcCharacter>(GetOwner());
    if (!Npc || !Expressions) return;
    const auto Display=GetDisplayedSpeech();
    const FName ActionEmotion=GetActionEmotion();
    FName Pose=TEXT("Calm");
    if (Npc->IsKnockedDown()) Pose=TEXT("Down");
    else if (Npc->Combat->IsReactingToHit() && (!Npc->Look || !Npc->Look->HitExpression.IsNone()))
        Pose=Npc->Look?Npc->Look->HitExpression:FName(TEXT("Surprised"));
    else if (Display.bActive) Pose=Display.Emotion;
    else if (IsShowingListeningReaction()) Pose=TEXT("Listening");
    else if (Npc->Look && Npc->Look->IsStartled() && !Npc->Look->StartleExpression.IsNone()) Pose=Npc->Look->StartleExpression;

    // 动作带的表情（例如生气）在走过去、踱步时就显示，不等台词声音开始
    else if (!ActionEmotion.IsNone() && ActionEmotion!=TEXT("Calm")) Pose=ActionEmotion;
    else if (Npc->Agent->IsListening() || Npc->Agent->IsDecisionPending() || IsWaitingForSpeechPlayback()) Pose=TEXT("Thinking");
    TMap<FName,float> Targets;
    if (const auto* Settings=Expressions->Poses.Find(Pose)) Targets=Settings->MorphWeights;

    // 眼睛看向动画蓝图 UDeliveryNpcConversationAnimInstance 的 LookTargetWorld；头已经转过去一部分，眼睛只补剩下的角度
    // 角度除以 MaxEyeYaw、MaxEyePitch 换算成 0 到 1 的形变权重，再乘 LookWeight
    const auto* LookAnimation=Cast<UDeliveryNpcConversationAnimInstance>(Npc->GetMesh()->GetPostProcessInstance());
    const bool bCanMoveEyes=!Npc->IsKnockedDown() && !Npc->Combat->IsRecovering() && !Npc->Combat->IsStriking() && !Npc->GetHeldVehicle();
    if (bCanMoveEyes && LookAnimation && LookAnimation->LookWeight>.01f)
    {
        const FRotator Direction=LookAnimation->GetHeadRelativeLookDirection(LookAnimation->LookTargetWorld).Rotation();
        const float Yaw=FMath::Clamp(Direction.Yaw/FMath::Max(1.f,Expressions->MaxEyeYaw),-1.f,1.f);
        const float Pitch=FMath::Clamp(Direction.Pitch/FMath::Max(1.f,Expressions->MaxEyePitch),-1.f,1.f);
        Targets.Add(Expressions->LookLeftMorph,FMath::Max(0.f,-Yaw)*LookAnimation->LookWeight);
        Targets.Add(Expressions->LookRightMorph,FMath::Max(0.f,Yaw)*LookAnimation->LookWeight);
        Targets.Add(Expressions->LookUpMorph,FMath::Max(0.f,Pitch)*LookAnimation->LookWeight);
        Targets.Add(Expressions->LookDownMorph,FMath::Max(0.f,-Pitch)*LookAnimation->LookWeight);
    }

    // 眨眼：到 NextBlink 时开始一次，下一次在 BlinkInterval 范围内随机；BlinkCurve 为空时用前 28% 闭眼、后面睁开的默认曲线
    const double Now=ServerTime();
    if (NextBlink==0) NextBlink=Now+FMath::FRandRange(1.f,3.f);
    if (Now>=NextBlink)
    {
        BlinkStarted=Now;
        NextBlink=Now+FMath::FRandRange(FMath::Max(.5,Expressions->BlinkInterval.X),FMath::Max(Expressions->BlinkInterval.X,Expressions->BlinkInterval.Y));
    }
    const float Progress=float((Now-BlinkStarted)/FMath::Max(.1f,Expressions->BlinkDuration));
    float Blink=0;
    if (BlinkStarted>=0 && Progress>=0 && Progress<=1)
    {
        if (Expressions->BlinkCurve) Blink=Expressions->BlinkCurve->GetFloatValue(Progress);
        else if (Progress<.28f) Blink=FMath::SmoothStep(0.f,.28f,Progress);
        else Blink=1.f-FMath::SmoothStep(.28f,1.f,Progress);
    }
    Targets.FindOrAdd(Expressions->BlinkMorph)=FMath::Max(Targets.FindRef(Expressions->BlinkMorph),FMath::Clamp(Blink,0.f,1.f));
    const float EyeOpenWeight=1.f-FMath::Clamp(Targets.FindRef(Expressions->BlinkMorph),0.f,1.f);

    // 把这一帧用到的形变名都加进 ExpressionWeights，没用到的会平滑回到 0
    for (const auto& Pair:Targets) ExpressionWeights.FindOrAdd(Pair.Key);
    for (auto& Pair:ExpressionWeights)
    {
        // 眨眼的权重直接用曲线值，不平滑，闭眼时能完全合上；其他形变按 BlendSpeed 平滑过渡
        const float Target=FMath::Clamp(Targets.FindRef(Pair.Key),0.f,1.f);
        const bool bBlink=Pair.Key==Expressions->BlinkMorph;
        Pair.Value=bBlink?Target:FMath::FInterpTo(Pair.Value,Target,DeltaTime,Expressions->BlendSpeed);

        // 闭眼时其他形变乘 EyeOpenWeight 变小，免得眨眼和情绪表情一起挤压眼睛，模型穿面
        // 只缩小显示的权重，Pair.Value 里的过渡进度不变，睁眼后接着显示原来的情绪
        // 眨眼形变本身和 BrowsAngry（眉毛）不缩小
        const bool bKeepWeight=bBlink || Pair.Key==TEXT("BrowsAngry");
        const float DisplayWeight=bKeepWeight?Pair.Value:Pair.Value*EyeOpenWeight;
        if (!Pair.Key.IsNone()) Npc->GetMesh()->SetMorphTarget(Pair.Key,DisplayWeight);
    }
}
