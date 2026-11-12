#include "AI/Conversation/DeliveryConversationComponent.h"
#include "AI/Characters/DeliveryNpcCharacter.h"
#include "AI/Decision/DeliveryNpcAgentComponent.h"
#include "AI/Presentation/DeliveryNpcPresentationComponent.h"
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
void UDeliveryConversationComponent::ServerRequestNpcAudio_Implementation(ADeliveryNpcCharacter* Npc,FGuid TurnId,int32 Offset)
{
    const double Now=FPlatformTime::Seconds();
    const auto* PC=Cast<APlayerController>(GetOwner());const APawn* Pawn=PC?PC->GetPawn():nullptr;
    if (Now<NextAudioRequestAt || Offset<0 || !Pawn || !IsValid(Npc) || Npc->GetWorld()!=GetWorld() ||
        FVector::DistSquared(Pawn->GetActorLocation(),Npc->GetActorLocation())>FMath::Square(1000.f)) return;
    NextAudioRequestAt=Now+.25;
    TArray<uint8> Audio;
    int32 Total=0;
    if (!Npc->Presentation->GetSpeechAudioChunk(TurnId,Offset,2048,Audio,Total)) return;
    // 每次最多补发 2 KB 压缩声音，避免一次补发阻塞人物状态同步
    ClientReceiveNpcAudio(Npc,TurnId,Offset,Total,Audio);
}
void UDeliveryConversationComponent::ClientReceiveNpcAudio_Implementation(ADeliveryNpcCharacter* Npc,FGuid TurnId,int32 Offset,int32 Total,const TArray<uint8>& Data)
{
    if (!IsValid(Npc) || Npc->GetWorld()!=GetWorld()) return;
    Npc->Presentation->ReceiveAudio_Implementation(TurnId,Offset,Total,Data);
}
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
bool UDeliveryConversationComponent::CanTalkTo(const ADeliveryNpcCharacter* Resident) const
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
    ADeliveryNpcCharacter* Closest=nullptr; double Distance=MAX_dbl;
    for (TActorIterator<ADeliveryNpcCharacter> It(GetWorld());It;++It)
    {
        const double Current=FVector::DistSquared(PC->GetPawn()->GetActorLocation(),It->GetActorLocation());
        if (Current<Distance && CanTalkTo(*It)) { Closest=*It; Distance=Current; }
    }
    if (Closest) ServerOpenConversation(Closest);
}
void UDeliveryConversationComponent::ServerOpenConversation_Implementation(ADeliveryNpcCharacter* Resident)
{ if (CanTalkTo(Resident)) ClientOpenConversation(Resident); }
void UDeliveryConversationComponent::ClientOpenConversation_Implementation(ADeliveryNpcCharacter* Resident)
{
    auto* PC=Cast<APlayerController>(GetOwner()); if (!PC || !PC->IsLocalController() || !IsValid(Resident)) return;
    LastResult=NAME_None;
    if (auto* HUD=Cast<ADeliveryHUD>(PC->GetHUD())) HUD->OpenResidentConversation(Resident);
}
bool UDeliveryConversationComponent::Submit(ADeliveryNpcCharacter* Resident,const FString& Text)
{
    auto* PC=Cast<APlayerController>(GetOwner());
    if (!PC || !PC->IsLocalController() || !IsValid(Resident) || Resident->GetWorld()!=GetWorld()) return false;
    const FString Message=Text.TrimStartAndEnd().Left(256);
    // 空输入不发送，也不覆盖上一条真实提交结果
    if (Message.IsEmpty()) return false;
    const double Now=FPlatformTime::Seconds();
    if (Now<NextLocalSendAt) return false;
    NextLocalSendAt=Now+1.1;
    LastSubmissionId=FGuid::NewGuid();
    LastResult=TEXT("Sending"); ServerSubmit(Resident,Message,LastSubmissionId);
    return true;
}
void UDeliveryConversationComponent::ServerSubmit_Implementation(ADeliveryNpcCharacter* Resident,const FString& Text,FGuid SpeechId)
{
    if (!SpeechId.IsValid()) { ClientResult(TEXT("Unavailable"),SpeechId); return; }
    const double Now=FPlatformTime::Seconds();
    if (Now<NextSendAt) { ClientResult(TEXT("TooFast"),SpeechId); return; }
    const FString Message=Text.TrimStartAndEnd();
    if (Message.IsEmpty() || Text.Len()>256) { ClientResult(TEXT("Empty"),SpeechId); return; }
    if (!CanTalkTo(Resident))
    {
        UE_LOG(LogTemp,Log,TEXT("ResidentConversation rejected: player=%s resident=%s reason=Unavailable"),*GetNameSafe(GetOwner()),*GetNameSafe(Resident));
        ClientResult(TEXT("Unavailable"),SpeechId); return;
    }
    // 服务器接收消息后开始计算发送间隔
    NextSendAt=Now+1;
    auto* PC=CastChecked<APlayerController>(GetOwner());
    if (!Resident->HearSpeech(PC->GetPawn(),Message,SpeechId,0)) { ClientResult(TEXT("NotHeard"),SpeechId); return; }
    UE_LOG(LogTemp,Log,TEXT("ResidentConversation accepted: player=%s resident=%s speech=%s pending=%d activeAction=%d"),
        *GetNameSafe(GetOwner()),*GetNameSafe(Resident),*SpeechId.ToString(),Resident->Agent->IsDecisionPending(),Resident->Agent->HasActiveAction());
    ClientResult(TEXT("Heard"),SpeechId);
}
void UDeliveryConversationComponent::ClientResult_Implementation(FName Result,FGuid SpeechId)
{
    // 通过发言编号区分每次发送结果，只更新当前这次发言的提示
    if (SpeechId!=LastSubmissionId) return;
    LastResult=Result; ResultExpiresAt=FPlatformTime::Seconds()+2.5;
}
