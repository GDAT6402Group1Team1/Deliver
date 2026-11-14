#include "AI/Conversation/DeliveryConversationComponent.h"
#include "AI/Characters/DeliveryNpcCharacter.h"
#include "AI/Decision/DeliveryNpcAgentComponent.h"
#include "Speech/DeliverySpeechInputComponent.h"
#include "UI/DeliveryHUD.h"
#include "EnhancedInputComponent.h"
#include "EnhancedInputSubsystems.h"
#include "InputAction.h"
#include "InputMappingContext.h"
#include "Engine/LocalPlayer.h"
#include "Engine/World.h"
#include "GameFramework/PlayerController.h"

void UDeliveryConversationComponent::BeginPlay()
{
    Super::BeginPlay();
    bEndingPlay=false;
    BindSpeechInput();
    if (GetOwner()->HasAuthority())
        if (auto* Turns=GetWorld()->GetSubsystem<UDeliveryConversationTurnSubsystem>())
        {
            BoundTurns=Turns;
            Turns->OnLeaseExpired.AddUObject(this,&UDeliveryConversationComponent::HandleLeaseExpired);
        }
}

UDeliverySpeechInputComponent* UDeliveryConversationComponent::GetSpeechInput() const
{
    return GetOwner()?GetOwner()->FindComponentByClass<UDeliverySpeechInputComponent>():nullptr;
}

void UDeliveryConversationComponent::BindSpeechInput()
{
    auto* PC=Cast<APlayerController>(GetOwner());
    if (bEndingPlay || !PC || !PC->IsLocalController()) return;
    auto* Speech=GetSpeechInput();
    if (Speech && BoundSpeech.Get()!=Speech)
    {
        if (auto* Old=BoundSpeech.Get())
        {
            Old->OnTurnStarted.RemoveAll(this); Old->OnHoldReleased.RemoveAll(this);
            Old->OnTurnCancelled.RemoveAll(this); Old->OnFinalText.RemoveAll(this);
        }
        BoundSpeech=Speech;
        Speech->OnTurnStarted.AddUObject(this,&UDeliveryConversationComponent::HandleVoiceStarted);
        Speech->OnHoldReleased.AddUObject(this,&UDeliveryConversationComponent::HandleVoiceReleased);
        Speech->OnTurnCancelled.AddUObject(this,&UDeliveryConversationComponent::HandleVoiceCancelled);
        Speech->OnFinalText.AddUObject(this,&UDeliveryConversationComponent::HandleVoiceText);
    }
    auto* Input=Cast<UEnhancedInputComponent>(PC->InputComponent);
    if (!Input) return;
    auto* Action=LoadObject<UInputAction>(nullptr,TEXT("/Game/UI/Conversation/Input/IA_PushToTalk"));
    auto* Mapping=LoadObject<UInputMappingContext>(nullptr,TEXT("/Game/UI/Conversation/Input/IMC_VoiceConversation"));
    if (!Action || !Mapping)
    {
        UE_LOG(LogTemp,Warning,TEXT("DeliverySpeech: voice toggle input assets are missing"));
        return;
    }
    if (BoundVoiceInput.Get()!=Input || VoiceInputBindingHandles.Num()!=1)
    {
        if (auto* Old=BoundVoiceInput.Get())
            for (uint32 Handle:VoiceInputBindingHandles) Old->RemoveBindingByHandle(Handle);
        VoiceInputBindingHandles.Reset(); BoundVoiceInput=Input;
        VoiceInputBindingHandles.Add(Input->BindAction(Action,ETriggerEvent::Started,this,&UDeliveryConversationComponent::StartVoiceFromKey).GetHandle());
    }
    if (auto* Sub=ULocalPlayer::GetSubsystem<UEnhancedInputLocalPlayerSubsystem>(PC->GetLocalPlayer()))
    {
        if (VoiceInputSubsystem.Get()!=Sub || VoiceMapping.Get()!=Mapping)
        {
            if (auto* Old=VoiceInputSubsystem.Get(); Old && bOwnsVoiceMapping && VoiceMapping.IsValid()) Old->RemoveMappingContext(VoiceMapping.Get());
            VoiceInputSubsystem=Sub; VoiceMapping=Mapping; bOwnsVoiceMapping=false;
        }
        if (!Sub->HasMappingContext(Mapping)) { Sub->AddMappingContext(Mapping,2); bOwnsVoiceMapping=true; }
    }
}

