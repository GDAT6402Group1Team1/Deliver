#include "AI/Residents/DeliveryNpcPresentationComponent.h"
#include "AI/Residents/DeliveryResidentCharacter.h"
#include "AI/Residents/DeliveryResidentCombatComponent.h"
#include "AI/DeliveryNpcAgentComponent.h"
#include "Audio/DeliveryDialogueVoiceComponent.h"
#include "Components/SkeletalMeshComponent.h"
#include "Components/WidgetComponent.h"
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
    if (!GetOwner()->HasAuthority() || !Speech.bActive) return;
    UE_LOG(LogTemp,Log,TEXT("ResidentSpeech cancelled: npc=%s turn=%s replyTo=%s"),
        *GetNameSafe(GetOwner()),*Speech.TurnId.ToString(),*Speech.ReplyToEventId.ToString());
    Speech.bActive=false; GetOwner()->ForceNetUpdate(); OnRep_Speech();
}
void UDeliveryNpcPresentationComponent::OnRep_Speech()
{
    if (!Voice || !Caption) return;
    if (!Speech.bActive || ServerTime()>Speech.StartTime+Speech.Duration)
    { Voice->StopSpeaking(); return; }
    if (PlayedTurn==Speech.TurnId) return;
    PlayedTurn=Speech.TurnId;
    const int32 Skip=FMath::Clamp(int32((ServerTime()-Speech.StartTime)/.075),0,Speech.Text.Len());
    auto Settings=UDeliveryDialogueVoiceComponent::MakePresetSettings(Speech.Emotion==TEXT("Angry")?EDeliveryVoicePreset::Angry:EDeliveryVoicePreset::Normal);
    Settings.BaseFrequency=VoiceFrequency;
    Voice->SpeakTextWithSettings(FText::FromString(Speech.Text.Mid(Skip)),Settings);

}
void UDeliveryNpcPresentationComponent::TickComponent(float Dt,ELevelTick Type,FActorComponentTickFunction* Fn)
{
    Super::TickComponent(Dt,Type,Fn);
    auto* Resident=Cast<ADeliveryResidentCharacter>(GetOwner()); if (!Resident) return;
    if (GetOwner()->HasAuthority() && Speech.bActive && ServerTime()>=Speech.StartTime+Speech.Duration)
    {
        // 仅完整说出的台词进入邻居记忆，被受击中断的对白不会当作已说事实
        Speech.bActive=false; GetOwner()->ForceNetUpdate(); OnRep_Speech();
        // 只记住完整说出的台词，模型可据此区分已回应和未回应的玩家发言
        FDeliveryNpcObservation Said; Said.Kind=TEXT("SelfSpeech");
        Said.Source=EDeliveryNpcEvidenceSource::ActionResult; Said.Description=Speech.Text; Said.Lifetime=60;
        Said.ReplyToEventId=Speech.ReplyToEventId;
        UE_LOG(LogTemp,Log,TEXT("ResidentSpeech completed: npc=%s turn=%s replyTo=%s"),
            *GetNameSafe(GetOwner()),*Speech.TurnId.ToString(),*Speech.ReplyToEventId.ToString());
        Resident->Agent->ObserveEvent(Said);
        if (SpeechDepth<3)
            for (TActorIterator<ADeliveryResidentCharacter> It(GetWorld());It;++It)
                if (*It!=Resident && FVector::DistSquared(It->GetActorLocation(),Resident->GetActorLocation())<FMath::Square(800.f))
                    It->HearSpeech(Resident,Speech.Text,Speech.TurnId,SpeechDepth+1);
    }
    if (GetNetMode()==NM_DedicatedServer) return;
    UAnimSequence* Desired=IdleAnimation;
    bool bLoop=true;
    const auto State=Resident->Agent->GetPublicState();
    if (Resident->Combat->IsRecovering() && GetUpAnimation) { Desired=GetUpAnimation; bLoop=false; }
    else if (Resident->IsKnockedDown()) { Desired=DownAnimation; bLoop=false; }
    else if (State.Status==EDeliveryNpcActionStatus::Running && State.Action==EDeliveryNpcAction::StrikeOnce) { Desired=AttackAnimation; bLoop=false; }
    else if (State.Status==EDeliveryNpcActionStatus::Running && State.Action==EDeliveryNpcAction::HoldVehicle) Desired=HoldAnimation;
    else if (Resident->GetVelocity().Size2D()>20) Desired=WalkAnimation;
    else if (Speech.bActive && TalkAnimation) Desired=TalkAnimation;
    if (Desired && Playing!=Desired) { Resident->GetMesh()->PlayAnimation(Desired,bLoop); Playing=Desired; }
    if (!MouthMorph.IsNone()) Resident->GetMesh()->SetMorphTarget(MouthMorph,Speech.bActive?FMath::Abs(FMath::Sin(GetWorld()->GetTimeSeconds()*20))*.6f:0);
    if (!AngryMorph.IsNone()) Resident->GetMesh()->SetMorphTarget(AngryMorph,Speech.bActive && Speech.Emotion==TEXT("Angry")?1.f:0.f);
    if (Caption)
        if (auto* Widget=Cast<UDeliveryNpcCaptionWidget>(Caption->GetUserWidgetObject()))
        {
            const int32 Count=FMath::Clamp(int32((ServerTime()-Speech.StartTime)/.075)+1,0,Speech.Text.Len());
            // 真正说话优先；等待模型时只用省略号反馈，不暴露后台状态文案
            FString Line;
            if (Speech.bActive) Line=Resident->DisplayName.ToString()+TEXT("：")+Speech.Text.Left(Count);
            else if (!Resident->IsKnockedDown() && Resident->Agent->IsDecisionPending())
                Line=Resident->DisplayName.ToString()+TEXT("：…");
            Widget->SetCaption(FText::FromString(Line));
        }
}
void UDeliveryNpcPresentationComponent::EndPlay(const EEndPlayReason::Type Reason)
{
    if (Voice) { Voice->StopSpeaking(); Voice->DestroyComponent(); Voice=nullptr; }
    if (Caption) { Caption->DestroyComponent(); Caption=nullptr; }
    Super::EndPlay(Reason);
}
void UDeliveryNpcPresentationComponent::GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const
{ Super::GetLifetimeReplicatedProps(OutLifetimeProps); DOREPLIFETIME(UDeliveryNpcPresentationComponent,Speech); }
