#include "AI/Presentation/DeliveryNpcPresentationComponent.h"
#include "AI/Presentation/DeliveryNpcBodyAnimInstance.h"
#include "AI/Characters/DeliveryNpcCharacter.h"
#include "AI/Actions/DeliveryNpcCombatComponent.h"
#include "AI/Decision/DeliveryNpcAgentComponent.h"
#include "Audio/DeliveryDialogueVoiceComponent.h"
#include "Components/SkeletalMeshComponent.h"
#include "Components/WidgetComponent.h"
#include "Components/AudioComponent.h"
#include "UI/DeliveryNpcCaptionWidget.h"
#include "Animation/AnimSequence.h"
#include "GameFramework/GameStateBase.h"
#include "Kismet/GameplayStatics.h"
#include "EngineUtils.h"
#include "Engine/World.h"
#include "Net/UnrealNetwork.h"

UDeliveryNpcPresentationComponent::UDeliveryNpcPresentationComponent()
{
    SetIsReplicatedByDefault(true); PrimaryComponentTick.bCanEverTick=true;
}
double UDeliveryNpcPresentationComponent::ServerTime() const
{
    if (const auto* GS=GetWorld()->GetGameState()) return GS->GetServerWorldTimeSeconds();
    return GetWorld()->GetTimeSeconds();
}
void UDeliveryNpcPresentationComponent::BeginPlay()
{
    Super::BeginPlay();
    if (auto* Npc=Cast<ADeliveryNpcCharacter>(GetOwner()))
    {
        // 先准备本帧的战斗和对白状态，再由网格更新主动画与注视姿态
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
        // 缺少字幕资产时保留组件，旧对白路径仍依赖它存在才会播放声音
        Caption->SetWidgetClass(CaptionClass); Caption->SetDrawSize(FVector2D(400,100)); Caption->SetDrawAtDesiredSize(true);
        Caption->SetCollisionEnabled(ECollisionEnabled::NoCollision); Caption->RegisterComponent();
    }
    OnRep_Speech();
}
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
    // 规则短台词沿原入口，完整模型回复使用已保存的收话人与编号
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