void UDeliveryConversationComponent::SetConversationTarget(ADeliveryNpcCharacter* Resident)
{
    if (IsValid(Resident) && Resident->GetWorld()==GetWorld())
    {
        ConversationTarget=Resident;
    }
}
void UDeliveryConversationComponent::ClearConversationTarget(ADeliveryNpcCharacter* Resident)
{
    if (ConversationTarget.Get()==Resident)
    {
        ConversationTarget.Reset();
    }
}
void UDeliveryConversationComponent::StartVoiceFromKey()
{
    KeyVoiceToken={};
    auto* Speech=GetSpeechInput();
    if (!Speech || !Speech->CanUsePushToTalkKey()) return;
    if (Speech->IsVoiceEnabled()) { Speech->DisableVoice(); return; }
    const auto* PC=Cast<APlayerController>(GetOwner());
    auto* HUD=PC?Cast<ADeliveryHUD>(PC->GetHUD()):nullptr;
    if (!HUD || !HUD->HasSpeechStatusWidget()) return;
    if (Speech->EnableVoice()) HUD->CloseResidentConversation();
}
void UDeliveryConversationComponent::FinishVoiceFromKey()
{
    // V 只在按下时切换连麦，松键不会结束自动录入的句子
}
void UDeliveryConversationComponent::CancelVoiceKeyHold()
{
    // 输入映射释放不能取消正在进行的连续语音
}
void UDeliveryConversationComponent::CancelVoiceFromKey()
{
    KeyVoiceToken={};
    if (auto* Speech=GetSpeechInput()) Speech->CancelHold();
    if (LocalVoiceToken.IsValid())
    {
        ServerCancelVoice(LocalVoiceToken.Id,LocalVoiceSequence);
        ResetLocalVoice();
    }
}

uint64 UDeliveryConversationComponent::NextInputSequence()
{
    LastVoiceInputError=NAME_None; VoiceErrorExpiresAt=0;
    ++LastLocalInputSequence;
    if (!LastLocalInputSequence) ++LastLocalInputSequence;
    return LastLocalInputSequence;
}
void UDeliveryConversationComponent::ResetLocalVoice()
{
    LocalVoiceToken={}; LocalVoiceSequence=0; LocalVoiceHandle={}; LocalVoiceTarget.Reset();
    KeyVoiceToken={};
    LocalVoicePawn.Reset(); LocalVoiceDeadline=0;
    FinalVoiceText.Reset(); bVoiceTextReady=false; bVoiceSubmissionSent=false; bVoiceReleased=false;
    SetComponentTickEnabled(false);
}
void UDeliveryConversationComponent::HandleVoiceStarted(const FDeliverySpeechTurnToken& Token,ADeliveryNpcCharacter* Resident)
{
    if (bEndingPlay || !Token.IsValid() || !IsValid(Resident)) return;
    if (LocalVoiceToken.IsValid()) ServerCancelVoice(LocalVoiceToken.Id,LocalVoiceSequence);
    ResetLocalVoice();
    LocalVoiceToken=Token; LocalVoiceTarget=Resident; LocalVoiceSequence=NextInputSequence();
    if (const auto* PC=Cast<APlayerController>(GetOwner())) LocalVoicePawn=PC->GetPawn();
    LocalVoiceDeadline=FPlatformTime::Seconds()+UDeliveryConversationTurnSubsystem::RecordingTimeoutSeconds;
    LastVoiceInputError=NAME_None; VoiceErrorExpiresAt=0;
    SetComponentTickEnabled(true);
    ServerBeginVoice(Resident,Token.Id,LocalVoiceSequence);
}
void UDeliveryConversationComponent::HandleVoiceReleased(const FDeliverySpeechTurnToken& Token)
{
    if (Token==LocalVoiceToken && !bVoiceReleased)
    {
        bVoiceReleased=true;
        LocalVoiceDeadline=FPlatformTime::Seconds()+UDeliveryConversationTurnSubsystem::FinalTextTimeoutSeconds;
        ServerFinishVoice(Token.Id,LocalVoiceSequence);
    }
}
void UDeliveryConversationComponent::HandleVoiceCancelled(const FDeliverySpeechTurnToken& Token)
{
    if (!(Token==LocalVoiceToken)) return;
    ServerCancelVoice(Token.Id,LocalVoiceSequence);
    ResetLocalVoice();
}
void UDeliveryConversationComponent::HandleVoiceText(const FDeliverySpeechTurnToken& Token,ADeliveryNpcCharacter* Resident,const FString& Text)
{
    if (!(Token==LocalVoiceToken) || Resident!=LocalVoiceTarget.Get() || bVoiceTextReady) return;
    const FString Message=Text.TrimStartAndEnd();
    if (Message.IsEmpty() || Message.Len()>256) { RejectLocalVoice(TEXT("Empty"),true); return; }
    FinalVoiceText=Message; bVoiceTextReady=true;
    SubmitReadyVoiceText();
}
void UDeliveryConversationComponent::SubmitReadyVoiceText()
{
    if (!bVoiceTextReady || bVoiceSubmissionSent || !LocalVoiceHandle.IsValid() || !LocalVoiceTarget.IsValid()) return;
    // 广播前标记已发，保留原轮次至接收回执，避免下一句取消服务器正在等限频的 final
    const auto Handle=LocalVoiceHandle;
    const auto Token=LocalVoiceToken;
    const uint64 Sequence=LocalVoiceSequence;
    auto* Resident=LocalVoiceTarget.Get();
    const FString Message=MoveTemp(FinalVoiceText);
    bVoiceSubmissionSent=true;
    LastSubmissionId=Handle.Token; LastResult=TEXT("Sending");
    ResultExpiresAt=FPlatformTime::Seconds()+2.5; NextLocalSendAt=FPlatformTime::Seconds()+1.1;
    OnSubmitted.Broadcast(Resident,LastSubmissionId,Message);
    if (LastLocalInputSequence==Sequence && LocalVoiceToken==Token && bVoiceSubmissionSent)
        ServerSubmitVoice(Resident,Message,Handle);
    else ServerCancelVoice(Token.Id,Sequence);
}

