#include "AI/Residents/DeliveryConversationComponent.h"
#include "AI/Residents/DeliveryResidentCharacter.h"
#include "AI/DeliveryNpcAgentComponent.h"
#include "UI/DeliveryHUD.h"
#include "EnhancedInputComponent.h"
#include "EnhancedInputSubsystems.h"
#include "InputAction.h"
#include "InputMappingContext.h"
#include "Engine/LocalPlayer.h"
#include "GameFramework/PlayerController.h"
#include "EngineUtils.h"
#include "Engine/World.h"

UDeliveryConversationComponent::UDeliveryConversationComponent() { SetIsReplicatedByDefault(true); }
void UDeliveryConversationComponent::BindInput()
{
    auto* PC=Cast<APlayerController>(GetOwner()); if (!PC || !PC->IsLocalController()) return;
    auto* Input=Cast<UEnhancedInputComponent>(PC->InputComponent);
    auto* Action=LoadObject<UInputAction>(nullptr,TEXT("/Game/NPC/Residents/Input/IA_Talk"));
    auto* Mapping=LoadObject<UInputMappingContext>(nullptr,TEXT("/Game/NPC/Residents/Input/IMC_ResidentConversation"));
    if (Input && Action) Input->BindAction(Action,ETriggerEvent::Started,this,&UDeliveryConversationComponent::OpenNearest);
    if (Mapping)
        if (auto* Sub=ULocalPlayer::GetSubsystem<UEnhancedInputLocalPlayerSubsystem>(PC->GetLocalPlayer())) Sub->AddMappingContext(Mapping,1);
}
bool UDeliveryConversationComponent::CanTalkTo(const ADeliveryResidentCharacter* Resident) const
{
    const auto* PC=Cast<APlayerController>(GetOwner()); const APawn* Pawn=PC?PC->GetPawn():nullptr;
    if (!Pawn || !IsValid(Resident) || Resident->GetWorld()!=GetWorld() || Resident->IsKnockedDown() ||
        FVector::DistSquared(Pawn->GetActorLocation(),Resident->GetActorLocation())>FMath::Square(600.f)) return false;
    FHitResult Hit; FCollisionQueryParams Params(SCENE_QUERY_STAT(ResidentConversation),false,Pawn);
    return !GetWorld()->LineTraceSingleByChannel(Hit,Pawn->GetActorLocation()+FVector(0,0,60),Resident->GetActorLocation(),ECC_Visibility,Params) || Hit.GetActor()==Resident;
}
void UDeliveryConversationComponent::OpenNearest()
{
    auto* PC=Cast<APlayerController>(GetOwner()); if (!PC || !PC->GetPawn()) return;
    ADeliveryResidentCharacter* Closest=nullptr; double Distance=MAX_dbl;
    for (TActorIterator<ADeliveryResidentCharacter> It(GetWorld());It;++It)
    {
        const double Current=FVector::DistSquared(PC->GetPawn()->GetActorLocation(),It->GetActorLocation());
        if (Current<Distance && CanTalkTo(*It)) { Closest=*It; Distance=Current; }
    }
    if (Closest) ServerOpenConversation(Closest);
}
void UDeliveryConversationComponent::ServerOpenConversation_Implementation(ADeliveryResidentCharacter* Resident)
{ if (CanTalkTo(Resident)) ClientOpenConversation(Resident); }
void UDeliveryConversationComponent::ClientOpenConversation_Implementation(ADeliveryResidentCharacter* Resident)
{
    auto* PC=Cast<APlayerController>(GetOwner()); if (!PC || !PC->IsLocalController() || !IsValid(Resident)) return;
    LastResult=NAME_None;
    if (auto* HUD=Cast<ADeliveryHUD>(PC->GetHUD())) HUD->OpenResidentConversation(Resident);
}
void UDeliveryConversationComponent::Submit(ADeliveryResidentCharacter* Resident,const FString& Text)
{
    auto* PC=Cast<APlayerController>(GetOwner()); if (!PC || !PC->IsLocalController()) return;
    const FString Message=Text.TrimStartAndEnd().Left(256);
    // 空输入不发送，也不覆盖上一条真实提交结果
    if (Message.IsEmpty()) return;
    const double Now=FPlatformTime::Seconds();
    if (Now<NextLocalSendAt) return;
    NextLocalSendAt=Now+1.1;
    LastResult=TEXT("Sending"); ServerSubmit(Resident,Message);
}
void UDeliveryConversationComponent::ServerSubmit_Implementation(ADeliveryResidentCharacter* Resident,const FString& Text)
{
    const double Now=FPlatformTime::Seconds();
    if (Now<NextSendAt) { ClientResult(TEXT("TooFast")); return; }
    const FString Message=Text.TrimStartAndEnd();
    if (Message.IsEmpty() || Text.Len()>256) { ClientResult(TEXT("Empty")); return; }
    if (!CanTalkTo(Resident))
    {
        UE_LOG(LogTemp,Log,TEXT("ResidentConversation rejected: player=%s resident=%s reason=Unavailable"),*GetNameSafe(GetOwner()),*GetNameSafe(Resident));
        ClientResult(TEXT("Unavailable")); return;
    }
    // 只有有效提交消耗冷却，失败重试不延长等待
    NextSendAt=Now+1;
    auto* PC=CastChecked<APlayerController>(GetOwner());
    if (!Resident->HearSpeech(PC->GetPawn(),Message,FGuid::NewGuid(),0)) { ClientResult(TEXT("NotHeard")); return; }
    UE_LOG(LogTemp,Log,TEXT("ResidentConversation accepted: player=%s resident=%s pending=%d activeAction=%d"),
        *GetNameSafe(GetOwner()),*GetNameSafe(Resident),Resident->Agent->IsDecisionPending(),Resident->Agent->HasActiveAction());
    ClientResult(TEXT("Heard"));
}
void UDeliveryConversationComponent::ClientResult_Implementation(FName Result) { LastResult=Result; ResultExpiresAt=FPlatformTime::Seconds()+2.5; }
