#include "AI/Conversation/DeliveryConversationComponent.h"
#include "AI/Characters/DeliveryNpcCharacter.h"
#include "AI/Decision/DeliveryNpcAgentComponent.h"
#include "Speech/DeliverySpeechInputComponent.h"
#include "UI/HUD/DeliveryHUD.h"
#include "EnhancedInputComponent.h"
#include "EnhancedInputSubsystems.h"
#include "InputAction.h"
#include "InputMappingContext.h"
#include "Engine/LocalPlayer.h"
#include "Engine/World.h"
#include "GameFramework/PlayerController.h"

/// @brief 开始时调用 BindSpeechInput 绑定开麦按键；服务器上还订阅 OnLeaseExpired，某一轮超时后调用 HandleLeaseExpired
void UDeliveryConversationComponent::BeginPlay()
{
    Super::BeginPlay();
    bEndingPlay = false;
    BindSpeechInput();
    if (GetOwner()->HasAuthority())
        if (auto* Turns = GetWorld()->GetSubsystem<UDeliveryConversationTurnSubsystem>())
        {
            BoundTurns = Turns;
            Turns->OnLeaseExpired.AddUObject(this, &UDeliveryConversationComponent::HandleLeaseExpired);
        }
}

// ── 开麦 ──
// 玩家按 IA_PushToTalk 打开麦克风后，语音识别组件 UDeliverySpeechInputComponent 自动把一句句话切出来
// 每句开始时它调用 HandleVoiceStarted，HandleVoiceStarted 调用 ServerBeginVoice；服务器同意后调用 ClientVoiceGranted，本机把 Handle（服务器给这句话的发言编号）存进 LocalVoiceHandle
// 识别出文字后它调用 HandleVoiceText，文字存进 FinalVoiceText；LocalVoiceHandle 和 FinalVoiceText 都有了，SubmitReadyVoiceText 才调用 ServerSubmitVoice
// 服务器的 ServerSubmitVoice_Implementation 检查后调用 DeliverFinalText；本机任何一步失败都调用 RejectLocalVoice，停掉这句并提示玩家

/// @brief 找到同一个玩家控制器上的语音识别组件
UDeliverySpeechInputComponent* UDeliveryConversationComponent::GetSpeechInput() const
{
    return GetOwner() ? GetOwner()->FindComponentByClass<UDeliverySpeechInputComponent>() : nullptr;
}

/// @brief 本机：订阅语音识别组件的 OnTurnStarted、OnHoldReleased、OnTurnCancelled、OnFinalText，并把 IA_PushToTalk 绑到 StartVoiceFromKey
/// 语音识别组件或 InputComponent 换了时，先从旧的上面解绑，再绑新的
void UDeliveryConversationComponent::BindSpeechInput()
{
    auto* PC = Cast<APlayerController>(GetOwner());
    if (bEndingPlay || !PC || !PC->IsLocalController())
        return;
    auto* Speech = GetSpeechInput();
    if (Speech && BoundSpeech.Get() != Speech)
    {
        if (auto* Old = BoundSpeech.Get())
        {
            Old->OnTurnStarted.RemoveAll(this);
            Old->OnHoldReleased.RemoveAll(this);
            Old->OnTurnCancelled.RemoveAll(this);
            Old->OnFinalText.RemoveAll(this);
        }
        BoundSpeech = Speech;
        Speech->OnTurnStarted.AddUObject(this, &UDeliveryConversationComponent::HandleVoiceStarted);
        Speech->OnHoldReleased.AddUObject(this, &UDeliveryConversationComponent::HandleVoiceReleased);
        Speech->OnTurnCancelled.AddUObject(this, &UDeliveryConversationComponent::HandleVoiceCancelled);
        Speech->OnFinalText.AddUObject(this, &UDeliveryConversationComponent::HandleVoiceText);
    }
    auto* Input = Cast<UEnhancedInputComponent>(PC->InputComponent);
    if (!Input)
        return;
    auto* Action = LoadObject<UInputAction>(nullptr, TEXT("/Game/UI/Conversation/Input/IA_PushToTalk"));
    auto* Mapping = LoadObject<UInputMappingContext>(nullptr, TEXT("/Game/UI/Conversation/Input/IMC_VoiceConversation"));
    if (!Action || !Mapping)
    {
        UE_LOG(LogTemp, Warning, TEXT("DeliverySpeech: voice toggle input assets are missing"));
        return;
    }

    // PC->InputComponent 换了，或者 VoiceInputBindingHandles 不是正好一个，才重新绑定 IA_PushToTalk
    if (BoundVoiceInput.Get() != Input || VoiceInputBindingHandles.Num() != 1)
    {
        if (auto* Old = BoundVoiceInput.Get())
            for (uint32 Handle : VoiceInputBindingHandles)
                Old->RemoveBindingByHandle(Handle);
        VoiceInputBindingHandles.Reset();
        BoundVoiceInput = Input;
        VoiceInputBindingHandles.Add(
            Input->BindAction(Action, ETriggerEvent::Started, this, &UDeliveryConversationComponent::StartVoiceFromKey)
                .GetHandle());
    }
    if (auto* Sub = ULocalPlayer::GetSubsystem<UEnhancedInputLocalPlayerSubsystem>(PC->GetLocalPlayer()))
    {
        // 输入子系统换了：bOwnsVoiceMapping 为 true 时从旧子系统删掉 VoiceMapping，再记下新的子系统
        if (VoiceInputSubsystem.Get() != Sub || VoiceMapping.Get() != Mapping)
        {
            if (auto* Old = VoiceInputSubsystem.Get(); Old && bOwnsVoiceMapping && VoiceMapping.IsValid())
                Old->RemoveMappingContext(VoiceMapping.Get());
            VoiceInputSubsystem = Sub;
            VoiceMapping = Mapping;
            bOwnsVoiceMapping = false;
        }

        // IMC_VoiceConversation 还没加上才加，并把 bOwnsVoiceMapping 设为 true；EndPlay 只删这个组件自己加的映射
        if (!Sub->HasMappingContext(Mapping))
        {
            Sub->AddMappingContext(Mapping, 2);
            bOwnsVoiceMapping = true;
        }
    }
}

