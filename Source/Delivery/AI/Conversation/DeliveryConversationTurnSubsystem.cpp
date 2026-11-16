#include "AI/Conversation/DeliveryConversationTurnSubsystem.h"

#include "AI/Characters/DeliveryNpcCharacter.h"
#include "Engine/World.h"
#include "GameFramework/Pawn.h"
#include "GameFramework/PlayerController.h"
#include "HAL/PlatformTime.h"

// ── 生命周期 ──
// 每个服务器世界一份；Initialize 生成 WorldEpoch（世界编号），Deinitialize 清空全部记录
// 发出去的 FDeliveryConversationTurnHandle 都带着 WorldEpoch，旧世界的 Handle 和新的 WorldEpoch 对不上，会被拒绝

/// @brief 只在正式游戏和编辑器试玩（PIE）的世界里创建
bool UDeliveryConversationTurnSubsystem::DoesSupportWorldType(EWorldType::Type WorldType) const
{
    return WorldType == EWorldType::Game || WorldType == EWorldType::PIE;
}

/// @brief 只在服务器或单机世界创建；客户端不判断谁占着 NPC
bool UDeliveryConversationTurnSubsystem::ShouldCreateSubsystem(UObject* Outer) const
{
    const auto* World = Cast<UWorld>(Outer);
    return World && World->IsGameWorld() && World->GetNetMode() != NM_Client && Super::ShouldCreateSubsystem(Outer);
}

/// @brief 生成 WorldEpoch（这个世界的编号），之后生成的 Handle 都带着它
void UDeliveryConversationTurnSubsystem::Initialize(FSubsystemCollectionBase& Collection)
{
    Super::Initialize(Collection);
    WorldEpoch = FGuid::NewGuid();
    bDeinitializing = false;
}

/// @brief 世界结束时清空全部轮次和接话记录，并解除所有通知
void UDeliveryConversationTurnSubsystem::Deinitialize()
{
    bDeinitializing = true;
    WorldEpoch.Invalidate();
    Leases.Reset();
    Owners.Reset();
    OnLeaseExpired.Clear();
    NpcReplies.Reset();
    OnNpcMessagesInvalidated.Clear();
    Super::Deinitialize();
}

/// @brief 判断这个世界由这台机器的服务器管理，而且还没开始结束
bool UDeliveryConversationTurnSubsystem::IsServerWorld() const
{
    const UWorld* World = GetWorld();
    return !bDeinitializing && WorldEpoch.IsValid() && World && World->IsGameWorld() && World->GetNetMode() != NM_Client;
}

/// @brief 判断开口的玩家还有效：控制器在这个世界里，并且正控制着一个角色
bool UDeliveryConversationTurnSubsystem::IsValidOwner(const APlayerController* Owner) const
{
    if (!IsServerWorld() || !IsValid(Owner) || Owner->IsActorBeingDestroyed() || Owner->GetWorld() != GetWorld() ||
        !Owner->HasAuthority())
        return false;
    const APawn* Pawn = Owner->GetPawn();
    return IsValid(Pawn) && !Pawn->IsActorBeingDestroyed() && Pawn->GetWorld() == GetWorld();
}

/// @brief 每 0.25 秒清理一次：超时或失效的轮次，以及过期的接话记录
void UDeliveryConversationTurnSubsystem::Tick(float)
{
    if (IsServerWorld() && FPlatformTime::Seconds() >= NextPruneAt)
    {
        NextPruneAt = FPlatformTime::Seconds() + .25;
        PruneInvalidLeases();

        // PruneInvalidLeases 会发出 OnLeaseExpired，收到通知的代码可能让这个世界开始结束，所以调用 PruneNpcMessages 前再检查一次 IsServerWorld
        if (IsServerWorld())
            PruneNpcMessages();
    }
}

/// @brief 给引擎的性能统计用
TStatId UDeliveryConversationTurnSubsystem::GetStatId() const
{
    RETURN_QUICK_DECLARE_CYCLE_STAT(UDeliveryConversationTurnSubsystem, STATGROUP_Tickables);
}

