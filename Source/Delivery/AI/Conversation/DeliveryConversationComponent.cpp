#include "AI/Conversation/DeliveryConversationComponent.h"
#include "AI/Conversation/DeliveryNpcConversationTrace.h"
#include "AI/Actions/DeliveryNpcCombatComponent.h"
#include "AI/Characters/DeliveryNpcCharacter.h"
#include "AI/Companion/Core/DeliveryCompanionCharacter.h"
#include "AI/Decision/DeliveryNpcAgentComponent.h"
#include "AI/Presentation/DeliveryNpcPresentationComponent.h"
#include "Components/CapsuleComponent.h"
#include "Interaction/DeliveryInteractableComponent.h"
#include "UI/HUD/DeliveryHUD.h"
#include "EnhancedInputComponent.h"
#include "EnhancedInputSubsystems.h"
#include "InputAction.h"
#include "InputMappingContext.h"
#include "Engine/LocalPlayer.h"
#include "GameFramework/PlayerController.h"
#include "EngineUtils.h"
#include "Engine/World.h"
#include "Speech/DeliverySpeechInputComponent.h"
#include "Character/DeliveryCharacter.h"
#include "AbilitySystem/DeliverGameplayTags.h"
#include "AbilitySystemComponent.h"

/// @brief 组件跟着玩家控制器在网络上同步；Tick 默认关闭，只在等开麦结果时打开，游戏暂停时也照常运行
UDeliveryConversationComponent::UDeliveryConversationComponent()
{
    SetIsReplicatedByDefault(true);
    PrimaryComponentTick.bCanEverTick = true;
    PrimaryComponentTick.bStartWithTickEnabled = false;
    PrimaryComponentTick.bTickEvenWhenPaused = true;
}

// ── 补听 ──
// 玩家走进 NPC 附近时，NPC 的那句话可能已经开始播了，本机缺开头的声音
// 本机调用 ServerRequestNpcAudio 一段段向服务器要，服务器调用 ClientReceiveNpcAudio 发回来
// ClientReceiveNpcAudio_Implementation 再调用这位 NPC 的 Presentation->ReceiveAudio_Implementation，把这段接到已有的声音后面播

/// @brief 服务器：玩家请求补发某句 NPC 语音的一段
/// 只给 10 米以内的玩家，或者在听自己同伴开麦回复的主人；每 0.25 秒最多发一段，每段最多 2 KB
/// @param Offset 从压缩声音的第几个字节开始要
void UDeliveryConversationComponent::ServerRequestNpcAudio_Implementation(ADeliveryNpcCharacter* Npc, FGuid TurnId, int32 Offset)
{
    const double Now = FPlatformTime::Seconds();
    const auto* PC = Cast<APlayerController>(GetOwner());
    const APawn* Pawn = PC ? PC->GetPawn() : nullptr;
    if (Now < NextAudioRequestAt || Offset < 0 || !Pawn || !IsValid(Npc) || Npc->GetWorld() != GetWorld() || !Npc->Presentation ||
        (FVector::DistSquared(Pawn->GetActorLocation(), Npc->GetActorLocation()) > FMath::Square(1000.f) &&
         !Npc->Presentation->CanPlayerReceiveLinkedAudio(PC, TurnId)))
        return;
    NextAudioRequestAt = Now + .25;
    TArray<uint8> Audio;
    int32 Total = 0;
    if (!Npc->Presentation->GetSpeechAudioChunk(TurnId, Offset, 2048, Audio, Total))
        return;

    // GetSpeechAudioChunk 一次最多取 2048 字节：太大的一包会挤占同一条连接，NPC 的位置和动作同步会跟着卡
    ClientReceiveNpcAudio(Npc, TurnId, Offset, Total, Audio);
}

/// @brief 本机：收到补发的一段声音，交给这位 NPC 的表现组件接到已有的声音后面播
void UDeliveryConversationComponent::ClientReceiveNpcAudio_Implementation(ADeliveryNpcCharacter* Npc, FGuid TurnId, int32 Offset,
                                                                          int32 Total, const TArray<uint8>& Data)
{
    if (!IsValid(Npc) || Npc->GetWorld() != GetWorld())
        return;
    Npc->Presentation->ReceiveAudio_Implementation(TurnId, Offset, Total, Data);
}