/// @brief 交谈面板打开时调用，把 Resident 记进 ConversationTarget
void UDeliveryConversationComponent::SetConversationTarget(ADeliveryNpcCharacter* Resident)
{
    if (IsValid(Resident) && Resident->GetWorld() == GetWorld())
    {
        ConversationTarget = Resident;
    }
}

/// @brief 面板关闭或换人时调用；Resident 是当前的 ConversationTarget 才清掉
void UDeliveryConversationComponent::ClearConversationTarget(ADeliveryNpcCharacter* Resident)
{
    if (ConversationTarget.Get() == Resident)
    {
        ConversationTarget.Reset();
    }
}

/// @brief 按下 IA_PushToTalk：IsVoiceEnabled 为 true 时调用 DisableVoice 关麦，否则调用 EnableVoice 开麦
/// EnableVoice 成功时调用 HUD->CloseResidentConversation 关掉打字的交谈面板
void UDeliveryConversationComponent::StartVoiceFromKey()
{
    KeyVoiceToken = {};
    auto* Speech = GetSpeechInput();
    if (!Speech || !Speech->CanUsePushToTalkKey())
        return;
    if (Speech->IsVoiceEnabled())
    {
        Speech->DisableVoice();
        return;
    }
    const auto* PC = Cast<APlayerController>(GetOwner());
    auto* HUD = PC ? Cast<ADeliveryHUD>(PC->GetHUD()) : nullptr;

    // HasSpeechStatusWidget 为 false（屏幕上没有显示麦克风状态的控件）时不开麦，免得玩家不知道麦克风开着
    if (!HUD || !HUD->HasSpeechStatusWidget())
        return;
    if (Speech->EnableVoice())
        HUD->CloseResidentConversation();
}

/// @brief 松开 IA_PushToTalk：不做任何事
void UDeliveryConversationComponent::FinishVoiceFromKey()
{
// IA_PushToTalk 按一下切换麦克风开关，松开时不结束正在识别的那句话
}

/// @brief IA_PushToTalk 的按键被系统取消（例如切换窗口）：不做任何事
void UDeliveryConversationComponent::CancelVoiceKeyHold()
{
// 麦克风打开后一直识别下去，按键被取消不影响正在识别的句子
}

/// @brief 主动取消正在说的这句：调用 Speech->CancelHold 停止识别；LocalVoiceToken 有效时调用 ServerCancelVoice 和 ResetLocalVoice
void UDeliveryConversationComponent::CancelVoiceFromKey()
{
    KeyVoiceToken = {};
    if (auto* Speech = GetSpeechInput())
        Speech->CancelHold();
    if (LocalVoiceToken.IsValid())
    {
        ServerCancelVoice(LocalVoiceToken.Id, LocalVoiceSequence);
        ResetLocalVoice();
    }
}

/// @brief LastLocalInputSequence 加一并返回，打字和开麦共用；同时清掉 LastVoiceInputError（上一次的开麦错误提示）
/// 序号只增不减，服务器据此丢掉晚到的旧请求；溢出回到 0 时再加一次，因为 0 表示无效
uint64 UDeliveryConversationComponent::NextInputSequence()
{
    LastVoiceInputError = NAME_None;
    VoiceErrorExpiresAt = 0;
    ++LastLocalInputSequence;
    if (!LastLocalInputSequence)
        ++LastLocalInputSequence;
    return LastLocalInputSequence;
}

/// @brief 清空本机这句开麦的全部状态（LocalVoiceToken、LocalVoiceHandle、FinalVoiceText 等），并关掉 Tick
void UDeliveryConversationComponent::ResetLocalVoice()
{
    LocalVoiceToken = {};
    LocalVoiceSequence = 0;
    LocalVoiceHandle = {};
    LocalVoiceTarget.Reset();
    KeyVoiceToken = {};
    LocalVoiceContext.Reset();
    LocalVoiceContextPendingSince = -1;
    LocalVoiceDeadline = 0;
    FinalVoiceText.Reset();
    bVoiceTextReady = false;
    bVoiceSubmissionSent = false;
    bVoiceReleased = false;
    SetComponentTickEnabled(false);
}