/// @brief 找出该结束的轮次并删掉，再逐个用 OnLeaseExpired 通知交谈组件
/// 结束的原因有三种：到了时限（TimedOut）、玩家换了角色或身份认不出（OwnerChanged）、NPC 没了（TargetGone）
void UDeliveryConversationTurnSubsystem::PruneInvalidLeases()
{
    check(IsInGameThread());
    const double Now = FPlatformTime::Seconds();

    struct FExpired
    {
        FDeliveryConversationTurnLease Lease;
        EDeliveryConversationLeaseEnd Reason;
    };

    TArray<FExpired> Expired;
    for (auto It = Leases.CreateIterator(); It; ++It)
    {
        auto& Lease = It.Value();
        auto* Owner = Lease.Owner.Get();
        auto* Npc = Lease.Npc.Get();
        EDeliveryConversationLeaseEnd Reason = EDeliveryConversationLeaseEnd::TimedOut;
        bool bExpired = Now >= Lease.Deadline;
        bool bOwnerMatches = false;

        // 开麦轮次：VoiceContext.Check 在玩家上下车时可能短暂返回 Pending，从 VoiceContextPendingSince 起最多等 TransitionGraceSeconds
        // 等过这段时间还是 Pending，或者 Check 返回 Invalid，bOwnerMatches 为 false，按 OwnerChanged 结束
        if (Lease.bVoice)
        {
            const auto State = Lease.VoiceContext.Check(Owner);
            if (Lease.VoiceContextPendingSince >= 0 &&
                Now - Lease.VoiceContextPendingSince >= FDeliverySpeechPlayerContext::TransitionGraceSeconds)
                bOwnerMatches = false;
            else if (State == EDeliverySpeechPlayerContextState::Pending)
            {
                if (Lease.VoiceContextPendingSince < 0)
                    Lease.VoiceContextPendingSince = Now;
                bOwnerMatches = Now - Lease.VoiceContextPendingSince < FDeliverySpeechPlayerContext::TransitionGraceSeconds;
            }
            else
            {
                Lease.VoiceContextPendingSince = -1;
                bOwnerMatches = State == EDeliverySpeechPlayerContextState::Valid;
            }
        }

        // 打字轮次：PC 当前控制的角色必须还是 PawnAtBegin（开口时控制的角色）
        else
            bOwnerMatches = IsValidOwner(Owner) && Owner->GetPawn() == Lease.PawnAtBegin.Get();
        if (!bOwnerMatches)
        {
            bExpired = true;
            Reason = EDeliveryConversationLeaseEnd::OwnerChanged;
        }
        else if (!IsValid(Npc) || Npc->IsActorBeingDestroyed() || Npc->GetWorld() != GetWorld())
        {
            bExpired = true;
            Reason = EDeliveryConversationLeaseEnd::TargetGone;
        }
        if (bExpired)
        {
            Expired.Add({Lease, Reason});
            It.RemoveCurrent();
        }
    }

    // 玩家换角色时保留 Owners 里的输入序号记录，PC 被销毁或离开这个世界后才删
    for (auto It = Owners.CreateIterator(); It; ++It)
    {
        const auto* Owner = It.Key().Get();
        if (!IsValid(Owner) || Owner->IsActorBeingDestroyed() || Owner->GetWorld() != GetWorld())
            It.RemoveCurrent();
    }

    // 上面的循环已经把过期轮次从 Leases 删掉，这里再调用 OnLeaseExpired.Broadcast
    // 收到通知的代码如果再调用 FinishRecording、Cancel 或 ReleaseReply，查不到这一轮，不会重复处理，也不会碰到新开的轮次
    for (const auto& Item : Expired)
        OnLeaseExpired.Broadcast(Item.Lease, Item.Reason);
}

// ── 开口 ──
// 打字调用 TryBeginText，开麦调用 TryBeginVoice，都转到 BeginInternal：NPC 没被占着或被这位玩家占着就接受，被别的玩家占着返回 Busy
// 开麦的轮次 Phase 先是 Recording（录音），玩家说完后调用 FinishRecording 改成 FinalPending（等文字）；打字的轮次直接是 FinalPending
// 文字到了先调用 ValidateFinal 检查，再调用 CommitFinal 把 Phase 改成 AwaitingReply（等 NPC 回复），直到 ReleaseReply 或 Cancel 删掉这一轮

