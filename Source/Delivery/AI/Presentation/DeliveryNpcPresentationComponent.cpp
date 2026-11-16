#include "AI/Presentation/DeliveryNpcPresentationComponent.h"
#include "AI/Presentation/Animation/DeliveryNpcBodyAnimInstance.h"
#include "AI/Characters/DeliveryNpcCharacter.h"
#include "AI/Actions/DeliveryNpcCombatComponent.h"
#include "AI/Decision/DeliveryNpcAgentComponent.h"
#include "Audio/DeliveryDialogueVoiceComponent.h"
#include "Components/SkeletalMeshComponent.h"
#include "Components/WidgetComponent.h"
#include "Components/AudioComponent.h"
#include "UI/Conversation/DeliveryNpcCaptionWidget.h"
#include "Animation/AnimSequence.h"
#include "GameFramework/GameStateBase.h"
#include "Kismet/GameplayStatics.h"
#include "Camera/PlayerCameraManager.h"
#include "EngineUtils.h"
#include "Engine/World.h"
#include "Net/UnrealNetwork.h"

// ── 生命周期 ──
// BeginPlay 在客户端（和单机）建好电子语音组件 Voice 和头顶字幕 Caption；EndPlay 停掉所有声音、取消后台压缩并清空记录

/// @brief 组件要同步 Speech 和 ReplyAction 给客户端，并每帧更新表情、字幕和动画
UDeliveryNpcPresentationComponent::UDeliveryNpcPresentationComponent()
{
    SetIsReplicatedByDefault(true); PrimaryComponentTick.bCanEverTick=true;
}

/// @brief 服务器的世界时间；Speech.StartTime 等时间都按它记录，客户端用它算播放进度
double UDeliveryNpcPresentationComponent::ServerTime() const
{
    if (const auto* GS=GetWorld()->GetGameState()) return GS->GetServerWorldTimeSeconds();
    return GetWorld()->GetTimeSeconds();
}

/// @brief 让网格在这个组件和战斗组件之后再更新，动画读到的是这一帧的状态
/// 不是专用服务器时建 Voice（电子语音，150 cm 内不衰减、800 cm 内衰减到听不见）和 Caption（头顶 100 cm 处的屏幕空间字幕）
void UDeliveryNpcPresentationComponent::BeginPlay()
{
    Super::BeginPlay();
    if (auto* Npc=Cast<ADeliveryNpcCharacter>(GetOwner()))
    {
        // AddTickPrerequisiteComponent：网格等这个组件和 Combat 更新完再更新，动画蓝图读到的是这一帧的说话和战斗状态
        Npc->GetMesh()->AddTickPrerequisiteComponent(this);
        Npc->GetMesh()->AddTickPrerequisiteComponent(Npc->Combat);
    }
    if (GetNetMode()!=NM_DedicatedServer)
    {
        Voice=NewObject<UDeliveryDialogueVoiceComponent>(GetOwner());
        Voice->bAllowWorldSpeaker=true; Voice->bAllowSpatialization=true; Voice->bOverrideAttenuation=true;
        Voice->AttenuationOverrides.bAttenuate=true; Voice->AttenuationOverrides.AttenuationShapeExtents=FVector(150,0,0); Voice->AttenuationOverrides.FalloffDistance=800; Voice->SetupAttachment(GetOwner()->GetRootComponent()); Voice->RegisterComponent();
        Caption=NewObject<UWidgetComponent>(GetOwner()); Caption->SetupAttachment(GetOwner()->GetRootComponent());
        Caption->SetRelativeLocation(FVector(0,0,100)); Caption->SetWidgetSpace(EWidgetSpace::Screen);
        auto* CaptionClass=LoadClass<UDeliveryNpcCaptionWidget>(nullptr,TEXT("/Game/UI/Conversation/WBP_NpcCaption.WBP_NpcCaption_C"));
        if (!CaptionClass)
            UE_LOG(LogTemp,Warning,TEXT("NPC caption blueprint missing: /Game/UI/Conversation/WBP_NpcCaption; voice playback remains enabled"));

        // 找不到 WBP_NpcCaption 时 Caption 照样注册：OnRep_Speech 要求 Voice 和 Caption 都存在才播放电子语音
        Caption->SetWidgetClass(CaptionClass); Caption->SetDrawSize(FVector2D(400,100)); Caption->SetDrawAtDesiredSize(true);
        Caption->SetCollisionEnabled(ECollisionEnabled::NoCollision); Caption->RegisterComponent();
    }
    OnRep_Speech();
}