/// @brief 语音识别发现玩家开始说新的一句时调用：记下 Token 和 Resident，调用 ServerBeginVoice
/// LocalVoiceToken 还有效（上一句没结束）时先调用 ServerCancelVoice；LocalVoiceDeadline 设为 RecordingTimeoutSeconds 后，并打开 Tick
void UDeliveryConversationComponent::HandleVoiceStarted(const FDeliverySpeechTurnToken& Token, ADeliveryNpcCharacter* Resident)
{
    if (bEndingPlay || !Token.IsValid() || !IsValid(Resident))
        return;
    if (LocalVoiceToken.IsValid())
        ServerCancelVoice(LocalVoiceToken.Id, LocalVoiceSequence);
    ResetLocalVoice();
    LocalVoiceToken = Token;
    LocalVoiceTarget = Resident;
    LocalVoiceSequence = NextInputSequence();

    // LocalVoiceContext.Capture 记下玩家此刻的身份（控制器、角色、PlayerState），SubmitReadyVoiceText 和 TickComponent 都用它检查
    if (!LocalVoiceContext.Capture(Cast<APlayerController>(GetOwner())))
    {
        RejectLocalVoice(TEXT("Unavailable"), false);
        return;
    }
    LocalVoiceDeadline = FPlatformTime::Seconds() + UDeliveryConversationTurnSubsystem::RecordingTimeoutSeconds;
    LastVoiceInputError = NAME_None;
    VoiceErrorExpiresAt = 0;
    SetComponentTickEnabled(true);
    ServerBeginVoice(Resident, Token.Id, LocalVoiceSequence);
}

/// @brief 语音识别判断这句话说完时调用：设置 bVoiceReleased，LocalVoiceDeadline 改成 FinalTextTimeoutSeconds 后，再调用 ServerFinishVoice
void UDeliveryConversationComponent::HandleVoiceReleased(const FDeliverySpeechTurnToken& Token)
{
    if (Token == LocalVoiceToken && !bVoiceReleased)
    {
        bVoiceReleased = true;
        LocalVoiceDeadline = FPlatformTime::Seconds() + UDeliveryConversationTurnSubsystem::FinalTextTimeoutSeconds;
        ServerFinishVoice(Token.Id, LocalVoiceSequence);
    }
}

/// @brief 语音识别取消这句（例如太短或全是杂音）时调用：调用 ServerCancelVoice，再调用 ResetLocalVoice
void UDeliveryConversationComponent::HandleVoiceCancelled(const FDeliverySpeechTurnToken& Token)
{
    if (!(Token == LocalVoiceToken))
        return;
    ServerCancelVoice(Token.Id, LocalVoiceSequence);
    ResetLocalVoice();
}

/// @brief 语音识别给出这句的最终文字时调用：去掉首尾空格后为空或超过 256 字就调用 RejectLocalVoice
/// 否则存进 FinalVoiceText，设置 bVoiceTextReady，再调用 SubmitReadyVoiceText
void UDeliveryConversationComponent::HandleVoiceText(const FDeliverySpeechTurnToken& Token, ADeliveryNpcCharacter* Resident,
                                                     const FString& Text)
{
    if (!(Token == LocalVoiceToken) || Resident != LocalVoiceTarget.Get() || bVoiceTextReady)
        return;
    const FString Message = Text.TrimStartAndEnd();
    if (Message.IsEmpty() || Message.Len() > 256)
    {
        RejectLocalVoice(TEXT("Empty"), true);
        return;
    }
    FinalVoiceText = Message;
    bVoiceTextReady = true;
    SubmitReadyVoiceText();
}

/// @brief bVoiceTextReady 为 true 且 LocalVoiceHandle 有效时调用 ServerSubmitVoice；还缺一样就直接返回，等 HandleVoiceText 或 ClientVoiceGranted 再调用
/// 发送前检查 LocalVoiceDeadline 和 LocalVoiceContext；身份暂时是 Pending（例如正在上下车）时最多等 TransitionGraceSeconds
void UDeliveryConversationComponent::SubmitReadyVoiceText()
{
    if (!bVoiceTextReady || bVoiceSubmissionSent || !LocalVoiceHandle.IsValid() || !LocalVoiceTarget.IsValid())
        return;
    const double Now = FPlatformTime::Seconds();
    if (Now >= LocalVoiceDeadline)
    {
        RejectLocalVoice(TEXT("Expired"), true);
        return;
    }

    // 从 LocalVoiceContextPendingSince 起已经等过 TransitionGraceSeconds，调用 RejectLocalVoice
    if (LocalVoiceContextPendingSince >= 0 &&
        Now - LocalVoiceContextPendingSince >= FDeliverySpeechPlayerContext::TransitionGraceSeconds)
    {
        RejectLocalVoice(TEXT("Unavailable"), true);
        return;
    }
    const auto ContextState = LocalVoiceContext.Check(Cast<APlayerController>(GetOwner()));

    // Check 返回 Pending：记下 LocalVoiceContextPendingSince，直接返回，下一帧 TickComponent 再调用这里
    if (ContextState == EDeliverySpeechPlayerContextState::Pending)
    {
        if (LocalVoiceContextPendingSince < 0)
            LocalVoiceContextPendingSince = Now;
        return;
    }
    if (ContextState == EDeliverySpeechPlayerContextState::Invalid)
    {
        RejectLocalVoice(TEXT("Unavailable"), true);
        return;
    }
    LocalVoiceContextPendingSince = -1;

    // 先设置 bVoiceSubmissionSent，再调用 OnSubmitted.Broadcast，让交谈面板显示玩家原话
    // 保留 LocalVoiceHandle，直到 ClientResult 收到这句的送达结果才调用 ResetLocalVoice
    const auto Handle = LocalVoiceHandle;
    const auto Token = LocalVoiceToken;
    const uint64 Sequence = LocalVoiceSequence;
    auto* Resident = LocalVoiceTarget.Get();
    const FString Message = MoveTemp(FinalVoiceText);
    bVoiceSubmissionSent = true;
    LastSubmissionId = Handle.Token;
    LastResult = TEXT("Sending");
    ResultExpiresAt = FPlatformTime::Seconds() + 2.5;
    NextLocalSendAt = FPlatformTime::Seconds() + 1.1;
    OnSubmitted.Broadcast(Resident, LastSubmissionId, Message);

    // OnSubmitted.Broadcast 之后再比较 LastLocalInputSequence 和 Sequence；面板回调里已经开始了新输入，就调用 ServerCancelVoice 取消这句
    if (LastLocalInputSequence == Sequence && LocalVoiceToken == Token && bVoiceSubmissionSent)
        ServerSubmitVoice(Resident, Message, Handle);
    else
        ServerCancelVoice(Token.Id, Sequence);
}