/// @brief 旧入口：先录音再提交文字，但不按开麦检查玩家身份
EDeliveryConversationTurnResult UDeliveryConversationTurnSubsystem::TryBegin(APlayerController* Owner, ADeliveryNpcCharacter* Npc,
                                                                             FGuid Token, uint64 InputSequence,
                                                                             FDeliveryConversationTurnHandle& OutHandle,
                                                                             TArray<FDeliveryConversationTurnLease>& OutReplaced)
{
    return BeginInternal(Owner, Npc, Token, InputSequence, false, false, OutHandle, OutReplaced);
}

/// @brief 玩家打字说话时开始一轮，直接进入等文字阶段
EDeliveryConversationTurnResult
UDeliveryConversationTurnSubsystem::TryBeginText(APlayerController* Owner, ADeliveryNpcCharacter* Npc, FGuid Token,
                                                 uint64 InputSequence, FDeliveryConversationTurnHandle& OutHandle,
                                                 TArray<FDeliveryConversationTurnLease>& OutReplaced)
{
    return BeginInternal(Owner, Npc, Token, InputSequence, true, false, OutHandle, OutReplaced);
}

/// @brief 玩家开麦说话时开始一轮，先进入录音阶段，并记下玩家当时的身份
EDeliveryConversationTurnResult
UDeliveryConversationTurnSubsystem::TryBeginVoice(APlayerController* Owner, ADeliveryNpcCharacter* Npc, FGuid Token,
                                                  uint64 InputSequence, FDeliveryConversationTurnHandle& OutHandle,
                                                  TArray<FDeliveryConversationTurnLease>& OutReplaced)
{
    return BeginInternal(Owner, Npc, Token, InputSequence, false, true, OutHandle, OutReplaced);
}