// ── 回复 ──
// 一轮回复有两种播法：模型回复带后端合成的声音时，由 Speech 文件里的流式代码逐句播放
// 只有文字的完整回复（游戏规则安排的短台词，或者模型回复没有声音）由 StartCompleteReply 开始，客户端用电子语音 Voice 逐字念出来
// 当前还在说别的话时，QueueCompleteReply 把完整回复排到当前这轮后面；CancelSpeech 打断正在说的回复

/// @brief 服务器：让 NPC 说一句完整的台词
/// ModelReplyId 有效时，这句必须和 Agent 保存的那轮模型回复一致，交给 QueueCompleteReply 排队
/// 否则按游戏规则的短台词处理：用新编号，收话人取 PublicState.SpeechTarget，调用 StartCompleteReply 马上开始
/// @return 文字不合规、层数超过 3、和保存的模型回复对不上时返回 false
bool UDeliveryNpcPresentationComponent::PublishSpeech(const FString& Text,FName Emotion,int32 Depth,FGuid ReplyToEventId,FGuid ModelReplyId)
{
    if (!GetOwner()->HasAuthority() || !DeliveryNpcMessages::IsValidReplyText(Text,false) || Depth<0 || Depth>3) return false;
    FDeliveryNpcCompleteReply Reply;
    const auto* Npc=Cast<ADeliveryNpcCharacter>(GetOwner());
    if (ModelReplyId.IsValid())
    {
        if (!Npc || !Npc->Agent->GetCompleteReply(ModelReplyId,Reply) || Reply.Text!=Text || Reply.MessageId!=ReplyToEventId) return false;
        return QueueCompleteReply(Reply);
    }
    if (Npc && Npc->Agent->GetCompleteReply(Npc->Agent->GetPublicState().GetReplyId(),Reply))
    {
        if (Reply.Text!=Text || Reply.MessageId!=ReplyToEventId) return false;
        return QueueCompleteReply(Reply);
    }

    // 游戏规则安排的短台词：新生成一个编号；当前有模型回复时沿用它的编号和收话人
    Reply.ReplyId=FGuid::NewGuid(); Reply.MessageId=ReplyToEventId;
    Reply.Text=Text; Reply.Emotion=Emotion; Reply.Depth=Depth;
    if (Npc)
    {
        const auto State=Npc->Agent->GetPublicState();
        Reply.Listener=State.SpeechTarget;
        if (State.GetReplyId().IsValid()) Reply.ReplyId=State.GetReplyId();
    }
    return StartCompleteReply(Reply);
}

/// @brief 服务器：开始说一句完整台词，写好 Speech 并同步给客户端；正在说别的话时返回 false
/// 显示时长按字数算：每字 0.075 秒加 0.5 秒，限制在 1 到 10 秒
bool UDeliveryNpcPresentationComponent::StartCompleteReply(const FDeliveryNpcCompleteReply& Reply)
{
    if (!GetOwner()->HasAuthority() || Speech.bActive || !Reply.ReplyId.IsValid()) return false;
    Speech={}; Speech.TurnId=Reply.ReplyId; Speech.Text=Reply.Text; Speech.Emotion=Reply.Emotion;
    Speech.ReplyToEventId=Reply.MessageId; ReplyListener=Reply.Listener;
    Speech.RadioListener=ResolveRadioListener(Reply.MessageId);
    Speech.StartTime=ServerTime(); Speech.Duration=FMath::Clamp(Reply.Text.Len()*.075f+.5f,1.f,10.f); Speech.bActive=true;
    SpeechDepth=Reply.Depth; GetOwner()->ForceNetUpdate(); OnRep_Speech();
    UE_LOG(LogTemp,Log,TEXT("ResidentSpeech started: npc=%s turn=%s replyTo=%s length=%d"),
        *GetNameSafe(GetOwner()),*Speech.TurnId.ToString(),*Speech.ReplyToEventId.ToString(),Speech.Text.Len());
    return true;
}