/// @brief LocalVoiceToken 有效时每帧执行：超过 LocalVoiceDeadline、身份 Invalid 或 Pending 太久、麦克风关了、CanVoiceTalkTo 不通过，都调用 RejectLocalVoice
/// 都没问题就调用 SubmitReadyVoiceText
void UDeliveryConversationComponent::TickComponent(float DeltaTime, ELevelTick TickType,
                                                   FActorComponentTickFunction* ThisTickFunction)
{
    Super::TickComponent(DeltaTime, TickType, ThisTickFunction);
    if (bEndingPlay || !LocalVoiceToken.IsValid())
    {
        SetComponentTickEnabled(false);
        return;
    }
    const auto* PC = Cast<APlayerController>(GetOwner());
    const auto* Speech = GetSpeechInput();
    const double Now = FPlatformTime::Seconds();
    const auto ContextState = LocalVoiceContext.Check(PC);
    if (Now >= LocalVoiceDeadline)
    {
        RejectLocalVoice(TEXT("Expired"), true);
        return;
    }
    if (LocalVoiceContextPendingSince >= 0 &&
        Now - LocalVoiceContextPendingSince >= FDeliverySpeechPlayerContext::TransitionGraceSeconds)
    {
        RejectLocalVoice(TEXT("Unavailable"), true);
        return;
    }
    if (!Speech || !Speech->IsVoiceEnabled() || ContextState == EDeliverySpeechPlayerContextState::Invalid)
    {
        RejectLocalVoice(TEXT("Unavailable"), true);
        return;
    }
    if (ContextState == EDeliverySpeechPlayerContextState::Pending)
    {
        if (LocalVoiceContextPendingSince < 0)
            LocalVoiceContextPendingSince = Now;
        return;
    }
    LocalVoiceContextPendingSince = -1;
    if (!CanVoiceTalkTo(LocalVoiceTarget.Get()))
    {
        RejectLocalVoice(TEXT("Unavailable"), true);
        return;
    }
    SubmitReadyVoiceText();
}

/// @brief 本机放弃这句开麦：调用 ResetLocalVoice，bNotifyServer 为 true 时调用 ServerCancelVoice
/// 再调用 OnVoiceInputError.Broadcast 给界面一条错误提示，VoiceErrorExpiresAt 设为 4 秒后
/// @param Reason 显示给玩家的原因，例如 Expired、Unavailable、Busy
void UDeliveryConversationComponent::RejectLocalVoice(FName Reason, bool bNotifyServer)
{
    if (!LocalVoiceToken.IsValid())
        return;
    const auto Token = LocalVoiceToken;
    const uint64 Sequence = LocalVoiceSequence;
    ResetLocalVoice();
    if (bNotifyServer)
        ServerCancelVoice(Token.Id, Sequence);

    // 语音识别交出文字后已经清掉了它自己的 Token，所以这里调用 Speech->ReportTurnRejected 告诉它这句失败了，让它显示提示
    if (auto* Speech = GetSpeechInput())
        Speech->ReportTurnRejected(Token, Reason);

    // LastLocalInputSequence 已经不等于这句的 Sequence（玩家开始了新输入），不调用 OnVoiceInputError.Broadcast
    if (bEndingPlay || LastLocalInputSequence != Sequence)
        return;
    LastVoiceInputError = Reason;
    VoiceErrorExpiresAt = FPlatformTime::Seconds() + 4;
    OnVoiceInputError.Broadcast(Reason);
}

/// @brief 服务器：ServerVoiceHandle 等于 ExpectedHandle 时清空 ServerVoiceClientToken、ServerVoiceHandle、ServerVoiceTarget；不相等说明已经换了新的一句，不清
void UDeliveryConversationComponent::ResetServerVoice(const FDeliveryConversationTurnHandle& ExpectedHandle)
{
    if (!(ServerVoiceHandle == ExpectedHandle))
        return;
    if (DeferredVoiceHandle == ExpectedHandle)
        ClearDeferredVoiceSubmission();
    ServerVoiceClientToken.Invalidate();
    ServerVoiceHandle = {};
    ServerVoiceTarget.Reset();
}