void UDeliveryConversationComponent::TickComponent(float DeltaTime,ELevelTick TickType,FActorComponentTickFunction* ThisTickFunction)
{
    Super::TickComponent(DeltaTime,TickType,ThisTickFunction);
    if (bEndingPlay || !LocalVoiceToken.IsValid()) { SetComponentTickEnabled(false); return; }
    const auto* PC=Cast<APlayerController>(GetOwner());
    const auto* Speech=GetSpeechInput();
    if (FPlatformTime::Seconds()>=LocalVoiceDeadline) RejectLocalVoice(TEXT("Expired"),true);
    else if (!PC || PC->GetPawn()!=LocalVoicePawn.Get() || !CanVoiceTalkTo(LocalVoiceTarget.Get()) || !Speech || !Speech->IsVoiceEnabled())
        RejectLocalVoice(TEXT("Unavailable"),true);
}

void UDeliveryConversationComponent::RejectLocalVoice(FName Reason,bool bNotifyServer)
{
    if (!LocalVoiceToken.IsValid()) return;
    const auto Token=LocalVoiceToken;
    const uint64 Sequence=LocalVoiceSequence;
    ResetLocalVoice();
    if (bNotifyServer) ServerCancelVoice(Token.Id,Sequence);
    // ASR 交出 final 后已清自身 token，交谈层仍负责等待 grant 的失败提示
    if (auto* Speech=GetSpeechInput()) Speech->ReportTurnRejected(Token,Reason);
    if (bEndingPlay || LastLocalInputSequence!=Sequence) return;
    LastVoiceInputError=Reason; VoiceErrorExpiresAt=FPlatformTime::Seconds()+4;
    OnVoiceInputError.Broadcast(Reason);
}

void UDeliveryConversationComponent::ResetServerVoice(const FDeliveryConversationTurnHandle& ExpectedHandle)
{
    if (!(ServerVoiceHandle==ExpectedHandle)) return;
    if (DeferredVoiceHandle==ExpectedHandle) ClearDeferredVoiceSubmission();
    ServerVoiceClientToken.Invalidate(); ServerVoiceHandle={}; ServerVoiceTarget.Reset();
}

void UDeliveryConversationComponent::ClearDeferredVoiceSubmission()
{
    if (GetWorld()) GetWorld()->GetTimerManager().ClearTimer(VoiceSubmitTimer);
    DeferredVoiceHandle={}; DeferredVoiceTarget.Reset(); DeferredVoiceText.Reset();
}

void UDeliveryConversationComponent::FlushDeferredVoiceSubmission()
{
    const auto Handle=DeferredVoiceHandle;
    const auto Target=DeferredVoiceTarget;
    const FString Text=MoveTemp(DeferredVoiceText);
    ClearDeferredVoiceSubmission();
    if (!bEndingPlay && Handle.IsValid() && Handle==ServerVoiceHandle && Target.IsValid())
        ServerSubmitVoice_Implementation(Target.Get(),Text,Handle);
}

