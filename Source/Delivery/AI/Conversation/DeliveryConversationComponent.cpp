#include "AI/Conversation/DeliveryConversationComponent.h"
#include "AI/Characters/DeliveryNpcCharacter.h"
#include "AI/Decision/DeliveryNpcAgentComponent.h"
#include "AI/Presentation/DeliveryNpcPresentationComponent.h"
#include "Components/CapsuleComponent.h"
#include "Interaction/DeliveryInteractableComponent.h"
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
    if (Input && Action) Input->BindAction(Action,ETriggerEvent::Started,this,&UDeliveryConversationComponent::OpenAimedConversation);
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
ADeliveryNpcCharacter* UDeliveryConversationComponent::GetAimedNpc() const
{
    const auto* PC=Cast<APlayerController>(GetOwner());
    if (!PC || !PC->IsLocalController() || !PC->GetPawn()) return nullptr;
    FVector ViewLocation;
    FRotator ViewRotation;
    PC->GetPlayerViewPoint(ViewLocation,ViewRotation);
    return FindNpcInView(ViewLocation,ViewRotation.Vector());
}
ADeliveryNpcCharacter* UDeliveryConversationComponent::FindNpcInView(
    const FVector& ViewLocation,const FVector& ViewDirection) const
{
    const auto* PC=Cast<APlayerController>(GetOwner());
    const APawn* Pawn=PC?PC->GetPawn():nullptr;
    if (!Pawn || ViewLocation.ContainsNaN() || ViewDirection.ContainsNaN()) return nullptr;
    const FVector Direction=ViewDirection.GetSafeNormal();
    if (Direction.IsNearlyZero()) return nullptr;
    // 第三人称镜头在角色身后，射线长度需要包含镜头到角色的距离
    const double ViewLength=FVector::Distance(ViewLocation,Pawn->GetActorLocation())+700.0;
    const FVector ViewEnd=ViewLocation+Direction*ViewLength;
    ADeliveryNpcCharacter* Best=nullptr;
    bool bBestDirect=false;
    double BestAlong=MAX_dbl;
    double BestAngle=MAX_dbl;
    for (TActorIterator<ADeliveryNpcCharacter> It(GetWorld());It;++It)
    {
        auto* Npc=*It;
        if (!Npc->ConversationInteractable->bInteractEnabled) continue;
        const UCapsuleComponent* Capsule=Npc->GetCapsuleComponent();
        const double Radius=Capsule->GetScaledCapsuleRadius();
        const FVector Center=Capsule->GetComponentLocation();
        if (FVector::DotProduct(Center-ViewLocation,Direction)<=0.0) continue;
        const FVector Axis=Capsule->GetUpVector()*FMath::Max(0.0,
            static_cast<double>(Capsule->GetScaledCapsuleHalfHeight())-Radius);
        FVector RayPoint,BodyPoint;
        FMath::SegmentDistToSegmentSafe(ViewLocation,ViewEnd,Center-Axis,Center+Axis,RayPoint,BodyPoint);
        const double Miss=FVector::Distance(RayPoint,BodyPoint);
        // 头部和身体都可选中，边缘只留少量容差，不选远离准星的邻居
        if (Miss>Radius+10.0 || !CanTalkTo(Npc)) continue;
        FHitResult Hit;
        FCollisionQueryParams Params(SCENE_QUERY_STAT(NpcConversationAim),false,Pawn);
        if (GetWorld()->LineTraceSingleByChannel(Hit,ViewLocation,BodyPoint,ECC_Visibility,Params) &&
            Hit.GetActor()!=Npc) continue;
        const double Along=FVector::DotProduct(RayPoint-ViewLocation,Direction);
        const double Angle=Miss/FMath::Max(Along,1.0);
        const bool bDirect=Miss<=Radius;
        // 前后重叠时选择挡在前面的人；只有擦过边缘时才比较偏离准星的角度
        const bool bBetter=!Best || (bDirect!=bBestDirect?bDirect:
            (bDirect?Along<BestAlong:Angle<BestAngle));
        if (bBetter)
        {
            Best=Npc;
            bBestDirect=bDirect;
            BestAlong=Along;
            BestAngle=Angle;
        }
    }
    return Best;
}
void UDeliveryConversationComponent::OpenAimedConversation()
{
    if (auto* Npc=GetAimedNpc()) ServerOpenConversation(Npc);
}
FString UDeliveryConversationComponent::GetProvokeText(const ADeliveryNpcCharacter* Npc) const
{
    if (!IsValid(Npc) || Npc->GetWorld()!=GetWorld()) return FString();
    const ADeliveryNpcCharacter* Other=nullptr;
    double Closest=FMath::Square(800.0);
    for (TActorIterator<ADeliveryNpcCharacter> It(GetWorld());It;++It)
    {
        if (*It==Npc || !CanTalkTo(*It)) continue;
        const double Distance=FVector::DistSquared(It->GetActorLocation(),Npc->GetActorLocation());
        if (Distance<Closest) { Closest=Distance; Other=*It; }
    }
    return Other
        ? FString::Printf(TEXT("%s，你敢不敢跟%s比划两下？我看你只敢动嘴。"),
            *Npc->DisplayName.ToString(),*Other->DisplayName.ToString())
        : FString::Printf(TEXT("%s，你就只敢嘴上横？我站这儿呢，你敢动手吗？"),*Npc->DisplayName.ToString());
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
    LastResult=TEXT("Sending"); ResultExpiresAt=Now+2.5;
    ServerSubmit(Resident,Message,LastSubmissionId);
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
    // 同在现场的人也能听到原问题，但只有交谈对象收到需要回复的玩家发言
    for (TActorIterator<ADeliveryNpcCharacter> It(GetWorld());It;++It)
        if (*It!=Resident) It->HearSpeech(PC->GetPawn(),Message,SpeechId,0,Resident);
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