// ── 选人 ──
// 玩家按 IA_Talk 时调用 OpenAimedConversation：先调用 GetAimedNpc，在镜头前方挑出最靠近画面中心、能交谈的 NPC
// 镜头前没人时改找身边的同伴，然后调用 ServerOpenConversation；服务器用 CanTalkTo 再检查一遍
// 通过后服务器调用 ClientOpenConversation，本机再调用 HUD->OpenResidentConversation 打开交谈面板

/// @brief 本机：把 IA_Talk 绑到 OpenAimedConversation，加上交谈的输入映射，同时绑定开麦和检查器的按键
/// 换了 InputComponent（例如换了角色）时先解掉旧绑定再绑新的
void UDeliveryConversationComponent::BindInput()
{
    BindSpeechInput();
    BindInspectorInput();
    auto* PC = Cast<APlayerController>(GetOwner());
    if (!PC || !PC->IsLocalController())
        return;

    // 从固定路径加载 IA_Talk 和 IMC_ResidentConversation
    auto* Input = Cast<UEnhancedInputComponent>(PC->InputComponent);
    auto* Action = LoadObject<UInputAction>(nullptr, TEXT("/Game/NPC/Residents/Input/IA_Talk"));
    auto* Mapping = LoadObject<UInputMappingContext>(nullptr, TEXT("/Game/NPC/Residents/Input/IMC_ResidentConversation"));

    // PC->InputComponent 换了，或者 TalkInputBindingHandle 为 0（还没绑过），才重新绑定；先从旧的 InputComponent 删掉旧绑定
    if (Input && Action && (BoundTalkInput.Get() != Input || !TalkInputBindingHandle))
    {
        if (auto* Old = BoundTalkInput.Get(); Old && TalkInputBindingHandle)
            Old->RemoveBindingByHandle(TalkInputBindingHandle);
        BoundTalkInput = Input;
        TalkInputBindingHandle =
            Input->BindAction(Action, ETriggerEvent::Started, this, &UDeliveryConversationComponent::OpenAimedConversation)
                .GetHandle();
    }

    // IMC_ResidentConversation 还没加到输入子系统才加
    if (Mapping)
        if (auto* Sub = ULocalPlayer::GetSubsystem<UEnhancedInputLocalPlayerSubsystem>(PC->GetLocalPlayer());
            Sub && !Sub->HasMappingContext(Mapping))
            Sub->AddMappingContext(Mapping, 1);
}

/// @brief 判断玩家现在能不能和这位 NPC 说话；本机选人和服务器检查用的是同一套条件
/// 条件：NPC 在附近（LocalConversationRangeCm 以内）、没被打倒、玩家没晕倒、两人之间说话不被墙挡住
bool UDeliveryConversationComponent::CanTalkTo(const ADeliveryNpcCharacter* Resident) const
{
    const auto* PC = Cast<APlayerController>(GetOwner());
    const APawn* Pawn = PC ? PC->GetPawn() : nullptr;
    if (!Pawn || !IsValid(Resident) || Resident->GetWorld() != GetWorld() ||

        // NPC 被打倒（IsKnockedDown）时不能交谈；IsDeliberatelyProne（自己故意躺下）不算，同伴被打倒也照样能听主人说话
        (Resident->IsKnockedDown() && !Resident->Combat->IsDeliberatelyProne() &&
         !Resident->IsA<ADeliveryCompanionCharacter>()) ||
        FVector::DistSquared(Pawn->GetActorLocation(), Resident->GetActorLocation()) >
            FMath::Square(DeliveryNpcConversationTrace::LocalConversationRangeCm))
        return false;

    // 玩家带 TAG_State_Stunned（晕倒）时不能交谈；骑车时 ResolveCharacter 从车上找回玩家的角色
    if (const auto* Character = ADeliveryHUD::ResolveCharacter(const_cast<APawn*>(Pawn)))
        if (const auto* ASC = Character->GetAbilitySystemComponent(); ASC && ASC->HasMatchingGameplayTag(TAG_State_Stunned))
            return false;
    return DeliveryNpcConversationTrace::HasClearSpeechPath(Resident, Pawn);
}