/// @brief 服务器：安排一轮完整回复；这轮还没登记就先调用 BeginReply
/// 这轮是当前轮（ReplyAction）时存进 DeferredCompleteReply 并试着开始；是排在后面的下一轮时存进 PendingReply
bool UDeliveryNpcPresentationComponent::QueueCompleteReply(const FDeliveryNpcCompleteReply& Reply)
{
    if (!GetOwner()->HasAuthority() || !Reply.ReplyId.IsValid() || !DeliveryNpcMessages::IsValidReplyText(Reply.Text,false)) return false;

    // 这句已经在说了（重复调用），直接返回
    if (Speech.bActive && !Speech.ActionId.IsValid() && Speech.TurnId==Reply.ReplyId) return true;
    if (!HasReply(Reply.ReplyId)) BeginReply(Reply.ReplyId,Reply.MessageId,Reply.Depth,Reply.Listener.Get());
    if (ReplyAction==Reply.ReplyId)
    {
        DeferredCompleteReply=Reply;
        StartDeferredCompleteReply();
        return true;
    }
    if (PendingReply.ReplyId==Reply.ReplyId) { PendingReply.CompleteReply=Reply; return true; }
    return false;
}

/// @brief 服务器：当前轮是一轮完整回复、没在说别的、Agent 的 CanStartReplySpeech 也同意时，开始说它
bool UDeliveryNpcPresentationComponent::StartDeferredCompleteReply()
{
    const auto* Npc=Cast<ADeliveryNpcCharacter>(GetOwner());
    if (!Npc || Speech.bActive || !DeferredCompleteReply.ReplyId.IsValid() ||
        DeferredCompleteReply.ReplyId!=ReplyAction || !Npc->Agent->CanStartReplySpeech(ReplyAction)) return false;
    const auto Reply=MoveTemp(DeferredCompleteReply);
    DeferredCompleteReply={};

    // 清掉流式回复用的 ReplyAction 等记录再调用 StartCompleteReply；这里不调用 Agent->CancelReply，Agent 那边这轮回复仍然有效
    ReplyAction.Invalidate(); ReplyMessage.Invalidate();
    ReplyGameplayAction.Invalidate(); ReplyTarget.Reset(); bReplyHadTarget=false;
    ExpectedSentences=INDEX_NONE; NextSentence=0;
    return StartCompleteReply(Reply);
}