/// @brief 三个开始入口共用的实现
/// 同一句话重发返回 AlreadyActive 和原来的 Handle；InputSequence 比这位玩家用过的旧返回 Stale；NPC 被别的玩家占着返回 Busy
/// 接受时，这位玩家对其他 NPC 还没交出文字的轮次会被顶掉，放进 OutReplaced 交给调用方打断
EDeliveryConversationTurnResult UDeliveryConversationTurnSubsystem::BeginInternal(
    APlayerController* Owner, ADeliveryNpcCharacter* Npc, FGuid Token, uint64 InputSequence, bool bText, bool bVoice,
    FDeliveryConversationTurnHandle& OutHandle, TArray<FDeliveryConversationTurnLease>& OutReplaced)
{
    check(IsInGameThread());
    OutHandle = {};
    OutReplaced.Reset();
    if (!IsServerWorld())
        return EDeliveryConversationTurnResult::Invalid;
    PruneInvalidLeases();
    if (!IsValidOwner(Owner) || !IsValid(Npc) || Npc->IsActorBeingDestroyed() || Npc->GetWorld() != GetWorld() ||
        !Token.IsValid() || InputSequence == 0)
        return EDeliveryConversationTurnResult::Invalid;

    // 开麦时调用 VoiceContext.Capture 记下玩家此刻的身份，之后 FindOwned 和 PruneInvalidLeases 都用它检查
    FDeliverySpeechPlayerContext VoiceContext;
    if (bVoice && !VoiceContext.Capture(Owner))
        return EDeliveryConversationTurnResult::Invalid;

    const TWeakObjectPtr<ADeliveryNpcCharacter> NpcKey(Npc);
    const TWeakObjectPtr<APlayerController> OwnerKey(Owner);
    const auto* PriorHistory = Owners.Find(OwnerKey);
    if (PriorHistory && InputSequence < PriorHistory->HighestInputSequence)
        return EDeliveryConversationTurnResult::Stale;
    const auto* Existing = Leases.Find(NpcKey);

    // 同一个 PC、同一个 Token、同一个 InputSequence：这是网络重发的同一句话，返回原来的 Handle，不重置 Deadline
    if (Existing && Existing->Owner == Owner && Existing->ClientToken == Token && Existing->Handle.InputSequence == InputSequence)
    {
        OutHandle = Existing->Handle;
        return EDeliveryConversationTurnResult::AlreadyActive;
    }

    // InputSequence 不大于 HighestInputSequence，或者 RecentTokens 里已有这个 Token，都说明是晚到的旧请求
    if (const auto* History = Owners.Find(OwnerKey))
        if (InputSequence <= History->HighestInputSequence || History->RecentTokens.Contains(Token))
            return EDeliveryConversationTurnResult::Stale;

    // NPC 被别的玩家占着：返回 Busy，这位玩家自己的其他轮次都不删
    if (Existing && Existing->Owner != Owner)
        return EDeliveryConversationTurnResult::Busy;

    // 删掉这位玩家对同一位 NPC 的旧轮次，以及对其他 NPC 的 Phase 不是 AwaitingReply 的轮次，放进 OutReplaced；对其他 NPC 已经在等回复的保留
    for (auto It = Leases.CreateIterator(); It; ++It)
    {
        const auto& Lease = It.Value();
        if (Lease.Owner == Owner && (Lease.Npc == Npc || Lease.Phase != EDeliveryConversationTurnPhase::AwaitingReply))
        {
            OutReplaced.Add(Lease);
            It.RemoveCurrent();
        }
    }

    // 生成新的 Handle：带上 WorldEpoch、新的 Token 和 InputSequence
    // Deadline 打字是 FinalTextTimeoutSeconds（5 秒），开麦是 RecordingTimeoutSeconds（15 秒）
    const double Now = FPlatformTime::Seconds();
    FDeliveryConversationTurnLease NewLease;
    NewLease.Handle.WorldEpoch = WorldEpoch;
    NewLease.Handle.Token = FGuid::NewGuid();
    NewLease.Handle.InputSequence = InputSequence;
    NewLease.ClientToken = Token;
    NewLease.Owner = Owner;
    NewLease.PawnAtBegin = Owner->GetPawn();
    NewLease.VoiceContext = VoiceContext;
    NewLease.bVoice = bVoice;
    NewLease.Npc = Npc;
    NewLease.Phase = bText ? EDeliveryConversationTurnPhase::FinalPending : EDeliveryConversationTurnPhase::Recording;
    NewLease.StartedAt = Now;
    NewLease.Deadline = Now + (bText ? FinalTextTimeoutSeconds : RecordingTimeoutSeconds);
    OutHandle = NewLease.Handle;
    Leases.Add(NpcKey, MoveTemp(NewLease));

    // 更新 HighestInputSequence，并在 RecentTokens 里保留最近 64 个 Token，用来认出晚到的旧请求
    auto& History = Owners.FindOrAdd(OwnerKey);
    History.HighestInputSequence = InputSequence;
    History.RecentTokens.Add(Token);
    if (History.RecentTokens.Num() > 64)
        History.RecentTokens.RemoveAt(0, History.RecentTokens.Num() - 64, EAllowShrinking::No);
    return EDeliveryConversationTurnResult::Accepted;
}

/// @brief 按 Handle 找到属于这位玩家的轮次
/// 开麦轮次要求玩家身份对得上；打字轮次要求玩家还控制着开口时的角色
/// @param bAllowPendingVoice true 时身份暂时认不出（例如正在上下车）的开麦轮次也算
/// @return 找不到或不属于这位玩家时返回 nullptr
FDeliveryConversationTurnLease* UDeliveryConversationTurnSubsystem::FindOwned(APlayerController* Owner,
                                                                              const FDeliveryConversationTurnHandle& Handle,
                                                                              bool bAllowPendingVoice)
{
    if (!IsServerWorld() || !IsValid(Owner) || Owner->GetWorld() != GetWorld() || !Owner->HasAuthority() || !Handle.IsValid() ||
        Handle.WorldEpoch != WorldEpoch)
        return nullptr;
    for (auto& Pair : Leases)
    {
        auto& Lease = Pair.Value;
        if (Lease.Owner != Owner || !(Lease.Handle == Handle))
            continue;
        if (Lease.bVoice)
        {
            const auto State = Lease.VoiceContext.Check(Owner);
            if (State == EDeliverySpeechPlayerContextState::Valid ||
                (bAllowPendingVoice && State == EDeliverySpeechPlayerContextState::Pending))
                return &Lease;
        }
        else if (IsValidOwner(Owner) && Lease.PawnAtBegin == Owner->GetPawn())
            return &Lease;
    }
    return nullptr;
}