/// @brief 本机：取镜头的位置和朝向，交给 FindNpcInView 选出镜头对着的那位 NPC
ADeliveryNpcCharacter* UDeliveryConversationComponent::GetAimedNpc() const
{
    const auto* PC = Cast<APlayerController>(GetOwner());
    if (!PC || !PC->IsLocalController() || !PC->GetPawn())
        return nullptr;
    FVector ViewLocation;
    FRotator ViewRotation;
    PC->GetPlayerViewPoint(ViewLocation, ViewRotation);
    return FindNpcInView(ViewLocation, ViewRotation.Vector());
}

/// @brief 选出镜头前方 30 度以内、能交谈、中间没被挡住的 NPC
/// 准星直接对着身体的优先，几位都对着时选最近的；只是擦过边缘时选偏得最少的
ADeliveryNpcCharacter* UDeliveryConversationComponent::FindNpcInView(const FVector& ViewLocation,
                                                                     const FVector& ViewDirection) const
{
    const auto* PC = Cast<APlayerController>(GetOwner());
    const APawn* Pawn = PC ? PC->GetPawn() : nullptr;
    if (!Pawn || ViewLocation.ContainsNaN() || ViewDirection.ContainsNaN())
        return nullptr;
    const FVector Direction = ViewDirection.GetSafeNormal();
    if (Direction.IsNearlyZero())
        return nullptr;

    // 第三人称镜头在角色身后，ViewLength 等于镜头到角色的距离再加 700 cm，所以找的是角色前方 7 米以内
    const double ViewLength = FVector::Distance(ViewLocation, Pawn->GetActorLocation()) + 700.0;
    const FVector ViewEnd = ViewLocation + Direction * ViewLength;
    ADeliveryNpcCharacter* Best = nullptr;
    bool bBestDirect = false;
    double BestAlong = MAX_dbl;
    double BestAngle = MAX_dbl;

    // 对每位 bInteractEnabled 的 NPC，算视线线段到它胶囊体中轴的最近距离 Miss，以及最近点在视线方向上的距离 Along
    for (TActorIterator<ADeliveryNpcCharacter> It(GetWorld()); It; ++It)
    {
        auto* Npc = *It;
        if (!Npc->ConversationInteractable->bInteractEnabled)
            continue;
        const UCapsuleComponent* Capsule = Npc->GetCapsuleComponent();
        const double Radius = Capsule->GetScaledCapsuleRadius();
        const FVector Center = Capsule->GetComponentLocation();
        if (FVector::DotProduct(Center - ViewLocation, Direction) <= 0.0)
            continue;
        const FVector Axis =
            Capsule->GetUpVector() * FMath::Max(0.0, static_cast<double>(Capsule->GetScaledCapsuleHalfHeight()) - Radius);
        FVector RayPoint, BodyPoint;
        FMath::SegmentDistToSegmentSafe(ViewLocation, ViewEnd, Center - Axis, Center + Axis, RayPoint, BodyPoint);
        const double Miss = FVector::Distance(RayPoint, BodyPoint);
        const double Along = FVector::DotProduct(RayPoint - ViewLocation, Direction);

        // EdgeAllowance 随 Along 增大，相当于镜头前方 30 度的锥形范围，俯视或拉远镜头时不用精确瞄准
        const double EdgeAllowance = Along * FMath::Tan(FMath::DegreesToRadians(30.0));
        if (Miss > Radius + EdgeAllowance || !CanTalkTo(Npc))
            continue;

        // 从镜头到 BodyPoint 做一次 ECC_Visibility 射线，先碰到的不是这位 NPC 就跳过
        FHitResult Hit;
        FCollisionQueryParams Params(SCENE_QUERY_STAT(NpcConversationAim), false, Pawn);
        if (GetWorld()->LineTraceSingleByChannel(Hit, ViewLocation, BodyPoint, ECC_Visibility, Params) && Hit.GetActor() != Npc)
            continue;
        const double Angle = Miss / FMath::Max(Along, 1.0);
        const bool bDirect = Miss <= Radius;

        // bDirect（Miss 不超过胶囊半径，准星对着身体）的优先；两位都是 bDirect 时选 Along 小的，都不是时选 Angle 小的
        const bool bBetter = !Best || (bDirect != bBestDirect ? bDirect : (bDirect ? Along < BestAlong : Angle < BestAngle));
        if (bBetter)
        {
            Best = Npc;
            bBestDirect = bDirect;
            BestAlong = Along;
            BestAngle = Angle;
        }
    }
    return Best;
}