/// @brief 服务器：打断 NPC 正在说的回复，清掉排队的下一轮、后台压缩和反应音，最后调用 Agent->CancelReply
/// @param bFinishCurrentSentence true 时当前这句说完再停，已经压好的声音保留给还在补听的客户端
/// @param bPreserveOtherReplies true 时只停当前这轮，ServerVoices 里其他轮的声音保留
void UDeliveryNpcPresentationComponent::CancelSpeech(bool bFinishCurrentSentence,bool bPreserveOtherReplies)
{
    if (!GetOwner()->HasAuthority()) return;
    DiscardPendingReply();
    UE_LOG(LogTemp,Log,TEXT("ResidentSpeech cancelled: npc=%s turn=%s replyTo=%s"),
        *GetNameSafe(GetOwner()),*Speech.TurnId.ToString(),*Speech.ReplyToEventId.ToString());

    // FinishSentence 记下已经说完的句子；RecordSpokenReply(false) 把这些写进记忆，被打断的半句不写
    FinishSentence();
    RecordSpokenReply(false);
    CancelSentenceGeneration();
    StopReaction();

    // 停止通知带着这一轮的编号 StoppedReply，客户端只停这一轮；通知晚到时，已经开始的新回复不受影响
    const FGuid StoppedReply=ReplyAction.IsValid()?ReplyAction:Speech.ActionId.IsValid()?Speech.ActionId:Speech.TurnId;
    if (StoppedReply.IsValid())
    {
        if (bFinishCurrentSentence) FinishReplyAfterSentence(StoppedReply);
        else StopStreamSpeech(StoppedReply,!bPreserveOtherReplies);
    }
    ReplyAction.Invalidate(); ReplyMessage.Invalidate(); Sentences.Reset(); EncodedSentences.Reset(); SpokenReply.Reset();
    ReplyListener.Reset();
    DeferredCompleteReply={};
    ReplyGameplayAction.Invalidate(); ReplyTarget.Reset(); bReplyHadTarget=false;
    NextSentence=0; ExpectedSentences=INDEX_NONE; SendingAudio.Reset(); SendingOffset=0;
    if (bFinishCurrentSentence)
    {
        // 当前这句说完再停时：已经压完的声音按原来的 5 秒期限保留，刚走近的客户端还能补听；没压完的删掉
        for (auto It=ServerVoices.CreateIterator();It;++It)
            if (!It.Value().bComplete || It.Value().ExpiresAt<=0) It.RemoveCurrent();
        TrimVoiceCache();
    }
    else if (bPreserveOtherReplies) ServerVoices.Remove(Speech.TurnId);
    else ServerVoices.Reset();
    Speech.bActive=false; GetOwner()->ForceNetUpdate(); OnRep_Speech();
    if (auto* Npc=Cast<ADeliveryNpcCharacter>(GetOwner())) Npc->Agent->CancelReply(StoppedReply);
}

/// @brief Speech 变化时调用（服务器上手动调用，客户端收到同步时调用）
/// ActionId 有效是流式回复，交给 AcceptLocalSpeech；否则是完整回复，用电子语音 Voice 从当前进度开始念
void UDeliveryNpcPresentationComponent::OnRep_Speech()
{
    if (!Speech.ActionId.IsValid() && CancelledVoiceActions.Contains(Speech.TurnId)) return;
    if (Speech.ActionId.IsValid())
    {
        // 这轮已经在 CancelledVoiceActions 里：Speech 的同步比停止通知晚到，不再播放
        if (CancelledVoiceActions.Contains(Speech.ActionId)) return;
        AcceptLocalSpeech(Speech,true);
        return;
    }
    if (!Voice || !Caption) return;
    if (!Speech.bActive || ServerTime()>Speech.StartTime+Speech.Duration)
    { Voice->StopSpeaking(); if (RecordedVoice) RecordedVoice->Stop(); return; }
    if (PlayedTurn==Speech.TurnId) return;
    PlayedTurn=Speech.TurnId;
    Voice->StopSpeaking();
    if (RecordedVoice) RecordedVoice->Stop();
    if (Speech.bRecordedVoice) return;

    // 晚收到的客户端按已经过去的时间跳过前面的字，每字 0.075 秒
    const int32 Skip=FMath::Clamp(int32((ServerTime()-Speech.StartTime)/.075),0,Speech.Text.Len());
    StopReaction();

    // 本机玩家是开麦对话的主人时不按距离衰减，声音直接传到耳边
    const bool bRadio=IsLocalRadioListener(Speech);
    Voice->bAllowSpatialization=!bRadio;
    Voice->AttenuationOverrides.bAttenuate=!bRadio;
    Voice->AttenuationOverrides.bSpatialize=!bRadio;
    auto Settings=UDeliveryDialogueVoiceComponent::MakePresetSettings(Speech.Emotion==TEXT("Angry")?EDeliveryVoicePreset::Angry:EDeliveryVoicePreset::Normal);
    Settings.BaseFrequency=VoiceFrequency;
    Voice->SpeakTextWithSettings(FText::FromString(Speech.Text.Mid(Skip)),Settings);

}

