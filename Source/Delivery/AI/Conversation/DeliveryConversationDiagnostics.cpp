#include "AI/Conversation/DeliveryConversationComponent.h"
#include "AI/Characters/DeliveryNpcCharacter.h"
#include "AI/Decision/DeliveryNpcAgentComponent.h"
#include "UI/DeliveryHUD.h"
#include "EnhancedInputComponent.h"
#include "EnhancedInputSubsystems.h"
#include "Engine/LocalPlayer.h"
#include "Engine/World.h"
#include "GameFramework/PlayerController.h"
#include "InputAction.h"
#include "InputMappingContext.h"

void UDeliveryConversationComponent::BindInspectorInput()
{
#if !UE_BUILD_SHIPPING
    auto* PC=Cast<APlayerController>(GetOwner());
    if (bEndingPlay || !PC || !PC->IsLocalController()) return;
    auto* Input=Cast<UEnhancedInputComponent>(PC->InputComponent);
    auto* Action=LoadObject<UInputAction>(nullptr,TEXT("/Game/UI/Conversation/Input/IA_ToggleAiInspector"));
    auto* Mapping=LoadObject<UInputMappingContext>(nullptr,TEXT("/Game/UI/Conversation/Input/IMC_AiInspector"));
    if (!Input || !Action || !Mapping) return;
    if (BoundInspectorInput.Get()!=Input || !InspectorInputHandle)
    {
        if (auto* Old=BoundInspectorInput.Get(); Old && InspectorInputHandle) Old->RemoveBindingByHandle(InspectorInputHandle);
        BoundInspectorInput=Input;
        InspectorInputHandle=Input->BindAction(Action,ETriggerEvent::Started,this,&UDeliveryConversationComponent::ToggleInspector).GetHandle();
    }
    if (auto* Sub=ULocalPlayer::GetSubsystem<UEnhancedInputLocalPlayerSubsystem>(PC->GetLocalPlayer()))
    {
        if (InspectorInputSubsystem.Get()!=Sub || InspectorMapping.Get()!=Mapping)
        {
            if (auto* Old=InspectorInputSubsystem.Get(); Old && bOwnsInspectorMapping && InspectorMapping.IsValid())
                Old->RemoveMappingContext(InspectorMapping.Get());
            InspectorInputSubsystem=Sub; InspectorMapping=Mapping; bOwnsInspectorMapping=false;
        }
        if (!Sub->HasMappingContext(Mapping)) { Sub->AddMappingContext(Mapping,1); bOwnsInspectorMapping=true; }
    }
#endif
}

void UDeliveryConversationComponent::ToggleInspector()
{
    if (const auto* PC=Cast<APlayerController>(GetOwner()))
        if (auto* HUD=Cast<ADeliveryHUD>(PC->GetHUD())) HUD->ToggleAiInspector();
}

bool UDeliveryConversationComponent::CanInspectNpc(const ADeliveryNpcCharacter* Resident) const
{
    const auto* PC=Cast<APlayerController>(GetOwner());
    const APawn* Pawn=PC?PC->GetPawn():nullptr;
    // 演示观察允许查看附近倒地或正在跑动的人物，不借用交谈成功的玩法条件
    return !bEndingPlay && Pawn && IsValid(Resident) && Resident->GetWorld()==GetWorld() &&
        Resident->Agent && FVector::DistSquared(Pawn->GetActorLocation(),Resident->GetActorLocation())<=FMath::Square(3000.f);
}

void UDeliveryConversationComponent::WatchNpcDiagnostics(ADeliveryNpcCharacter* Resident)
{
#if !UE_BUILD_SHIPPING
    const auto* PC=Cast<APlayerController>(GetOwner());
    if (bEndingPlay || !PC || !PC->IsLocalController() || !IsValid(Resident) || Resident->GetWorld()!=GetWorld()) return;
    if (LocalInspectorTarget.Get()==Resident && LocalInspectorPawn.Get()==PC->GetPawn()) return;
    LocalInspectorTarget=Resident; LocalInspectorPawn=PC->GetPawn();
    InspectorWorldEpoch.Invalidate(); InspectorRevision=-1;
    if (++LocalInspectorGeneration==0) ++LocalInspectorGeneration;
    FDeliveryNpcDecisionTrace Empty; Empty.NpcName=Resident->DisplayName.ToString();
    OnDiagnosticsChanged.Broadcast(Empty);
    ServerWatchNpcDiagnostics(Resident,LocalInspectorGeneration);
#endif
}