void UDeliveryConversationComponent::ServerBeginVoice_Implementation(ADeliveryNpcCharacter* Resident,FGuid ClientToken,uint64 InputSequence,uint64 FacingSequence,APawn* FacingPawn)
{
    auto* PC=Cast<APlayerController>(GetOwner());
    auto* Turns=GetWorld()->GetSubsystem<UDeliveryConversationTurnSubsystem>();
    if (!PC || !Turns || !CanVoiceTalkTo(Resident))
    { ClientVoiceRejected(ClientToken,InputSequence,TEXT("Unavailable")); return; }
    PruneVoiceTurns();
    if (VoiceTurns.Num()>=MaxVoiceTurns &&
        (ClientToken!=ServerVoiceClientToken || InputSequence!=ServerVoiceHandle.InputSequence))
    { ClientVoiceRejected(ClientToken,InputSequence,TEXT("Busy")); return; }
    FDeliveryConversationTurnHandle Handle;
    TArray<FDeliveryConversationTurnLease> Replaced;
    const auto Result=Turns->TryBegin(PC,Resident,ClientToken,InputSequence,Handle,Replaced);
    if (Result!=EDeliveryConversationTurnResult::Accepted && Result!=EDeliveryConversationTurnResult::AlreadyActive)
    { ClientVoiceRejected(ClientToken,InputSequence,Result==EDeliveryConversationTurnResult::Busy?TEXT("Busy"):TEXT("Expired")); return; }
    if (Result==EDeliveryConversationTurnResult::Accepted)
    {
        CancelReplacedLeases(Replaced);
        CancelNpcMessageTurns(Resident);
        WatchReplies(Resident);
        FDeliveryConversationTurnLease Current;
        if (!Turns->GetLease(Resident,Current) || !(Current.Handle==Handle))
        { ClientVoiceRejected(ClientToken,InputSequence,TEXT("Expired")); return; }
        ServerVoiceClientToken=ClientToken; ServerVoiceHandle=Handle; ServerVoiceTarget=Resident;
        VoiceTurns.Add(Handle.Token,FVoiceTurn{Handle,Resident});
    }
    ClientVoiceGranted(ClientToken,InputSequence,Handle);
}
void UDeliveryConversationComponent::ServerFinishVoice_Implementation(FGuid ClientToken,uint64 InputSequence)
{
    if (ServerVoiceClientToken!=ClientToken || ServerVoiceHandle.InputSequence!=InputSequence) return;
    const auto Handle=ServerVoiceHandle;
    const auto Target=ServerVoiceTarget;
    auto* Turns=GetWorld()->GetSubsystem<UDeliveryConversationTurnSubsystem>();
    if (!Turns || !CanVoiceTalkTo(Target.Get()) ||
        (Turns->FinishRecording(Cast<APlayerController>(GetOwner()),Handle)!=EDeliveryConversationTurnResult::Accepted))
    {
        // 重复松键已在协调层保持原期限，只有占用失效才通知本机停止
        FDeliveryConversationTurnLease Lease;
        if (!Turns || !Turns->GetLease(Target.Get(),Lease) || !(Lease.Handle==Handle))
            ClientVoiceRejected(ClientToken,InputSequence,TEXT("Expired"));
        else if (!CanVoiceTalkTo(Target.Get()))
        {
            Turns->Cancel(Cast<APlayerController>(GetOwner()),Handle);
            VoiceTurns.Remove(Handle.Token);
            ResetServerVoice(Handle);
            ClientVoiceRejected(ClientToken,InputSequence,TEXT("Unavailable"));
        }
    }
}
void UDeliveryConversationComponent::ServerCancelVoice_Implementation(FGuid ClientToken,uint64 InputSequence)
{
    if (ServerVoiceClientToken!=ClientToken || ServerVoiceHandle.InputSequence!=InputSequence) return;
    const auto Handle=ServerVoiceHandle;
    if (auto* Turns=GetWorld()->GetSubsystem<UDeliveryConversationTurnSubsystem>())
        Turns->Cancel(Cast<APlayerController>(GetOwner()),Handle);
    VoiceTurns.Remove(Handle.Token);
    ResetServerVoice(Handle);
}
void UDeliveryConversationComponent::ServerSubmitVoice_Implementation(ADeliveryNpcCharacter* Resident,const FString& Text,FDeliveryConversationTurnHandle Handle)
{
    if (!(Handle==ServerVoiceHandle) || Resident!=ServerVoiceTarget.Get())
    { return; }
    if (!IsVoiceTurn(Handle) || !CanVoiceTalkTo(Resident))
    {
        if (auto* Turns=GetWorld()->GetSubsystem<UDeliveryConversationTurnSubsystem>())
            Turns->Cancel(Cast<APlayerController>(GetOwner()),Handle);
        VoiceTurns.Remove(Handle.Token);
        ResetServerVoice(Handle);
        ClientResult(TEXT("Unavailable"),Handle.Token);
        return;
    }
    if (DeferredVoiceHandle==Handle) return;
    const FString Message=Text.TrimStartAndEnd();
    const double WaitSeconds=NextSendAt-FPlatformTime::Seconds();
    // 短答保留同一份服务器轮次，最多等原有的一秒间隔，不丢弃或无限排队
    if (WaitSeconds>0 && WaitSeconds<=1.1 && !Message.IsEmpty() && Message.Len()<=256 && CanVoiceTalkTo(Resident))
    {
        auto* Turns=GetWorld()->GetSubsystem<UDeliveryConversationTurnSubsystem>();
        if (Turns && Turns->ValidateFinal(Cast<APlayerController>(GetOwner()),Resident,Handle)==EDeliveryConversationTurnResult::Accepted)
        {
            DeferredVoiceHandle=Handle; DeferredVoiceTarget=Resident; DeferredVoiceText=Message;
            GetWorld()->GetTimerManager().SetTimer(VoiceSubmitTimer,this,
                &UDeliveryConversationComponent::FlushDeferredVoiceSubmission,static_cast<float>(WaitSeconds)+.001f,false);
            return;
        }
    }
    if (WaitSeconds>0 && !Message.IsEmpty())
    {
        if (auto* Turns=GetWorld()->GetSubsystem<UDeliveryConversationTurnSubsystem>())
            Turns->Cancel(Cast<APlayerController>(GetOwner()),Handle);
        VoiceTurns.Remove(Handle.Token);
        ClientResult(TEXT("TooFast"),Handle.Token);
    }
    else if (!DeliverFinalText(Resident,Message,Handle)) VoiceTurns.Remove(Handle.Token);
    ResetServerVoice(Handle);
}
void UDeliveryConversationComponent::ClientVoiceGranted_Implementation(FGuid ClientToken,uint64 InputSequence,FDeliveryConversationTurnHandle Handle)
{
    if (LocalVoiceToken.Id!=ClientToken || LocalVoiceSequence!=InputSequence || !Handle.IsValid() || Handle.InputSequence!=InputSequence)
    { ServerCancelVoice(ClientToken,InputSequence); return; }
    LocalVoiceHandle=Handle;
    SubmitReadyVoiceText();
}
void UDeliveryConversationComponent::ClientVoiceRejected_Implementation(FGuid ClientToken,uint64 InputSequence,FName Reason)
{
    if (LocalVoiceToken.Id!=ClientToken || LocalVoiceSequence!=InputSequence) return;
    RejectLocalVoice(Reason,false);
}