/// @brief 每帧：更新反应音和流式句子；服务器上完整回复说完时结束它
/// 客户端（和单机）上再更新表情、口型、生气的形变和头顶字幕；动画蓝图不是 UDeliveryNpcBodyAnimInstance 时用旧的单片段动画
void UDeliveryNpcPresentationComponent::TickComponent(float Dt,ELevelTick Type,FActorComponentTickFunction* Fn)
{
    Super::TickComponent(Dt,Type,Fn);
    TickReaction();

    // 电子语音正按开麦方式播放，但本机玩家已经不是收听的主人了，就停下
    if (Voice && !Voice->bAllowSpatialization && !IsLocalRadioListener(Speech)) Voice->StopSpeaking();
    auto* Resident=Cast<ADeliveryNpcCharacter>(GetOwner()); if (!Resident) return;
    TickSentences();
    if (GetOwner()->HasAuthority() && !ReplyAction.IsValid() && Speech.bActive && ServerTime()>=Speech.StartTime+Speech.Duration)
    {
        // 完整回复说完：写一条 SelfSpeech 事件进记忆，调用 FinishReplyPlayback 告诉 Agent 这轮结束
        Speech.bActive=false; GetOwner()->ForceNetUpdate(); OnRep_Speech();
        FDeliveryNpcObservation Said; Said.Kind=TEXT("SelfSpeech");
        Said.Source=EDeliveryNpcEvidenceSource::ActionResult; Said.Description=Speech.Text; Said.Lifetime=60;
        const TWeakObjectPtr<AActor> CompletedListener(Resident->Agent->GetReplyListenerForPlayback(Speech.TurnId,ReplyListener.Get()));
        Said.Subject=Resident; Said.Addressee=CompletedListener.Get();
        if (Resident->Agent->CanLinkReplyPlayback(Speech.TurnId)) Said.ReplyToEventId=Speech.ReplyToEventId;
        Said.EventId=Speech.TurnId;
        UE_LOG(LogTemp,Log,TEXT("ResidentSpeech completed: npc=%s turn=%s replyTo=%s"),
            *GetNameSafe(GetOwner()),*Speech.TurnId.ToString(),*Speech.ReplyToEventId.ToString());
        const auto CompletedSpeech=Speech;
        const int32 CompletedDepth=SpeechDepth;
        const TWeakObjectPtr<ADeliveryNpcCharacter> WeakResident(Resident);
        Resident->Agent->ObserveEvent(Said);
        Resident->Agent->FinishReplyPlayback(CompletedSpeech.TurnId,CompletedSpeech.TurnId);
        if (!WeakResident.IsValid() || Resident->IsActorBeingDestroyed()) return;

        // 让 800 cm 内还没收到这句的 NPC 听到；层数加一，第三层或开麦对话时 bRequestReply 为 false，只记住不回话
        // 最后调用 PromotePendingReply 开始排在后面的下一轮
        for (TActorIterator<ADeliveryNpcCharacter> It(GetWorld());It;++It)
            if (*It!=Resident && !Resident->Agent->WasReplyTextDelivered(CompletedSpeech.TurnId,*It) &&
                FVector::DistSquared(It->GetActorLocation(),Resident->GetActorLocation())<FMath::Square(800.f))
                It->HearSpeech(Resident,CompletedSpeech.Text,CompletedSpeech.TurnId,FMath::Min(CompletedDepth+1,3),
                    CompletedListener.IsValid()?CompletedListener.Get():Resident,Said.ReplyToEventId,
                    CompletedDepth<3 && !CompletedSpeech.RadioListener);
        PromotePendingReply();
    }
    if (GetNetMode()==NM_DedicatedServer) return;
    TickExpression(Dt);
    const auto Display=GetDisplayedSpeech();

    // 网格用的是 UDeliveryNpcBodyAnimInstance 时不调用 UpdateLegacyAnimation：PlayAnimation 会把网格切回单片段模式，主动画蓝图就失效了
    const auto* MainClass=Resident->GetMesh()->GetAnimClass();
    if (!MainClass || !MainClass->IsChildOf(UDeliveryNpcBodyAnimInstance::StaticClass())) UpdateLegacyAnimation(Resident);
    if (!MouthMorph.IsNone()) Resident->GetMesh()->SetMorphTarget(MouthMorph,GetVoiceAmplitude());
    if (!AngryMorph.IsNone()) Resident->GetMesh()->SetMorphTarget(AngryMorph,Display.bActive && Display.Emotion==TEXT("Angry")?1.f:0.f);
    if (Caption)
        if (auto* Widget=Cast<UDeliveryNpcCaptionWidget>(Caption->GetUserWidgetObject()))
        {
            const int32 Count=FMath::Clamp(int32(GetDisplayedSpeechProgress()*Display.Text.Len())+1,0,Display.Text.Len());

            // 字幕只在说台词、而且镜头在 CaptionVisibleDistance 以内时显示，格式是名字加已经说到的字；等回复时不显示
            FString Line;
            const auto* Camera=UGameplayStatics::GetPlayerCameraManager(this,0);
            const bool bNear=!Camera || FVector::DistSquared(Camera->GetCameraLocation(),Resident->GetActorLocation())<=FMath::Square(CaptionVisibleDistance);
            if (Display.bActive && bNear) Line=Resident->DisplayName.ToString()+TEXT("：")+Display.Text.Left(Count);
            Widget->SetCaption(FText::FromString(Line));
        }
}