/// @brief 查询开麦轮次里玩家的身份现在是认得出、暂时认不出，还是已经不对了
EDeliverySpeechPlayerContextState
UDeliveryConversationTurnSubsystem::GetVoiceContextState(APlayerController* Owner, const FDeliveryConversationTurnHandle& Handle)
{
    if (!IsServerWorld())
        return EDeliverySpeechPlayerContextState::Invalid;
    PruneInvalidLeases();
    const auto* Lease = FindOwned(Owner, Handle, true);
    return Lease && Lease->bVoice ? Lease->VoiceContext.Check(Owner) : EDeliverySpeechPlayerContextState::Invalid;
}

/// @brief 玩家开麦说完一句：从录音阶段进入等文字阶段，给语音识别 5 秒交出文字
EDeliveryConversationTurnResult UDeliveryConversationTurnSubsystem::FinishRecording(APlayerController* Owner,
                                                                                    const FDeliveryConversationTurnHandle& Handle)
{
    check(IsInGameThread());
    if (!IsServerWorld())
        return EDeliveryConversationTurnResult::Invalid;
    PruneInvalidLeases();

    // FindOwned 的第三个参数传 true：VoiceContext 暂时是 Pending 也能结束录音、开始计时；ValidateFinal 调用 FindOwned 时不传 true，那时要求身份认得出
    auto* Lease = FindOwned(Owner, Handle, true);
    if (!Lease)
        return EDeliveryConversationTurnResult::Stale;
    if (Lease->Phase == EDeliveryConversationTurnPhase::FinalPending)
        return EDeliveryConversationTurnResult::AlreadyActive;
    if (Lease->Phase != EDeliveryConversationTurnPhase::Recording)
        return EDeliveryConversationTurnResult::WrongPhase;
    Lease->Phase = EDeliveryConversationTurnPhase::FinalPending;
    Lease->Deadline = FPlatformTime::Seconds() + FinalTextTimeoutSeconds;
    return EDeliveryConversationTurnResult::Accepted;
}

/// @brief 提交文字之前检查：这一轮还属于这位玩家、对象是这位 NPC、正处于等文字阶段
EDeliveryConversationTurnResult UDeliveryConversationTurnSubsystem::ValidateFinal(APlayerController* Owner,
                                                                                  ADeliveryNpcCharacter* Npc,
                                                                                  const FDeliveryConversationTurnHandle& Handle)
{
    check(IsInGameThread());
    if (!IsServerWorld())
        return EDeliveryConversationTurnResult::Invalid;
    PruneInvalidLeases();
    auto* Lease = FindOwned(Owner, Handle);
    if (!Lease || !IsValid(Npc) || Lease->Npc != Npc)
        return EDeliveryConversationTurnResult::Stale;
    return Lease->Phase == EDeliveryConversationTurnPhase::FinalPending ? EDeliveryConversationTurnResult::Accepted
                                                                        : EDeliveryConversationTurnResult::WrongPhase;
}

/// @brief 记下文字已经交给 NPC：进入等回复阶段，最多等 300 秒
/// @param MessageId 交给 NPC 的消息编号，必须等于 Handle.Token
bool UDeliveryConversationTurnSubsystem::CommitFinal(APlayerController* Owner, ADeliveryNpcCharacter* Npc,
                                                     const FDeliveryConversationTurnHandle& Handle, FGuid MessageId)
{
    check(IsInGameThread());
    if (!MessageId.IsValid() || MessageId != Handle.Token ||
        ValidateFinal(Owner, Npc, Handle) != EDeliveryConversationTurnResult::Accepted)
        return false;
    auto* Lease = FindOwned(Owner, Handle);
    if (!Lease)
        return false;
    Lease->MessageId = MessageId;
    Lease->Phase = EDeliveryConversationTurnPhase::AwaitingReply;
    Lease->Deadline = FPlatformTime::Seconds() + ReplyFailureTimeoutSeconds;
    return true;
}