/// @brief 本机：玩家按下 IA_Talk 时调用，选出要交谈的 NPC，请服务器打开交谈
void UDeliveryConversationComponent::OpenAimedConversation()
{
    auto* Npc = GetAimedNpc();

    // GetAimedNpc 没找到人，就调用 ResolveVoiceCompanion 找自家同伴
    if (!Npc)
    {
        const auto* PC = Cast<APlayerController>(GetOwner());
        const APawn* Pawn = PC && PC->IsLocalController() ? PC->GetPawn() : nullptr;
        auto* Companion = Pawn ? Cast<ADeliveryCompanionCharacter>(ResolveVoiceCompanion()) : nullptr;

        // 同伴要在 300 cm 以内，或者 IsMountedOnCompanionBike（骑在同伴的车上，可能停在几米外），而且 CanTalkTo 通过
        if (Companion && Companion->ConversationInteractable->bInteractEnabled &&
            (Companion->IsMountedOnCompanionBike() ||
             FVector::DistSquared(Pawn->GetActorLocation(), Companion->GetActorLocation()) <= FMath::Square(300.f)) &&
            CanTalkTo(Companion))
            Npc = Companion;
    }
    if (!Npc)
        return;

    // PrepareConversationFacingInput 生成转身序号，和 LocalFacingPawn 一起传给 ServerOpenConversation
    const uint64 FacingSequence = PrepareConversationFacingInput();
    ServerOpenConversation(Npc, FacingSequence, LocalFacingPawn.Get());
}

// ── 面板 ──
// 交谈面板由 HUD 创建；面板的发送按钮调用 Submit，面板每帧读 LastResult 显示这句话的处理结果
// GetProvokeText 给面板提供一句示例话

/// @brief 给面板一句挑拨两位 NPC 比试的示例话：点出交谈对象身边 8 米内的另一位 NPC，问他俩谁厉害
/// NPC 听到后会不会真动手，由模型决定
/// @return 附近没有别人时改成挑衅交谈对象本人的句子
FString UDeliveryConversationComponent::GetProvokeText(const ADeliveryNpcCharacter* Npc) const
{
    if (!IsValid(Npc) || Npc->GetWorld() != GetWorld())
        return FString();
    const ADeliveryNpcCharacter* Other = nullptr;
    double Closest = FMath::Square(800.0);
    for (TActorIterator<ADeliveryNpcCharacter> It(GetWorld()); It; ++It)
    {
        if (*It == Npc || !CanTalkTo(*It))
            continue;
        const double Distance = FVector::DistSquared(It->GetActorLocation(), Npc->GetActorLocation());
        if (Distance < Closest)
        {
            Closest = Distance;
            Other = *It;
        }
    }
    return Other ? FString::Printf(TEXT("%s，%s就在旁边。你俩到底谁厉害？敢不敢比划两下，输的当面认怂？"),
                                   *Npc->DisplayName.ToString(), *Other->DisplayName.ToString())
                 : FString::Printf(TEXT("%s，你就只敢嘴上横？我站这儿呢，你敢动手吗？"), *Npc->DisplayName.ToString());
}