void UDeliveryConversationComponent::StopWatchingNpcDiagnostics()
{
    if (++LocalInspectorGeneration==0) ++LocalInspectorGeneration;
    LocalInspectorTarget.Reset(); LocalInspectorPawn.Reset();
    InspectorWorldEpoch.Invalidate(); InspectorRevision=-1;
    if (!bEndingPlay) ServerWatchNpcDiagnostics(nullptr,LocalInspectorGeneration);
    OnDiagnosticsChanged.Broadcast(FDeliveryNpcDecisionTrace());
}

void UDeliveryConversationComponent::UnbindServerInspector()
{
    if (auto* Npc=ServerInspectorTarget.Get(); Npc && Npc->Agent)
        Npc->Agent->OnDecisionTraceChanged.Remove(InspectorTraceHandle);
    InspectorTraceHandle.Reset(); ServerInspectorTarget.Reset(); ServerInspectorPawn.Reset();
}

void UDeliveryConversationComponent::ServerWatchNpcDiagnostics_Implementation(ADeliveryNpcCharacter* Resident,uint64 Generation)
{
    if (!GetOwner() || !GetOwner()->HasAuthority() || bEndingPlay || !Generation || Generation<=ServerInspectorGeneration) return;
    ServerInspectorGeneration=Generation;
    UnbindServerInspector();
#if !UE_BUILD_SHIPPING
    if (!CanInspectNpc(Resident))
    {
        FDeliveryNpcDecisionTrace Empty;
        Empty.Stage=Resident?FName(TEXT("Unavailable")):NAME_None;
        ClientNpcDiagnostics(Generation,Empty);
        return;
    }
    ServerInspectorTarget=Resident;
    ServerInspectorPawn=CastChecked<APlayerController>(GetOwner())->GetPawn();
    InspectorTraceHandle=Resident->Agent->OnDecisionTraceChanged.AddUObject(this,&UDeliveryConversationComponent::HandleInspectorTrace);
    FDeliveryNpcDecisionTrace Initial=Resident->Agent->GetDecisionTrace();
    if (Initial.NpcName.IsEmpty()) Initial.NpcName=Resident->DisplayName.ToString();
    ClientNpcDiagnostics(Generation,Initial);
#endif
}

void UDeliveryConversationComponent::HandleInspectorTrace(const FDeliveryNpcDecisionTrace& Trace)
{
    auto* PC=Cast<APlayerController>(GetOwner());
    auto* Npc=ServerInspectorTarget.Get();
    if (!PC || !PC->HasAuthority()) return;
    if (!CanInspectNpc(Npc) || ServerInspectorPawn.Get()!=PC->GetPawn())
    {
        UnbindServerInspector();
        FDeliveryNpcDecisionTrace Empty; Empty.Stage=TEXT("Unavailable");
        if (!bEndingPlay) ClientNpcDiagnostics(ServerInspectorGeneration,Empty);
        return;
    }
    ClientNpcDiagnostics(ServerInspectorGeneration,Trace);
}

void UDeliveryConversationComponent::ClientNpcDiagnostics_Implementation(uint64 Generation,const FDeliveryNpcDecisionTrace& Trace)
{
    const auto* PC=Cast<APlayerController>(GetOwner());
    if (bEndingPlay || !PC || !PC->IsLocalController() || Generation!=LocalInspectorGeneration ||
        !PC->GetPawn() || LocalInspectorPawn.Get()!=PC->GetPawn()) return;
    if (Trace.RequestId.IsValid())
    {
        if (!LocalInspectorTarget.IsValid() || LocalInspectorTarget->GetWorld()!=GetWorld() || !Trace.WorldEpoch.IsValid()) return;
        // 同一订阅只属于一个世界；迟到的旧目标、旧世界和旧版本都不能覆盖新记录
        if (InspectorWorldEpoch.IsValid() && InspectorWorldEpoch!=Trace.WorldEpoch) return;
        if (Trace.Revision<=InspectorRevision) return;
        InspectorWorldEpoch=Trace.WorldEpoch; InspectorRevision=Trace.Revision;
    }
    if (Trace.Stage==TEXT("Unavailable")) LocalInspectorTarget.Reset();
    OnDiagnosticsChanged.Broadcast(Trace);
}

void UDeliveryConversationComponent::ShutdownInspector()
{
    UnbindServerInspector();
    if (auto* Input=BoundInspectorInput.Get(); Input && InspectorInputHandle) Input->RemoveBindingByHandle(InspectorInputHandle);
    if (auto* Sub=InspectorInputSubsystem.Get(); Sub && bOwnsInspectorMapping && InspectorMapping.IsValid())
        Sub->RemoveMappingContext(InspectorMapping.Get());
    BoundInspectorInput.Reset(); InspectorInputSubsystem.Reset(); InspectorMapping.Reset();
    InspectorInputHandle=0; bOwnsInspectorMapping=false;
    LocalInspectorTarget.Reset(); LocalInspectorPawn.Reset(); OnDiagnosticsChanged.Clear();
}