// ── 旧动画配置 ──
// 没有用主动画蓝图的旧 NPC 走这里：直接用 PlayAnimation 播单个片段，选片段的顺序和主动画蓝图一样

/// @brief 按起身 → 倒地 → 受击 → 出拳 → 拽车 → 走路（踱步、快走）→ 说话 → 站立的顺序选片段并播放
void UDeliveryNpcPresentationComponent::UpdateLegacyAnimation(ADeliveryNpcCharacter* Resident)
{
    const auto Display=GetDisplayedSpeech();
    UAnimSequence* Desired=IdleAnimation;
    bool bLoop=true;
    float DesiredPlayRate=1.f;
    const auto State=Resident->Agent->GetPublicState();
    const double StrikeStartedAt=Resident->Combat->GetStrikeStartTime(State.ActionId);
    const double StrikeElapsed=StrikeStartedAt>=0?ServerTime()-StrikeStartedAt:-1;
    const bool bStrikePlaying=State.Status==EDeliveryNpcActionStatus::Running &&
        (State.Action==EDeliveryNpcAction::StrikeOnce || State.Action==EDeliveryNpcAction::Fight) &&
        StrikeElapsed>=0 && StrikeElapsed<UDeliveryNpcCombatComponent::StrikeDuration;
    if (Resident->Combat->IsRecovering() && GetUpAnimation) { Desired=GetUpAnimation; bLoop=false; }
    else if (Resident->IsKnockedDown()) { Desired=DownAnimation; bLoop=false; }
    else if (Resident->Combat->IsReactingToHit() && HitAnimation) { Desired=HitAnimation; bLoop=false; }
    else if (bStrikePlaying) { Desired=AttackAnimation; bLoop=false; }
    else if (State.Status==EDeliveryNpcActionStatus::Running && State.Action==EDeliveryNpcAction::HoldVehicle) Desired=HoldAnimation;
    else if (Resident->GetVelocity().Size2D()>20)
    {
        Desired=WalkAnimation;
        float ReferenceSpeed=WalkAnimationSpeed;
        if (State.Status==EDeliveryNpcActionStatus::Running && State.Action==EDeliveryNpcAction::Pace && PaceAnimation)
        { Desired=PaceAnimation; ReferenceSpeed=PaceAnimationSpeed; }
        else if (State.Status==EDeliveryNpcActionStatus::Running &&
            (State.Action==EDeliveryNpcAction::StrikeOnce || State.Action==EDeliveryNpcAction::Fight || State.Action==EDeliveryNpcAction::Flee) && HurryAnimation)
        { Desired=HurryAnimation; ReferenceSpeed=HurryAnimationSpeed; }
        DesiredPlayRate=FMath::Clamp(Resident->GetVelocity().Size2D()/FMath::Max(10.f,ReferenceSpeed),.2f,2.f);
    }
    else if (Display.bActive && TalkAnimation) Desired=TalkAnimation;
    if (Desired && (Playing!=Desired || (bStrikePlaying && PlayingStrike!=Resident->Combat->GetCurrentPunchId())))
    {
        Resident->GetMesh()->PlayAnimation(Desired,bLoop); Playing=Desired;
        if (bStrikePlaying && Desired==AttackAnimation)
        {
            // 出拳片段从这一拳已经过去的时间开始播，晚收到消息的客户端和服务器动作对得上
            Resident->GetMesh()->SetPosition(static_cast<float>(StrikeElapsed),false);
            PlayingStrike=Resident->Combat->GetCurrentPunchId();
        }
    }

    // 走路时播放速率等于实际速度除以片段的参考速度；不走路时 DesiredPlayRate 是 1，恢复原速
    if (Desired) Resident->GetMesh()->SetPlayRate(DesiredPlayRate);

    // 受击片段按 HitReactionDuration 拉伸，每帧直接设播放位置
    if (Desired==HitAnimation && HitAnimation && Resident->Combat->IsReactingToHit())
    {
        const float Progress=FMath::Clamp(static_cast<float>((ServerTime()-Resident->Combat->GetHitReactionStartTime()) /
            UDeliveryNpcCombatComponent::HitReactionDuration),0.f,1.f);
        Resident->GetMesh()->SetPosition(Progress*HitAnimation->GetPlayLength(),false);
    }
}