/// @brief 服务器：清掉 VoiceSubmitTimer 和暂存的 DeferredVoiceHandle、DeferredVoiceTarget、DeferredVoiceText
void UDeliveryConversationComponent::ClearDeferredVoiceSubmission()
{
    if (GetWorld())
        GetWorld()->GetTimerManager().ClearTimer(VoiceSubmitTimer);
    DeferredVoiceHandle = {};
    DeferredVoiceTarget.Reset();
    DeferredVoiceText.Reset();
}

/// @brief 服务器：VoiceSubmitTimer 到时调用，取出暂存的文字再调用一次 ServerSubmitVoice_Implementation；DeferredVoiceHandle 已经不等于 ServerVoiceHandle 就丢掉
void UDeliveryConversationComponent::FlushDeferredVoiceSubmission()
{
    const auto Handle = DeferredVoiceHandle;
    const auto Target = DeferredVoiceTarget;
    const FString Text = MoveTemp(DeferredVoiceText);
    ClearDeferredVoiceSubmission();
    if (!bEndingPlay && Handle.IsValid() && Handle == ServerVoiceHandle && Target.IsValid())
        ServerSubmitVoice_Implementation(Target.Get(), Text, Handle);
}

/// @brief 服务器：玩家开麦说新的一句，CanVoiceTalkTo 通过后调用 TryBeginVoice 登记这一轮
/// 同意时调用 ClientVoiceGranted 发回 Handle，拒绝时调用 ClientVoiceRejected 发回原因
void UDeliveryConversationComponent::ServerBeginVoice_Implementation(ADeliveryNpcCharacter* Resident, FGuid ClientToken,
                                                                     uint64 InputSequence, uint64 FacingSequence,
                                                                     APawn* FacingPawn)
{
    auto* PC = Cast<APlayerController>(GetOwner());
    auto* Turns = GetWorld()->GetSubsystem<UDeliveryConversationTurnSubsystem>();
    if (!PC || !Turns || !CanVoiceTalkTo(Resident))
    {
        ClientVoiceRejected(ClientToken, InputSequence, TEXT("Unavailable"));
        return;
    }
    PruneVoiceTurns();

    // VoiceTurns 已有 MaxVoiceTurns 条时返回 Busy；ClientToken 和 InputSequence 都等于当前这一轮（网络重发的同一句）不受限制
    if (VoiceTurns.Num() >= MaxVoiceTurns &&
        (ClientToken != ServerVoiceClientToken || InputSequence != ServerVoiceHandle.InputSequence))
    {
        ClientVoiceRejected(ClientToken, InputSequence, TEXT("Busy"));
        return;
    }
    FDeliveryConversationTurnHandle Handle;
    TArray<FDeliveryConversationTurnLease> Replaced;
    const auto Result = Turns->TryBeginVoice(PC, Resident, ClientToken, InputSequence, Handle, Replaced);
    if (Result != EDeliveryConversationTurnResult::Accepted && Result != EDeliveryConversationTurnResult::AlreadyActive)
    {
        ClientVoiceRejected(ClientToken, InputSequence,
                            Result == EDeliveryConversationTurnResult::Busy ? TEXT("Busy") : TEXT("Expired"));
        return;
    }

    // Accepted（新的一轮）：依次调用 CancelReplacedLeases、CancelNpcMessageTurns、WatchReplies，再用 GetLease 确认这一轮确实登记上了
    // 然后写 ServerVoiceClientToken、ServerVoiceHandle、ServerVoiceTarget，并加进 VoiceTurns；AlreadyActive（网络重发）跳过这些，只再调用一次 ClientVoiceGranted
    if (Result == EDeliveryConversationTurnResult::Accepted)
    {
        CancelReplacedLeases(Replaced);
        CancelNpcMessageTurns(Resident);
        WatchReplies(Resident);
        FDeliveryConversationTurnLease Current;
        if (!Turns->GetLease(Resident, Current) || !(Current.Handle == Handle))
        {
            ClientVoiceRejected(ClientToken, InputSequence, TEXT("Expired"));
            return;
        }
        ServerVoiceClientToken = ClientToken;
        ServerVoiceHandle = Handle;
        ServerVoiceTarget = Resident;
        VoiceTurns.Add(Handle.Token, FVoiceTurn{Handle, Resident});
    }
    ClientVoiceGranted(ClientToken, InputSequence, Handle);
}