/// @brief 服务器：玩家请求和这位 NPC 交谈，再检查一遍条件，通过后让主角转身并请本机打开面板
void UDeliveryConversationComponent::ServerOpenConversation_Implementation(ADeliveryNpcCharacter* Resident, uint64 FacingSequence,
                                                                           APawn* FacingPawn)
{
    // 先调用 RegisterConversationFacingInput 登记转身序号；CanTalkTo 不通过就直接返回，不调用 ConfirmConversationFacingInput，主角不转身
    RegisterConversationFacingInput(Resident, FGuid(), FacingSequence, FacingPawn);
    if (!CanTalkTo(Resident))
        return;
    if (FacingSequence)
    {
        ConfirmConversationFacingInput(Resident, FGuid(), FacingSequence);
    }
    ClientOpenConversation(Resident);
}

/// @brief 本机：清掉上一句的结果，请 HUD 打开和这位 NPC 的交谈面板
void UDeliveryConversationComponent::ClientOpenConversation_Implementation(ADeliveryNpcCharacter* Resident)
{
    auto* PC = Cast<APlayerController>(GetOwner());
    if (!PC || !PC->IsLocalController() || !IsValid(Resident))
        return;
    LastResult = NAME_None;
    if (auto* HUD = Cast<ADeliveryHUD>(PC->GetHUD()))
        HUD->OpenResidentConversation(Resident);
}

// ── 打字 ──
// 玩家在面板里发送：本机 Submit 生成 ClientToken（本机给这句话的编号），调用 ServerSubmit
// 服务器检查后调用 TryBeginText 登记这一轮，再调用 ClientSubmissionBound 把 Handle.Token（服务器给这句话的编号）告诉本机
// 然后调用 DeliverFinalText 把这句话交给 NPC；处理结果用 ClientResult 发回本机，面板按 Heard、TooFast 等结果显示提示

/// @brief 本机：玩家在面板里发送一句话
/// 去掉首尾空格后为空或超过 256 字不发；两次发送至少隔 1.1 秒
/// @return 发出去了返回 true；面板据此决定是否清空输入框
bool UDeliveryConversationComponent::Submit(ADeliveryNpcCharacter* Resident, const FString& Text)
{
    auto* PC = Cast<APlayerController>(GetOwner());
    if (!PC || !PC->IsLocalController() || !IsValid(Resident) || Resident->GetWorld() != GetWorld())
        return false;
    const FString Message = Text.TrimStartAndEnd();

    // 空的或超过 256 字的不发，直接返回，LastResult 保持上一句的结果
    if (Message.IsEmpty() || Message.Len() > 256)
        return false;
    const double Now = FPlatformTime::Seconds();
    if (Now < NextLocalSendAt)
        return false;

    // 打字发送前先调用 Speech->CancelHold 停止正在识别的语音；LocalVoiceToken 有效说明开麦那句还没结束，调用 ServerCancelVoice 取消它
    if (auto* Speech = GetSpeechInput())
        Speech->CancelHold();
    if (LocalVoiceToken.IsValid())
    {
        ServerCancelVoice(LocalVoiceToken.Id, LocalVoiceSequence);
        ResetLocalVoice();
    }

    // 生成 ClientToken，记进 LastSubmissionId；ClientSubmissionBound 收到服务器编号后会替换它
    // LastResult 设为 Sending，ResultExpiresAt 设为 2.5 秒后，面板在这之前显示发送中的提示
    NextLocalSendAt = Now + 1.1;
    const FGuid ClientToken = FGuid::NewGuid();
    const uint64 Sequence = NextInputSequence();
    LastSubmissionId = ClientToken;
    LastResult = TEXT("Sending");
    ResultExpiresAt = Now + 2.5;
    const uint64 FacingSequence = PrepareConversationFacingInput();

    // 先调用 ServerSubmit，再调用 OnSubmitted.Broadcast，让面板显示玩家原话
    ServerSubmit(Resident, Message, ClientToken, Sequence, FacingSequence, LocalFacingPawn.Get());

    // 单机时 ServerSubmit 会立即执行，其中的 ClientResult 等回调可能又调用了 NextInputSequence 开始新输入
    // 那样 LastLocalInputSequence 已经不等于 Sequence，不调用 OnSubmitted.Broadcast，免得面板清掉新输入的草稿
    if (LastLocalInputSequence == Sequence)
        OnSubmitted.Broadcast(Resident, LastSubmissionId, Message);
    return true;
}