void UDeliveryConversationComponent::WatchReplies(ADeliveryNpcCharacter* Resident)
{
    auto* Agent=IsValid(Resident)?Resident->Agent.Get():nullptr;
    if (!Agent || WatchedAgents.Contains(Agent)) return;
    WatchedAgents.Add(Agent);
    const TWeakObjectPtr<ADeliveryNpcCharacter> WeakNpc=Resident;
    Agent->OnPlayerReplyEnded.AddWeakLambda(this,[this,WeakNpc](APlayerController* Player,FGuid MessageId,FName Reason)
    {
        if (Player!=GetOwner() || !WeakNpc.IsValid() || !GetWorld()) return;
        if (auto* Turns=GetWorld()->GetSubsystem<UDeliveryConversationTurnSubsystem>())
        {
            const auto* Voice=VoiceTurns.Find(MessageId);
            if (Voice && Voice->Npc==WeakNpc && Voice->Handle.WorldEpoch==Turns->GetWorldEpoch())
                VoiceTurns.Remove(MessageId);
            Turns->ReleaseReply(WeakNpc.Get(),MessageId);
        }
    });
}
void UDeliveryConversationComponent::CancelNpcMessageTurns(ADeliveryNpcCharacter* Resident)
{
    if (GetOwner()->HasAuthority() && IsValid(Resident) && Resident->GetWorld()==GetWorld() && Resident->Agent)
        Resident->Agent->InvalidateNpcMessageTurns(TEXT("PlayerInputStarted"));
}