/// @brief 服务器：玩家说完这句，调用 FinishRecording，Phase 改成 FinalPending，Deadline 改成 5 秒后
void UDeliveryConversationComponent::ServerFinishVoice_Implementation(FGuid ClientToken, uint64 InputSequence)
{
    if (ServerVoiceClientToken != ClientToken || ServerVoiceHandle.InputSequence != InputSequence)
        return;
    const auto Handle = ServerVoiceHandle;
    const auto Target = ServerVoiceTarget;
    auto* Turns = GetWorld()->GetSubsystem<UDeliveryConversationTurnSubsystem>();

    // GetVoiceContextState 返回 Pending（例如正在上下车，PlayerState 还没同步过来）时也调用 FinishRecording，不取消这句
    if (Turns &&
        Turns->GetVoiceContextState(Cast<APlayerController>(GetOwner()), Handle) == EDeliverySpeechPlayerContextState::Pending &&
        Target.IsValid() && !Target->IsKnockedDown() && IsVoiceTurn(Handle))
    {
        Turns->FinishRecording(Cast<APlayerController>(GetOwner()), Handle);
        return;
    }
    if (!Turns || !CanVoiceTalkTo(Target.Get()) ||
        (Turns->FinishRecording(Cast<APlayerController>(GetOwner()), Handle) != EDeliveryConversationTurnResult::Accepted))
    {
        // FinishRecording 没返回 Accepted：GetLease 取不到这一轮，调用 ClientVoiceRejected 发回 Expired
        // 这一轮还在但 CanVoiceTalkTo 不通过（例如玩家晕倒），调用 Turns->Cancel 取消，发回 Unavailable；其余情况是同一句重复说完，Deadline 不变，不处理
        FDeliveryConversationTurnLease Lease;
        if (!Turns || !Turns->GetLease(Target.Get(), Lease) || !(Lease.Handle == Handle))
            ClientVoiceRejected(ClientToken, InputSequence, TEXT("Expired"));
        else if (!CanVoiceTalkTo(Target.Get()))
        {
            Turns->Cancel(Cast<APlayerController>(GetOwner()), Handle);
            VoiceTurns.Remove(Handle.Token);
            ResetServerVoice(Handle);
            ClientVoiceRejected(ClientToken, InputSequence, TEXT("Unavailable"));
        }
    }
}

/// @brief 服务器：本机取消这句开麦，调用 Turns->Cancel 删掉这一轮；ClientToken 或 InputSequence 和当前这一轮不同的旧请求不处理
void UDeliveryConversationComponent::ServerCancelVoice_Implementation(FGuid ClientToken, uint64 InputSequence)
{
    if (ServerVoiceClientToken != ClientToken || ServerVoiceHandle.InputSequence != InputSequence)
        return;
    const auto Handle = ServerVoiceHandle;
    if (auto* Turns = GetWorld()->GetSubsystem<UDeliveryConversationTurnSubsystem>())
        Turns->Cancel(Cast<APlayerController>(GetOwner()), Handle);
    VoiceTurns.Remove(Handle.Token);
    ResetServerVoice(Handle);
}

/// @brief 服务器：收到开麦这句的文字，检查后调用 DeliverFinalText
/// GetVoiceContextState 返回 Pending 时，文字存进 DeferredVoiceText，0.05 秒后 FlushDeferredVoiceSubmission 再调用这里
/// 距 NextSendAt 不超过 1.1 秒时，也存进 DeferredVoiceText，等到 NextSendAt 再调用；等得更久就发回 TooFast
void UDeliveryConversationComponent::ServerSubmitVoice_Implementation(ADeliveryNpcCharacter* Resident, const FString& Text,
                                                                      FDeliveryConversationTurnHandle Handle)
{
    if (!(Handle == ServerVoiceHandle) || Resident != ServerVoiceTarget.Get())
    {
        return;
    }
    auto* Turns = GetWorld()->GetSubsystem<UDeliveryConversationTurnSubsystem>();
    const auto ContextState = Turns ? Turns->GetVoiceContextState(Cast<APlayerController>(GetOwner()), Handle)
                                    : EDeliverySpeechPlayerContextState::Invalid;
    if (ContextState == EDeliverySpeechPlayerContextState::Pending && IsVoiceTurn(Handle) && IsValid(Resident) &&
        !Resident->IsKnockedDown())
    {
        if (DeferredVoiceHandle == Handle)
            return;
        const FString Message = Text.TrimStartAndEnd();
        if (!Message.IsEmpty() && Message.Len() <= 256)
        {
            // DeferredVoiceHandle 只存一句，已经等于 Handle 就不重复存；等待期间 TurnSubsystem 里这一轮的身份等待和 Deadline 照常计时
            DeferredVoiceHandle = Handle;
            DeferredVoiceTarget = Resident;
            DeferredVoiceText = Message;
            GetWorld()->GetTimerManager().SetTimer(VoiceSubmitTimer, this,
                                                   &UDeliveryConversationComponent::FlushDeferredVoiceSubmission, .05f, false);
            return;
        }
    }

    // IsVoiceTurn 或 CanVoiceTalkTo 不通过：调用 Turns->Cancel，再发回 Unavailable
    if (!IsVoiceTurn(Handle) || !CanVoiceTalkTo(Resident))
    {
        if (Turns)
            Turns->Cancel(Cast<APlayerController>(GetOwner()), Handle);
        VoiceTurns.Remove(Handle.Token);
        ResetServerVoice(Handle);
        ClientResult(TEXT("Unavailable"), Handle.Token);
        return;
    }
    if (DeferredVoiceHandle == Handle)
        return;
    const FString Message = Text.TrimStartAndEnd();
    const double WaitSeconds = NextSendAt - FPlatformTime::Seconds();

    // 玩家连着说了两句短话，当前时间离 NextSendAt 不超过 1.1 秒：ValidateFinal 通过后存进 DeferredVoiceText，VoiceSubmitTimer 到 NextSendAt 时调用 FlushDeferredVoiceSubmission
    if (WaitSeconds > 0 && WaitSeconds <= 1.1 && !Message.IsEmpty() && Message.Len() <= 256 && CanVoiceTalkTo(Resident))
    {
        if (Turns && Turns->ValidateFinal(Cast<APlayerController>(GetOwner()), Resident, Handle) ==
                         EDeliveryConversationTurnResult::Accepted)
        {
            DeferredVoiceHandle = Handle;
            DeferredVoiceTarget = Resident;
            DeferredVoiceText = Message;
            GetWorld()->GetTimerManager().SetTimer(VoiceSubmitTimer, this,
                                                   &UDeliveryConversationComponent::FlushDeferredVoiceSubmission,
                                                   static_cast<float>(WaitSeconds) + .001f, false);
            return;
        }
    }

    // 还没到 NextSendAt 又不能暂存：调用 Turns->Cancel，发回 TooFast；已经到了 NextSendAt 就调用 DeliverFinalText
    if (WaitSeconds > 0 && !Message.IsEmpty())
    {
        if (Turns)
            Turns->Cancel(Cast<APlayerController>(GetOwner()), Handle);
        VoiceTurns.Remove(Handle.Token);
        ClientResult(TEXT("TooFast"), Handle.Token);
    }
    else if (!DeliverFinalText(Resident, Message, Handle))
        VoiceTurns.Remove(Handle.Token);
    ResetServerVoice(Handle);
}

