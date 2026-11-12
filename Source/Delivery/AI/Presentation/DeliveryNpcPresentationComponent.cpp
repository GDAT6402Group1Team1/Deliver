#include "AI/Presentation/DeliveryNpcPresentationComponent.h"
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
    if (GetNetMode()!=NM_DedicatedServer)
    {
        Voice=NewObject<UDeliveryDialogueVoiceComponent>(GetOwner());
        Voice->bAllowWorldSpeaker=true; Voice->bAllowSpatialization=true; Voice->bOverrideAttenuation=true;
        Voice->AttenuationOverrides.bAttenuate=true; Voice->AttenuationOverrides.AttenuationShapeExtents=FVector(150,0,0); Voice->AttenuationOverrides.FalloffDistance=800; Voice->SetupAttachment(GetOwner()->GetRootComponent()); Voice->RegisterComponent();
        Caption=NewObject<UWidgetComponent>(GetOwner()); Caption->SetupAttachment(GetOwner()->GetRootComponent());
        Caption->SetRelativeLocation(FVector(0,0,100)); Caption->SetWidgetSpace(EWidgetSpace::Screen);
        Caption->SetWidgetClass(UDeliveryNpcCaptionWidget::StaticClass()); Caption->SetDrawSize(FVector2D(400,100)); Caption->SetDrawAtDesiredSize(true);
        Caption->SetCollisionEnabled(ECollisionEnabled::NoCollision); Caption->RegisterComponent();
    }
    OnRep_Speech();
}
bool UDeliveryNpcPresentationComponent::PublishSpeech(const FString& Text,FName Emotion,int32 Depth,FGuid ReplyToEventId)
{
    if (!GetOwner()->HasAuthority() || Text.IsEmpty() || Text.Len()>120 || Depth>3 || Speech.bActive) return false;
    Speech={}; Speech.TurnId=FGuid::NewGuid(); Speech.Text=Text; Speech.Emotion=Emotion; Speech.ReplyToEventId=ReplyToEventId;
    Speech.StartTime=ServerTime(); Speech.Duration=FMath::Clamp(Text.Len()*.075f+.5f,1.f,10.f); Speech.bActive=true;
    SpeechDepth=Depth; GetOwner()->ForceNetUpdate(); OnRep_Speech();
    UE_LOG(LogTemp,Log,TEXT("ResidentSpeech started: npc=%s turn=%s replyTo=%s length=%d"),
        *GetNameSafe(GetOwner()),*Speech.TurnId.ToString(),*Speech.ReplyToEventId.ToString(),Speech.Text.Len());
    return true;
}
void UDeliveryNpcPresentationComponent::CancelSpeech()
{
    if (!GetOwner()->HasAuthority()) return;
    UE_LOG(LogTemp,Log,TEXT("ResidentSpeech cancelled: npc=%s turn=%s replyTo=%s"),
        *GetNameSafe(GetOwner()),*Speech.TurnId.ToString(),*Speech.ReplyToEventId.ToString());
    // 保留此前已经说完的句子，当前被打断的半句话不写入记忆
    FinishSentence();
    RecordSpokenReply(false);
    StopReaction();
    StopStreamSpeech(ReplyAction.IsValid()?ReplyAction:Speech.ActionId,true);
    ReplyAction.Invalidate(); ReplyMessage.Invalidate(); Sentences.Reset(); EncodedSentences.Reset(); SpokenReply.Reset();
    NextSentence=0; ExpectedSentences=INDEX_NONE; SendingAudio.Reset(); SendingOffset=0; ServerVoices.Reset();
    Speech.bActive=false; GetOwner()->ForceNetUpdate(); OnRep_Speech();
}
void UDeliveryNpcPresentationComponent::OnRep_Speech()
{
    if (Speech.ActionId.IsValid())
    {
        // 已取消的回复偶尔会晚于停止通知到达，保留停止结果
        if (CancelledVoiceActions.Contains(Speech.ActionId)) { Speech.bActive=false; return; }
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
    auto Settings=UDeliveryDialogueVoiceComponent::MakePresetSettings(Speech.Emotion==TEXT("Angry")?EDeliveryVoicePreset::Angry:EDeliveryVoicePreset::Normal);
    Settings.BaseFrequency=VoiceFrequency;
    Voice->SpeakTextWithSettings(FText::FromString(Speech.Text.Mid(Skip)),Settings);

}
void UDeliveryNpcPresentationComponent::TickComponent(float Dt,ELevelTick Type,FActorComponentTickFunction* Fn)
{
    Super::TickComponent(Dt,Type,Fn);
    TickReaction();
    auto* Resident=Cast<ADeliveryNpcCharacter>(GetOwner()); if (!Resident) return;
    TickSentences();
    if (GetOwner()->HasAuthority() && !ReplyAction.IsValid() && Speech.bActive && ServerTime()>=Speech.StartTime+Speech.Duration)
    {
        // 说完整句话后，将内容记录下来并告知附近的 NPC
        Speech.bActive=false; GetOwner()->ForceNetUpdate(); OnRep_Speech();
        FDeliveryNpcObservation Said; Said.Kind=TEXT("SelfSpeech");
        Said.Source=EDeliveryNpcEvidenceSource::ActionResult; Said.Description=Speech.Text; Said.Lifetime=60;
        Said.ReplyToEventId=Speech.ReplyToEventId;
        UE_LOG(LogTemp,Log,TEXT("ResidentSpeech completed: npc=%s turn=%s replyTo=%s"),
            *GetNameSafe(GetOwner()),*Speech.TurnId.ToString(),*Speech.ReplyToEventId.ToString());
        Resident->Agent->ObserveEvent(Said);
        if (SpeechDepth<3)
            for (TActorIterator<ADeliveryNpcCharacter> It(GetWorld());It;++It)
                if (*It!=Resident && FVector::DistSquared(It->GetActorLocation(),Resident->GetActorLocation())<FMath::Square(800.f))
                    It->HearSpeech(Resident,Speech.Text,Speech.TurnId,SpeechDepth+1);
    }
    if (GetNetMode()==NM_DedicatedServer) return;
    TickExpression(Dt);
    const auto Display=GetDisplayedSpeech();
    UAnimSequence* Desired=IdleAnimation;
    bool bLoop=true;
    const auto State=Resident->Agent->GetPublicState();
    if (Resident->Combat->IsRecovering() && GetUpAnimation) { Desired=GetUpAnimation; bLoop=false; }
    else if (Resident->IsKnockedDown()) { Desired=DownAnimation; bLoop=false; }
    else if (State.Status==EDeliveryNpcActionStatus::Running && State.Action==EDeliveryNpcAction::StrikeOnce) { Desired=AttackAnimation; bLoop=false; }
    else if (State.Status==EDeliveryNpcActionStatus::Running && State.Action==EDeliveryNpcAction::HoldVehicle) Desired=HoldAnimation;
    else if (Resident->GetVelocity().Size2D()>20) Desired=WalkAnimation;
    else if (Display.bActive && TalkAnimation) Desired=TalkAnimation;
    if (Desired && Playing!=Desired) { Resident->GetMesh()->PlayAnimation(Desired,bLoop); Playing=Desired; }
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
void UDeliveryNpcPresentationComponent::EndPlay(const EEndPlayReason::Type Reason)
{
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
{ Super::GetLifetimeReplicatedProps(OutLifetimeProps); DOREPLIFETIME(UDeliveryNpcPresentationComponent,Speech); }