void UDeliveryConversationComponent::CancelReplacedLeases(const TArray<FDeliveryConversationTurnLease>& Leases)
{
    auto* PC=Cast<APlayerController>(GetOwner());
    for (const auto& Lease:Leases)
    {
        if (Lease.Owner.Get()!=PC) continue;
        ResetServerVoice(Lease.Handle);
        VoiceTurns.Remove(Lease.Handle.Token);
        if (Lease.Phase==EDeliveryConversationTurnPhase::AwaitingReply && Lease.Npc.IsValid())
        {
            auto* Agent=Lease.Npc->Agent.Get();
            if (Agent && Agent->GetReplyMessage(PC)==Lease.MessageId) Agent->InterruptPlayerReply(PC);
        }
        else ClientVoiceRejected(Lease.ClientToken,Lease.Handle.InputSequence,TEXT("Cancelled"));
    }
}
void UDeliveryConversationComponent::HandleLeaseExpired(const FDeliveryConversationTurnLease& Lease,EDeliveryConversationLeaseEnd Reason)
{
    auto* PC=Cast<APlayerController>(GetOwner());
    if (Lease.Owner.Get()!=PC) return;
    ResetServerVoice(Lease.Handle);
    const auto* Voice=VoiceTurns.Find(Lease.Handle.Token);
    if (Voice && Voice->Handle==Lease.Handle) VoiceTurns.Remove(Lease.Handle.Token);
    if (Lease.Phase==EDeliveryConversationTurnPhase::AwaitingReply)
    {
        if (Lease.Npc.IsValid())
        {
            auto* Agent=Lease.Npc->Agent.Get();
            if (Agent && Agent->GetReplyMessage(PC)==Lease.MessageId) Agent->InterruptPlayerReply(PC);
        }
        ClientResult(TEXT("ReplyFailed"),Lease.MessageId);
    }
    else ClientVoiceRejected(Lease.ClientToken,Lease.Handle.InputSequence,TEXT("Expired"));
}
void UDeliveryConversationComponent::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
    bEndingPlay=true;
    ClearDeferredVoiceSubmission();
    ShutdownInspector();
    CancelConversationFacingInput();
    if (LocalVoiceToken.IsValid()) ServerCancelVoice(LocalVoiceToken.Id,LocalVoiceSequence);
    if (auto* Speech=BoundSpeech.Get())
    {
        Speech->CancelHold();
        Speech->OnTurnStarted.RemoveAll(this); Speech->OnHoldReleased.RemoveAll(this);
        Speech->OnTurnCancelled.RemoveAll(this); Speech->OnFinalText.RemoveAll(this);
    }
    if (auto* Input=BoundVoiceInput.Get())
        for (uint32 Handle:VoiceInputBindingHandles) Input->RemoveBindingByHandle(Handle);
    if (auto* Sub=VoiceInputSubsystem.Get(); Sub && bOwnsVoiceMapping && VoiceMapping.IsValid()) Sub->RemoveMappingContext(VoiceMapping.Get());
    if (auto* Input=BoundTalkInput.Get(); Input && TalkInputBindingHandle) Input->RemoveBindingByHandle(TalkInputBindingHandle);
    if (auto* Turns=BoundTurns.Get())
    {
        Turns->OnLeaseExpired.RemoveAll(this);
        TArray<FDeliveryConversationTurnLease> Removed;
        Turns->ReleasePlayer(Cast<APlayerController>(GetOwner()),Removed);
        CancelReplacedLeases(Removed);
    }
    for (const auto& Weak:WatchedAgents)
        if (auto* Agent=Weak.Get()) Agent->OnPlayerReplyEnded.RemoveAll(this);
    WatchedAgents.Reset(); BoundTurns.Reset(); BoundSpeech.Reset(); VoiceMapping.Reset();
    VoiceTurns.Reset(); CachedVoiceCompanion.Reset();
    VoiceInputBindingHandles.Reset(); BoundVoiceInput.Reset(); ConversationTarget.Reset(); ResetLocalVoice();
    VoiceInputSubsystem.Reset(); bOwnsVoiceMapping=false; BoundTalkInput.Reset(); TalkInputBindingHandle=0;
    OnSubmitted.Clear(); OnSubmissionBound.Clear(); OnVoiceInputError.Clear();
    Super::EndPlay(EndPlayReason);
}