/// @brief 本机：服务器同意开这一轮，Handle 存进 LocalVoiceHandle，再调用 SubmitReadyVoiceText（FinalVoiceText 已经有了就马上提交）
/// ClientToken 或 InputSequence 和本机当前这句不同（本机已经在说下一句了），就调用 ServerCancelVoice
void UDeliveryConversationComponent::ClientVoiceGranted_Implementation(FGuid ClientToken, uint64 InputSequence,
                                                                       FDeliveryConversationTurnHandle Handle)
{
    if (LocalVoiceToken.Id != ClientToken || LocalVoiceSequence != InputSequence || !Handle.IsValid() ||
        Handle.InputSequence != InputSequence)
    {
        ServerCancelVoice(ClientToken, InputSequence);
        return;
    }
    LocalVoiceHandle = Handle;
    SubmitReadyVoiceText();
}

/// @brief 本机：服务器拒绝或取消了这一轮，ClientToken 和 InputSequence 对得上时调用 RejectLocalVoice
void UDeliveryConversationComponent::ClientVoiceRejected_Implementation(FGuid ClientToken, uint64 InputSequence, FName Reason)
{
    if (LocalVoiceToken.Id != ClientToken || LocalVoiceSequence != InputSequence)
        return;
    RejectLocalVoice(Reason, false);
}

// ── 交付 ──
// 交给 NPC 之后，要等它回复结束，OnPlayerReplyEnded 触发时调用 Turns->ReleaseReply 删掉这一轮
// 新的一轮顶掉旧的时调用 CancelReplacedLeases，一轮超时时调用 HandleLeaseExpired，两者都会调用 InterruptPlayerReply 打断对应的回复

/// @brief 服务器：订阅这位 NPC 的 Agent->OnPlayerReplyEnded；它对这个玩家的回复结束时，调用 Turns->ReleaseReply 删掉这一轮，NPC 就能听别人说话
/// WatchedAgents 里已有这位 NPC 就不再订阅
void UDeliveryConversationComponent::WatchReplies(ADeliveryNpcCharacter* Resident)
{
    auto* Agent = IsValid(Resident) ? Resident->Agent.Get() : nullptr;
    if (!Agent || WatchedAgents.Contains(Agent))
        return;
    WatchedAgents.Add(Agent);
    const TWeakObjectPtr<ADeliveryNpcCharacter> WeakNpc = Resident;
    Agent->OnPlayerReplyEnded.AddWeakLambda(this,
                                            [this, WeakNpc](APlayerController* Player, FGuid MessageId, FName Reason)
                                            {
                                                if (Player != GetOwner() || !WeakNpc.IsValid() || !GetWorld())
                                                    return;
                                                if (auto* Turns = GetWorld()->GetSubsystem<UDeliveryConversationTurnSubsystem>())
                                                {
                                                    const auto* Voice = VoiceTurns.Find(MessageId);
                                                    if (Voice && Voice->Npc == WeakNpc &&
                                                        Voice->Handle.WorldEpoch == Turns->GetWorldEpoch())
                                                        VoiceTurns.Remove(MessageId);
                                                    Turns->ReleaseReply(WeakNpc.Get(), MessageId);
                                                }
                                            });
}

/// @brief 服务器：玩家开口时调用 Agent->InvalidateNpcMessageTurns，打断这位 NPC 和其他 NPC 之间的接话
void UDeliveryConversationComponent::CancelNpcMessageTurns(ADeliveryNpcCharacter* Resident)
{
    if (GetOwner()->HasAuthority() && IsValid(Resident) && Resident->GetWorld() == GetWorld() && Resident->Agent)
        Resident->Agent->InvalidateNpcMessageTurns(TEXT("PlayerInputStarted"));
}