/// @brief 组件结束：取消后台压缩，解除网格的更新顺序，停掉并销毁反应音、电子语音、录制语音和字幕，清空所有记录
void UDeliveryNpcPresentationComponent::EndPlay(const EEndPlayReason::Type Reason)
{
    PendingReply={};
    CancelSentenceGeneration();
    if (auto* Npc=Cast<ADeliveryNpcCharacter>(GetOwner()))
    {
        Npc->GetMesh()->RemoveTickPrerequisiteComponent(this);
        Npc->GetMesh()->RemoveTickPrerequisiteComponent(Npc->Combat);
    }
    bReactionsEnded=true;
    if (ReactionAudio) { ReactionAudio->Stop();ReactionAudio->DestroyComponent();ReactionAudio=nullptr; }
    ReactionEventId.Invalidate();ReactionEndsAt=0;SeenReactionEvents.Reset();CancelledReactionEvents.Reset();
    if (Voice) { Voice->StopSpeaking(); Voice->DestroyComponent(); Voice=nullptr; }
    if (RecordedVoice) { RecordedVoice->Stop(); RecordedVoice->DestroyComponent(); RecordedVoice=nullptr; }
    RecordedWave=nullptr; LocalVoices.Reset(); DisplayedSpeech={}; CancelledVoiceActions.Reset(); FinishedVoiceTurns.Reset(); ServerVoices.Reset(); SendingAudio.Reset(); Sentences.Reset(); EncodedSentences.Reset();
    ReplyAction.Invalidate(); ReplyMessage.Invalidate();
    if (Caption) { Caption->DestroyComponent(); Caption=nullptr; }
    Super::EndPlay(Reason);
}

/// @brief 登记要同步的属性：Speech（正在说的句子）和 ReplyAction（当前这轮回复的编号）
void UDeliveryNpcPresentationComponent::GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const
{
    Super::GetLifetimeReplicatedProps(OutLifetimeProps);
    DOREPLIFETIME(UDeliveryNpcPresentationComponent,Speech);
    DOREPLIFETIME(UDeliveryNpcPresentationComponent,ReplyAction);
}