/// @brief 服务器：收到玩家打字发来的一句话，逐项检查，不通过就用 ClientResult 告诉本机原因
/// 依次检查：编号有效、两次发送间隔至少 1 秒、长度、能不能交谈、NPC 有没有被别的玩家占着
void UDeliveryConversationComponent::ServerSubmit_Implementation(ADeliveryNpcCharacter* Resident, const FString& Text,
                                                                 FGuid SpeechId, uint64 InputSequence, uint64 FacingSequence,
                                                                 APawn* FacingPawn)
{
    // 先调用 RegisterConversationFacingInput 登记转身序号；DeliverFinalText 交给 NPC 成功后才调用 ConfirmConversationFacingInput
    RegisterConversationFacingInput(Resident, SpeechId, FacingSequence, FacingPawn);
    if (!SpeechId.IsValid())
    {
        ClientResult(TEXT("Unavailable"), SpeechId);
        return;
    }

    // 当前时间早于 NextSendAt（上一句交给 NPC 后 1 秒）返回 TooFast；去掉首尾空格后为空或超过 256 字返回 Empty
    const double Now = FPlatformTime::Seconds();
    if (Now < NextSendAt)
    {
        ClientResult(TEXT("TooFast"), SpeechId);
        return;
    }
    const FString Message = Text.TrimStartAndEnd();
    if (Message.IsEmpty() || Text.Len() > 256)
    {
        ClientResult(TEXT("Empty"), SpeechId);
        return;
    }

    // CanTalkTo 不通过：写一条日志记下距离和 NPC 是否倒地，再返回 Unavailable
    if (!CanTalkTo(Resident))
    {
        const auto* PC = Cast<APlayerController>(GetOwner());
        const APawn* Pawn = PC ? PC->GetPawn() : nullptr;
        const float Distance =
            Pawn && IsValid(Resident) ? FVector::Dist(Pawn->GetActorLocation(), Resident->GetActorLocation()) : -1.f;
        UE_LOG(LogTemp, Log,
               TEXT("ResidentConversation rejected: player=%s resident=%s reason=Unavailable pawn=%s distanceCm=%.1f down=%d"),
               *GetNameSafe(GetOwner()), *GetNameSafe(Resident), *GetNameSafe(Pawn), Distance,
               IsValid(Resident) && Resident->IsKnockedDown());
        ClientResult(TEXT("Unavailable"), SpeechId);
        return;
    }

    // 调用 TryBeginText 登记这一轮；这位 NPC 被别的玩家占着时返回 Busy
    auto* PC = CastChecked<APlayerController>(GetOwner());
    auto* Turns = GetWorld()->GetSubsystem<UDeliveryConversationTurnSubsystem>();
    if (!Turns)
    {
        ClientResult(TEXT("Unavailable"), SpeechId);
        return;
    }
    FDeliveryConversationTurnHandle Handle;
    TArray<FDeliveryConversationTurnLease> Replaced;
    const auto Result = Turns->TryBeginText(PC, Resident, SpeechId, InputSequence, Handle, Replaced);

    // AlreadyActive 说明是网络重发的同一句话：只再调用一次 ClientSubmissionBound，不再调用 DeliverFinalText
    if (Result == EDeliveryConversationTurnResult::AlreadyActive)
    {
        ClientSubmissionBound(SpeechId, Handle.Token);
        return;
    }
    if (Result != EDeliveryConversationTurnResult::Accepted)
    {
        ClientResult(Result == EDeliveryConversationTurnResult::Busy ? TEXT("Busy") : TEXT("Unavailable"), SpeechId);
        return;
    }

    // CancelReplacedLeases 打断被顶掉的旧轮次，CancelNpcMessageTurns 打断这位 NPC 和其他 NPC 之间的接话，WatchReplies 订阅它回复结束的通知
    // 然后调用 ClientSubmissionBound 告诉本机服务器编号，最后调用 DeliverFinalText 把这句话交给 NPC
    CancelReplacedLeases(Replaced);
    CancelNpcMessageTurns(Resident);
    WatchReplies(Resident);
    ClientSubmissionBound(SpeechId, Handle.Token);
    DeliverFinalText(Resident, Message, Handle);
}