/// @brief 服务器：处理被顶掉的旧轮次，先调用 ResetServerVoice 并从 VoiceTurns 删掉
/// Phase 是 AwaitingReply 的调用 InterruptPlayerReply 打断 NPC 的回复；其余的调用 ClientVoiceRejected 发回 Cancelled
void UDeliveryConversationComponent::CancelReplacedLeases(const TArray<FDeliveryConversationTurnLease>& Leases)
{
    auto* PC = Cast<APlayerController>(GetOwner());
    for (const auto& Lease : Leases)
    {
        if (Lease.Owner.Get() != PC)
            continue;
        ResetServerVoice(Lease.Handle);
        VoiceTurns.Remove(Lease.Handle.Token);
        if (Lease.Phase == EDeliveryConversationTurnPhase::AwaitingReply && Lease.Npc.IsValid())
        {
            auto* Agent = Lease.Npc->Agent.Get();
            if (Agent && Agent->GetReplyMessage(PC) == Lease.MessageId)
                Agent->InterruptPlayerReply(PC);
        }
        else
            ClientVoiceRejected(Lease.ClientToken, Lease.Handle.InputSequence, TEXT("Cancelled"));
    }
}

/// @brief 服务器：OnLeaseExpired 触发时调用，这位玩家的某一轮超时或失效
/// Phase 是 AwaitingReply 的调用 InterruptPlayerReply，再调用 ClientResult 发回 ReplyFailed；其余的调用 ClientVoiceRejected 发回 Expired
void UDeliveryConversationComponent::HandleLeaseExpired(const FDeliveryConversationTurnLease& Lease,
                                                        EDeliveryConversationLeaseEnd Reason)
{
    auto* PC = Cast<APlayerController>(GetOwner());
    if (Lease.Owner.Get() != PC)
        return;
    ResetServerVoice(Lease.Handle);
    const auto* Voice = VoiceTurns.Find(Lease.Handle.Token);
    if (Voice && Voice->Handle == Lease.Handle)
        VoiceTurns.Remove(Lease.Handle.Token);
    if (Lease.Phase == EDeliveryConversationTurnPhase::AwaitingReply)
    {
        if (Lease.Npc.IsValid())
        {
            auto* Agent = Lease.Npc->Agent.Get();
            if (Agent && Agent->GetReplyMessage(PC) == Lease.MessageId)
                Agent->InterruptPlayerReply(PC);
        }
        ClientResult(TEXT("ReplyFailed"), Lease.MessageId);
    }
    else
        ClientVoiceRejected(Lease.ClientToken, Lease.Handle.InputSequence, TEXT("Expired"));
}

/// @brief 组件结束：取消开麦、转身和检查器，解掉全部按键绑定和通知，调用 ReleasePlayer 删掉这位玩家的全部轮次
void UDeliveryConversationComponent::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
    bEndingPlay = true;
    ClearDeferredVoiceSubmission();
    ShutdownInspector();
    CancelConversationFacingInput();
    if (LocalVoiceToken.IsValid())
        ServerCancelVoice(LocalVoiceToken.Id, LocalVoiceSequence);
    if (auto* Speech = BoundSpeech.Get())
    {
        Speech->CancelHold();
        Speech->OnTurnStarted.RemoveAll(this);
        Speech->OnHoldReleased.RemoveAll(this);
        Speech->OnTurnCancelled.RemoveAll(this);
        Speech->OnFinalText.RemoveAll(this);
    }
    if (auto* Input = BoundVoiceInput.Get())
        for (uint32 Handle : VoiceInputBindingHandles)
            Input->RemoveBindingByHandle(Handle);
    if (auto* Sub = VoiceInputSubsystem.Get(); Sub && bOwnsVoiceMapping && VoiceMapping.IsValid())
        Sub->RemoveMappingContext(VoiceMapping.Get());
    if (auto* Input = BoundTalkInput.Get(); Input && TalkInputBindingHandle)
        Input->RemoveBindingByHandle(TalkInputBindingHandle);
    if (auto* Turns = BoundTurns.Get())
    {
        Turns->OnLeaseExpired.RemoveAll(this);
        TArray<FDeliveryConversationTurnLease> Removed;
        Turns->ReleasePlayer(Cast<APlayerController>(GetOwner()), Removed);
        CancelReplacedLeases(Removed);
    }
    for (const auto& Weak : WatchedAgents)
        if (auto* Agent = Weak.Get())
            Agent->OnPlayerReplyEnded.RemoveAll(this);
    WatchedAgents.Reset();
    BoundTurns.Reset();
    BoundSpeech.Reset();
    VoiceMapping.Reset();
    VoiceTurns.Reset();
    CachedVoiceCompanion.Reset();
    VoiceInputBindingHandles.Reset();
    BoundVoiceInput.Reset();
    ConversationTarget.Reset();
    ResetLocalVoice();
    VoiceInputSubsystem.Reset();
    bOwnsVoiceMapping = false;
    BoundTalkInput.Reset();
    TalkInputBindingHandle = 0;
    OnSubmitted.Clear();
    OnSubmissionBound.Clear();
    OnVoiceInputError.Clear();
    Super::EndPlay(EndPlayReason);
}