// ── 结束 ──
// Cancel、ReleasePlayer、ReleaseReply 都从 Leases 删掉记录，NPC 就能听别人说话；删掉的记录通过 OutRemoved 交给调用方，用来停掉对应的声音和界面

/// @brief 玩家取消某一轮，删掉记录
/// @param OutRemoved 不为空时写入被删掉的那一轮
bool UDeliveryConversationTurnSubsystem::Cancel(APlayerController* Owner, const FDeliveryConversationTurnHandle& Handle,
                                                FDeliveryConversationTurnLease* OutRemoved)
{
    check(IsInGameThread());
    if (OutRemoved)
        *OutRemoved = {};
    if (!IsServerWorld() || !Handle.IsValid() || Handle.WorldEpoch != WorldEpoch)
        return false;
    PruneInvalidLeases();
    for (auto It = Leases.CreateIterator(); It; ++It)
        if (It.Value().Owner == Owner && It.Value().Handle == Handle)
        {
            if (OutRemoved)
                *OutRemoved = It.Value();
            It.RemoveCurrent();
            return true;
        }
    return false;
}

/// @brief 玩家离开时删掉他的全部轮次
void UDeliveryConversationTurnSubsystem::ReleasePlayer(APlayerController* Owner,
                                                       TArray<FDeliveryConversationTurnLease>& OutRemoved)
{
    check(IsInGameThread());
    OutRemoved.Reset();
    if (!Owner)
        return;
    for (auto It = Leases.CreateIterator(); It; ++It)
        if (It.Value().Owner.GetEvenIfUnreachable() == Owner)
        {
            OutRemoved.Add(It.Value());
            It.RemoveCurrent();
        }

// 不删 Owners 里的序号记录：这个 PC 还在的话，晚到的旧 TryBeginText 或 TryBeginVoice 仍会因为 InputSequence 太旧被拒绝
}

/// @brief NPC 回复播完或确定回不了时调用，删掉这一轮，NPC 可以听别人说话
/// @param MessageId 玩家那句话的编号，只有正在等这句话回复的轮次才会被删
bool UDeliveryConversationTurnSubsystem::ReleaseReply(ADeliveryNpcCharacter* Npc, FGuid MessageId,
                                                      FDeliveryConversationTurnLease* OutRemoved)
{
    check(IsInGameThread());
    if (OutRemoved)
        *OutRemoved = {};
    if (!IsServerWorld() || !IsValid(Npc) || Npc->GetWorld() != GetWorld() || !MessageId.IsValid())
        return false;
    PruneInvalidLeases();
    const TWeakObjectPtr<ADeliveryNpcCharacter> Key(Npc);
    const auto* Lease = Leases.Find(Key);
    if (!Lease || Lease->Phase != EDeliveryConversationTurnPhase::AwaitingReply || Lease->MessageId != MessageId)
        return false;
    if (OutRemoved)
        *OutRemoved = *Lease;
    Leases.Remove(Key);
    return true;
}

/// @brief 查询这位 NPC 现在被哪一轮占着
/// 查询前会先清理过期的轮次，这时可能发出 OnLeaseExpired 通知
/// @return 有人占着时返回 true，并把那一轮写进 OutLease
bool UDeliveryConversationTurnSubsystem::GetLease(ADeliveryNpcCharacter* Npc, FDeliveryConversationTurnLease& OutLease)
{
    check(IsInGameThread());
    OutLease = {};
    if (!IsServerWorld() || !IsValid(Npc) || Npc->GetWorld() != GetWorld())
        return false;
    PruneInvalidLeases();
    if (const auto* Lease = Leases.Find(TWeakObjectPtr<ADeliveryNpcCharacter>(Npc)))
    {
        OutLease = *Lease;
        return true;
    }
    return false;
}