bool UDeliveryNpcPresentationComponent::QueueCompleteReply(const FDeliveryNpcCompleteReply& Reply)
{
    if (!GetOwner()->HasAuthority() || !Reply.ReplyId.IsValid() || !DeliveryNpcMessages::IsValidReplyText(Reply.Text,false)) return false;
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

bool UDeliveryNpcPresentationComponent::StartDeferredCompleteReply()
{
    const auto* Npc=Cast<ADeliveryNpcCharacter>(GetOwner());
    if (!Npc || Speech.bActive || !DeferredCompleteReply.ReplyId.IsValid() ||
        DeferredCompleteReply.ReplyId!=ReplyAction || !Npc->Agent->CanStartReplySpeech(ReplyAction)) return false;
    const auto Reply=MoveTemp(DeferredCompleteReply);
    DeferredCompleteReply={};
    // 使用原完整台词的播音方式；这里只结束队首等待，不取消 Agent 的回复
    ReplyAction.Invalidate(); ReplyMessage.Invalidate();
    ReplyGameplayAction.Invalidate(); ReplyTarget.Reset(); bReplyHadTarget=false;
    ExpectedSentences=INDEX_NONE; NextSentence=0;
    return StartCompleteReply(Reply);
}
void UDeliveryNpcPresentationComponent::CancelSpeech(bool bFinishCurrentSentence,bool bPreserveOtherReplies)
{
    if (!GetOwner()->HasAuthority()) return;
    DiscardPendingReply();
    UE_LOG(LogTemp,Log,TEXT("ResidentSpeech cancelled: npc=%s turn=%s replyTo=%s"),
        *GetNameSafe(GetOwner()),*Speech.TurnId.ToString(),*Speech.ReplyToEventId.ToString());
    // 保留此前已经说完的句子，当前被打断的半句话不写入记忆
    FinishSentence();
    RecordSpokenReply(false);
    CancelSentenceGeneration();
    StopReaction();
    // 只停止这一轮声音，旧停止通知晚到时不能取消已经开始的新回复
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
        // 刚走近的客户端可能还在补取当前句，已完成的声音保留原来的五秒期限
        for (auto It=ServerVoices.CreateIterator();It;++It)
            if (!It.Value().bComplete || It.Value().ExpiresAt<=0) It.RemoveCurrent();
        TrimVoiceCache();
    }
    else if (bPreserveOtherReplies) ServerVoices.Remove(Speech.TurnId);
    else ServerVoices.Reset();
    Speech.bActive=false; GetOwner()->ForceNetUpdate(); OnRep_Speech();
    if (auto* Npc=Cast<ADeliveryNpcCharacter>(GetOwner())) Npc->Agent->CancelReply(StoppedReply);
}
void UDeliveryNpcPresentationComponent::OnRep_Speech()
{
    if (!Speech.ActionId.IsValid() && CancelledVoiceActions.Contains(Speech.TurnId)) return;
    if (Speech.ActionId.IsValid())
    {
        // 已取消的回复偶尔会晚于停止通知到达，保留停止结果
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
    const int32 Skip=FMath::Clamp(int32((ServerTime()-Speech.StartTime)/.075),0,Speech.Text.Len());
    StopReaction();
    const bool bRadio=IsLocalRadioListener(Speech);
    Voice->bAllowSpatialization=!bRadio;
    Voice->AttenuationOverrides.bAttenuate=!bRadio;
    Voice->AttenuationOverrides.bSpatialize=!bRadio;
    auto Settings=UDeliveryDialogueVoiceComponent::MakePresetSettings(Speech.Emotion==TEXT("Angry")?EDeliveryVoicePreset::Angry:EDeliveryVoicePreset::Normal);
    Settings.BaseFrequency=VoiceFrequency;
    Voice->SpeakTextWithSettings(FText::FromString(Speech.Text.Mid(Skip)),Settings);

}
void UDeliveryNpcPresentationComponent::TickComponent(float Dt,ELevelTick Type,FActorComponentTickFunction* Fn)
{
    Super::TickComponent(Dt,Type,Fn);
    TickReaction();
    if (Voice && !Voice->bAllowSpatialization && !IsLocalRadioListener(Speech)) Voice->StopSpeaking();
    auto* Resident=Cast<ADeliveryNpcCharacter>(GetOwner()); if (!Resident) return;
    TickSentences();
    if (GetOwner()->HasAuthority() && !ReplyAction.IsValid() && Speech.bActive && ServerTime()>=Speech.StartTime+Speech.Duration)
    {
        // 说完整句话后，将内容记录下来并告知附近的 NPC
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
        // 完整回复与流式回复一样，最后一轮只通知邻居记住台词
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
    // PlayAnimation 会把网格切回单片段模式，主动画蓝图已经接管时不能再调用
    const auto* MainClass=Resident->GetMesh()->GetAnimClass();
    if (!MainClass || !MainClass->IsChildOf(UDeliveryNpcBodyAnimInstance::StaticClass())) UpdateLegacyAnimation(Resident);
    if (!MouthMorph.IsNone()) Resident->GetMesh()->SetMorphTarget(MouthMorph,GetVoiceAmplitude());
    if (!AngryMorph.IsNone()) Resident->GetMesh()->SetMorphTarget(AngryMorph,Display.bActive && Display.Emotion==TEXT("Angry")?1.f:0.f);
    if (Caption)
        if (auto* Widget=Cast<UDeliveryNpcCaptionWidget>(Caption->GetUserWidgetObject()))
        {
            const int32 Count=FMath::Clamp(int32(GetDisplayedSpeechProgress()*Display.Text.Len())+1,0,Display.Text.Len());
            // 等待回复时用眼神和姿态表现倾听，开口后显示字幕
            FString Line;
            if (Display.bActive) Line=Resident->DisplayName.ToString()+TEXT("：")+Display.Text.Left(Count);
            Widget->SetCaption(FText::FromString(Line));
        }
}
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
            // 远端按实际出拳时间播放，较晚收到消息时从当前进度开始
            Resident->GetMesh()->SetPosition(static_cast<float>(StrikeElapsed),false);
            PlayingStrike=Resident->Combat->GetCurrentPunchId();
        }
    }
    // 低速踱步和快步靠近各用自己的步频，停下后恢复其他动作的原速
    if (Desired) Resident->GetMesh()->SetPlayRate(DesiredPlayRate);
    if (Desired==HitAnimation && HitAnimation && Resident->Combat->IsReactingToHit())
    {
        const float Progress=FMath::Clamp(static_cast<float>((ServerTime()-Resident->Combat->GetHitReactionStartTime()) /
            UDeliveryNpcCombatComponent::HitReactionDuration),0.f,1.f);
        Resident->GetMesh()->SetPosition(Progress*HitAnimation->GetPlayLength(),false);
    }
}
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
void UDeliveryNpcPresentationComponent::GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const
{
    Super::GetLifetimeReplicatedProps(OutLifetimeProps);
    DOREPLIFETIME(UDeliveryNpcPresentationComponent,Speech);
    DOREPLIFETIME(UDeliveryNpcPresentationComponent,ReplyAction);
}