/// @brief 本机：服务器接收这句话后发来服务器编号，之后用它对结果和回复；同时通知面板换编号
void UDeliveryConversationComponent::ClientSubmissionBound_Implementation(FGuid ClientToken, FGuid MessageId)
{
    if (LastSubmissionId != ClientToken || !MessageId.IsValid())
        return;
    LastSubmissionId = MessageId;
    OnSubmissionBound.Broadcast(ClientToken, MessageId);
}

// ── 交付 ──
// 打字由 ServerSubmit_Implementation 调用 DeliverFinalText，开麦由 ServerSubmitVoice_Implementation 调用
// DeliverFinalText 确认这一轮还归这位玩家，调用 CommitFinal 记下文字已交给 NPC，再调用 HearSpeech 或 HearCompanionVoice

/// @brief 服务器：把玩家这句话交给交谈对象，再让附近的 NPC 旁听
/// 打字时其他 NPC 都能听到，并知道这句话是对谁说的；开麦时只有玩家身边 9 米内最近的几位听到，而且只记住不回话
/// @return 交给 NPC 成功返回 true；同一句话已经交过时也返回 true
bool UDeliveryConversationComponent::DeliverFinalText(ADeliveryNpcCharacter* Resident, const FString& Message,
                                                      const FDeliveryConversationTurnHandle& Handle)
{
    auto* PC = Cast<APlayerController>(GetOwner());
    auto* Turns = GetWorld() ? GetWorld()->GetSubsystem<UDeliveryConversationTurnSubsystem>() : nullptr;
    if (!PC || !Turns)
    {
        ClientResult(TEXT("Unavailable"), Handle.Token);
        return false;
    }

    // 这位 NPC 当前的轮次必须是传入的 Handle，而且 Owner 是这个 PC，否则返回 Expired
    FDeliveryConversationTurnLease Lease;
    if (!Turns->GetLease(Resident, Lease) || !(Lease.Handle == Handle) || Lease.Owner.Get() != PC)
    {
        ClientResult(TEXT("Expired"), Handle.Token);
        return false;
    }

    // Phase 已经是 AwaitingReply 说明这句话交过了（网络重发），直接返回 true，不打断 NPC 正在说的回复
    if (Lease.Phase == EDeliveryConversationTurnPhase::AwaitingReply)
        return true;

    // 打字用 CanTalkTo、开麦用 CanVoiceTalkTo 再检查一遍，然后依次调用 ValidateFinal 和 CommitFinal；任何一步不通过就调用 Turns->Cancel
    const bool bVoiceMessage = IsVoiceTurn(Handle);
    if (!(bVoiceMessage ? CanVoiceTalkTo(Resident) : CanTalkTo(Resident)) || Message.IsEmpty() || Message.Len() > 256 ||
        Turns->ValidateFinal(PC, Resident, Handle) != EDeliveryConversationTurnResult::Accepted ||
        !Turns->CommitFinal(PC, Resident, Handle, Handle.Token))
    {
        Turns->Cancel(PC, Handle);
        ClientResult(Message.IsEmpty() ? TEXT("Empty") : TEXT("Unavailable"), Handle.Token);
        return false;
    }

    // NextSendAt 设为 1 秒后，再交给交谈对象：打字调用 HearSpeech，开麦调用 HearCompanionVoice
    NextSendAt = FPlatformTime::Seconds() + 1;
    const FGuid SpeechId = Handle.Token;
    const bool bHeard = bVoiceMessage ? Resident->HearCompanionVoice(PC, Message, SpeechId)
                                      : Resident->HearSpeech(PC->GetPawn(), Message, SpeechId, 0);
    if (!bHeard)
    {
        Turns->Cancel(PC, Handle);
        ClientResult(TEXT("NotHeard"), SpeechId);
        return false;
    }

    // 打字时调用 ConfirmConversationFacingInput，传入这一轮登记时的 Lease.ClientToken
    // 玩家之后又发过别的话的话，ServerFacingClientToken 已经变了，对不上，主角不会转回这位 NPC
    if (!bVoiceMessage)
        ConfirmConversationFacingInput(Resident, Lease.ClientToken);

    // 再让其他 NPC 旁听：调用它们的 HearSpeech 时传入 Resident 作为 Addressee，它们知道这句话是对谁说的
    // 开麦时只有玩家身边 900 cm 内最近的 VoiceOverhearLimit 位（最多 8 位）听到；打字时其他 NPC 都能听到
    if (bVoiceMessage)
    {
        TArray<ADeliveryNpcCharacter*> Nearby;
        const FVector SpeakerLocation = PC->GetPawn()->GetActorLocation();
        for (TActorIterator<ADeliveryNpcCharacter> It(GetWorld()); It; ++It)
            if (*It != Resident && FVector::DistSquared(It->GetActorLocation(), SpeakerLocation) <= FMath::Square(900.f))
                Nearby.Add(*It);
        Nearby.Sort(
            [&](const ADeliveryNpcCharacter& A, const ADeliveryNpcCharacter& B)
            {
                return FVector::DistSquared(A.GetActorLocation(), SpeakerLocation) <
                       FVector::DistSquared(B.GetActorLocation(), SpeakerLocation);
            });
        int32 HeardCount = 0;
        for (auto* Listener : Nearby)
        {
            if (HeardCount >= FMath::Clamp(VoiceOverhearLimit, 0, 8))
                break;

            // 最后一个参数 bRequestReply 传 false：只记住、不回话，不会为开麦的每句话都去问一次模型
            if (Listener->HearSpeech(PC->GetPawn(), Message, SpeechId, 0, Resident, FGuid(), false))
                ++HeardCount;
        }
    }
    else
        for (TActorIterator<ADeliveryNpcCharacter> It(GetWorld()); It; ++It)
            if (*It != Resident)
                It->HearSpeech(PC->GetPawn(), Message, SpeechId, 0, Resident);

    // 写日志，再调用 ClientResult 发回 Heard
    UE_LOG(LogTemp, Log, TEXT("ResidentConversation accepted: player=%s resident=%s speech=%s pending=%d activeAction=%d"),
           *GetNameSafe(GetOwner()), *GetNameSafe(Resident), *SpeechId.ToString(), Resident->Agent->IsDecisionPending(),
           Resident->Agent->HasActiveAction());
    ClientResult(TEXT("Heard"), SpeechId);
    return true;
}

/// @brief 本机：记下服务器对某句话的处理结果，面板每帧按编号读取，2.5 秒后不再显示
/// @param Result 结果，例如 Heard、TooFast、Busy、Unavailable
void UDeliveryConversationComponent::ClientResult_Implementation(FName Result, FGuid SpeechId)
{
    // 开麦那句话（LocalVoiceHandle.Token 等于 SpeechId）有了结果，调用 ResetLocalVoice，本机可以开始下一句开麦
    // 这一步放在下面的 LastSubmissionId 检查之前，旧句子的结果也会执行
    if (bVoiceSubmissionSent && LocalVoiceHandle.IsValid() && LocalVoiceHandle.Token == SpeechId)
        ResetLocalVoice();

    // SpeechId 不是 LastSubmissionId（最近一句的编号）时不更新 LastResult
    if (SpeechId != LastSubmissionId)
        return;
    LastResult = Result;
    ResultExpiresAt = FPlatformTime::Seconds() + 2.5;
}
