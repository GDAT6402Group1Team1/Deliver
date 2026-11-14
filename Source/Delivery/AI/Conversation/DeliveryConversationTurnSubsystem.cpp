#include "AI/Conversation/DeliveryConversationTurnSubsystem.h"

#include "AI/Characters/DeliveryNpcCharacter.h"
#include "Engine/World.h"
#include "GameFramework/Pawn.h"
#include "GameFramework/PlayerController.h"
#include "HAL/PlatformTime.h"

bool UDeliveryConversationTurnSubsystem::DoesSupportWorldType(EWorldType::Type WorldType) const
{
    return WorldType == EWorldType::Game || WorldType == EWorldType::PIE;
}

bool UDeliveryConversationTurnSubsystem::ShouldCreateSubsystem(UObject* Outer) const
{
    const auto* World = Cast<UWorld>(Outer);
    return World && World->IsGameWorld() && World->GetNetMode() != NM_Client && Super::ShouldCreateSubsystem(Outer);
}

void UDeliveryConversationTurnSubsystem::Initialize(FSubsystemCollectionBase& Collection)
{
    Super::Initialize(Collection);
    WorldEpoch = FGuid::NewGuid();
    bDeinitializing = false;
}

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

bool UDeliveryConversationTurnSubsystem::IsServerWorld() const
{
    const UWorld* World = GetWorld();
    return !bDeinitializing && WorldEpoch.IsValid() && World && World->IsGameWorld() && World->GetNetMode() != NM_Client;
}

bool UDeliveryConversationTurnSubsystem::IsValidOwner(const APlayerController* Owner) const
{
    if (!IsServerWorld() || !IsValid(Owner) || Owner->IsActorBeingDestroyed() || Owner->GetWorld() != GetWorld() || !Owner->HasAuthority()) return false;
    const APawn* Pawn = Owner->GetPawn();
    return IsValid(Pawn) && !Pawn->IsActorBeingDestroyed() && Pawn->GetWorld() == GetWorld();
}

void UDeliveryConversationTurnSubsystem::Tick(float)
{
    if (IsServerWorld() && FPlatformTime::Seconds() >= NextPruneAt)
    {
        NextPruneAt=FPlatformTime::Seconds()+.25;
        PruneInvalidLeases();
        if (IsServerWorld()) PruneNpcMessages();
    }
}

TStatId UDeliveryConversationTurnSubsystem::GetStatId() const
{
    RETURN_QUICK_DECLARE_CYCLE_STAT(UDeliveryConversationTurnSubsystem, STATGROUP_Tickables);
}

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
        const auto& Lease = It.Value();
        auto* Owner = Lease.Owner.Get();
        auto* Npc = Lease.Npc.Get();
        EDeliveryConversationLeaseEnd Reason = EDeliveryConversationLeaseEnd::TimedOut;
        bool bExpired = Now >= Lease.Deadline;
        if (!IsValidOwner(Owner) || Owner->GetPawn() != Lease.PawnAtBegin.Get())
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
    for (auto It = Owners.CreateIterator(); It; ++It)
    {
        const auto* Owner = It.Key().Get();
        // 换 Pawn 仍保留输入序号，只在原控制器退出世界后清掉历史
        if (!IsValid(Owner) || Owner->IsActorBeingDestroyed() || Owner->GetWorld() != GetWorld()) It.RemoveCurrent();
    }
    // 先从映射移除，再通知外层取消；回调中的旧 Finish 不会再次释放或碰到新轮次
    for (const auto& Item : Expired) OnLeaseExpired.Broadcast(Item.Lease, Item.Reason);
}

EDeliveryConversationTurnResult UDeliveryConversationTurnSubsystem::TryBegin(APlayerController* Owner,
    ADeliveryNpcCharacter* Npc, FGuid Token, uint64 InputSequence, FDeliveryConversationTurnHandle& OutHandle,
    TArray<FDeliveryConversationTurnLease>& OutReplaced)
{
    return BeginInternal(Owner, Npc, Token, InputSequence, false, OutHandle, OutReplaced);
}

EDeliveryConversationTurnResult UDeliveryConversationTurnSubsystem::TryBeginText(APlayerController* Owner,
    ADeliveryNpcCharacter* Npc, FGuid Token, uint64 InputSequence, FDeliveryConversationTurnHandle& OutHandle,
    TArray<FDeliveryConversationTurnLease>& OutReplaced)
{
    return BeginInternal(Owner, Npc, Token, InputSequence, true, OutHandle, OutReplaced);
}

EDeliveryConversationTurnResult UDeliveryConversationTurnSubsystem::BeginInternal(APlayerController* Owner,
    ADeliveryNpcCharacter* Npc, FGuid Token, uint64 InputSequence, bool bText,
    FDeliveryConversationTurnHandle& OutHandle, TArray<FDeliveryConversationTurnLease>& OutReplaced)
{
    check(IsInGameThread());
    OutHandle = {};
    OutReplaced.Reset();
    if (!IsServerWorld()) return EDeliveryConversationTurnResult::Invalid;
    PruneInvalidLeases();
    if (!IsValidOwner(Owner) || !IsValid(Npc) || Npc->IsActorBeingDestroyed() || Npc->GetWorld() != GetWorld() ||
        !Token.IsValid() || InputSequence == 0) return EDeliveryConversationTurnResult::Invalid;

    const TWeakObjectPtr<ADeliveryNpcCharacter> NpcKey(Npc);
    const TWeakObjectPtr<APlayerController> OwnerKey(Owner);
    const auto* PriorHistory = Owners.Find(OwnerKey);
    if (PriorHistory && InputSequence < PriorHistory->HighestInputSequence) return EDeliveryConversationTurnResult::Stale;
    const auto* Existing = Leases.Find(NpcKey);
    if (Existing && Existing->Owner == Owner && Existing->ClientToken == Token && Existing->Handle.InputSequence == InputSequence)
    {
        OutHandle = Existing->Handle;
        return EDeliveryConversationTurnResult::AlreadyActive;
    }
    if (const auto* History = Owners.Find(OwnerKey))
        if (InputSequence <= History->HighestInputSequence || History->RecentTokens.Contains(Token)) return EDeliveryConversationTurnResult::Stale;
    // 新目标忙碌时保持原输入，不能先清掉玩家自己的另一份录音
    if (Existing && Existing->Owner != Owner) return EDeliveryConversationTurnResult::Busy;

    for (auto It = Leases.CreateIterator(); It; ++It)
    {
        const auto& Lease = It.Value();
        if (Lease.Owner == Owner && (Lease.Npc == Npc || Lease.Phase != EDeliveryConversationTurnPhase::AwaitingReply))
        {
            OutReplaced.Add(Lease);
            It.RemoveCurrent();
        }
    }
    const double Now = FPlatformTime::Seconds();
    FDeliveryConversationTurnLease NewLease;
    NewLease.Handle.WorldEpoch = WorldEpoch;
    NewLease.Handle.Token = FGuid::NewGuid();
    NewLease.Handle.InputSequence = InputSequence;
    NewLease.ClientToken = Token;
    NewLease.Owner = Owner;
    NewLease.PawnAtBegin = Owner->GetPawn();
    NewLease.Npc = Npc;
    NewLease.Phase = bText ? EDeliveryConversationTurnPhase::FinalPending : EDeliveryConversationTurnPhase::Recording;
    NewLease.StartedAt = Now;
    NewLease.Deadline = Now + (bText ? FinalTextTimeoutSeconds : RecordingTimeoutSeconds);
    OutHandle = NewLease.Handle;
    Leases.Add(NpcKey, MoveTemp(NewLease));

    auto& History = Owners.FindOrAdd(OwnerKey);
    History.HighestInputSequence = InputSequence;
    History.RecentTokens.Add(Token);
    if (History.RecentTokens.Num() > 64) History.RecentTokens.RemoveAt(0, History.RecentTokens.Num() - 64, EAllowShrinking::No);
    return EDeliveryConversationTurnResult::Accepted;
}

FDeliveryConversationTurnLease* UDeliveryConversationTurnSubsystem::FindOwned(APlayerController* Owner,
    const FDeliveryConversationTurnHandle& Handle)
{
    if (!IsValidOwner(Owner) || !Handle.IsValid() || Handle.WorldEpoch != WorldEpoch) return nullptr;
    for (auto& Pair : Leases)
        if (Pair.Value.Owner == Owner && Pair.Value.Handle == Handle && Pair.Value.PawnAtBegin == Owner->GetPawn()) return &Pair.Value;
    return nullptr;
}

EDeliveryConversationTurnResult UDeliveryConversationTurnSubsystem::FinishRecording(APlayerController* Owner,
    const FDeliveryConversationTurnHandle& Handle)
{
    check(IsInGameThread());
    if (!IsServerWorld()) return EDeliveryConversationTurnResult::Invalid;
    PruneInvalidLeases();
    auto* Lease = FindOwned(Owner, Handle);
    if (!Lease) return EDeliveryConversationTurnResult::Stale;
    if (Lease->Phase == EDeliveryConversationTurnPhase::FinalPending) return EDeliveryConversationTurnResult::AlreadyActive;
    if (Lease->Phase != EDeliveryConversationTurnPhase::Recording) return EDeliveryConversationTurnResult::WrongPhase;
    Lease->Phase = EDeliveryConversationTurnPhase::FinalPending;
    Lease->Deadline = FPlatformTime::Seconds() + FinalTextTimeoutSeconds;
    return EDeliveryConversationTurnResult::Accepted;
}

EDeliveryConversationTurnResult UDeliveryConversationTurnSubsystem::ValidateFinal(APlayerController* Owner,
    ADeliveryNpcCharacter* Npc, const FDeliveryConversationTurnHandle& Handle)
{
    check(IsInGameThread());
    if (!IsServerWorld()) return EDeliveryConversationTurnResult::Invalid;
    PruneInvalidLeases();
    auto* Lease = FindOwned(Owner, Handle);
    if (!Lease || !IsValid(Npc) || Lease->Npc != Npc) return EDeliveryConversationTurnResult::Stale;
    return Lease->Phase == EDeliveryConversationTurnPhase::FinalPending
        ? EDeliveryConversationTurnResult::Accepted : EDeliveryConversationTurnResult::WrongPhase;
}

bool UDeliveryConversationTurnSubsystem::CommitFinal(APlayerController* Owner, ADeliveryNpcCharacter* Npc,
    const FDeliveryConversationTurnHandle& Handle, FGuid MessageId)
{
    check(IsInGameThread());
    if (!MessageId.IsValid() || MessageId != Handle.Token ||
        ValidateFinal(Owner, Npc, Handle) != EDeliveryConversationTurnResult::Accepted) return false;
    auto* Lease = FindOwned(Owner, Handle);
    if (!Lease) return false;
    Lease->MessageId = MessageId;
    Lease->Phase = EDeliveryConversationTurnPhase::AwaitingReply;
    Lease->Deadline = FPlatformTime::Seconds() + ReplyFailureTimeoutSeconds;
    return true;
}

bool UDeliveryConversationTurnSubsystem::Cancel(APlayerController* Owner, const FDeliveryConversationTurnHandle& Handle,
    FDeliveryConversationTurnLease* OutRemoved)
{
    check(IsInGameThread());
    if (OutRemoved) *OutRemoved = {};
    if (!IsServerWorld() || !Handle.IsValid() || Handle.WorldEpoch != WorldEpoch) return false;
    PruneInvalidLeases();
    for (auto It = Leases.CreateIterator(); It; ++It)
        if (It.Value().Owner == Owner && It.Value().Handle == Handle)
        {
            if (OutRemoved) *OutRemoved = It.Value();
            It.RemoveCurrent();
            return true;
        }
    return false;
}

void UDeliveryConversationTurnSubsystem::ReleasePlayer(APlayerController* Owner, TArray<FDeliveryConversationTurnLease>& OutRemoved)
{
    check(IsInGameThread());
    OutRemoved.Reset();
    if (!Owner) return;
    for (auto It = Leases.CreateIterator(); It; ++It)
        if (It.Value().Owner.GetEvenIfUnreachable() == Owner)
        {
            OutRemoved.Add(It.Value());
            It.RemoveCurrent();
        }
    // 仍存活的控制器保留最高序号，晚到 Begin 不能在释放后恢复旧轮次
}

bool UDeliveryConversationTurnSubsystem::ReleaseReply(ADeliveryNpcCharacter* Npc, FGuid MessageId,
    FDeliveryConversationTurnLease* OutRemoved)
{
    check(IsInGameThread());
    if (OutRemoved) *OutRemoved = {};
    if (!IsServerWorld() || !IsValid(Npc) || Npc->GetWorld() != GetWorld() || !MessageId.IsValid()) return false;
    PruneInvalidLeases();
    const TWeakObjectPtr<ADeliveryNpcCharacter> Key(Npc);
    const auto* Lease = Leases.Find(Key);
    if (!Lease || Lease->Phase != EDeliveryConversationTurnPhase::AwaitingReply || Lease->MessageId != MessageId) return false;
    if (OutRemoved) *OutRemoved = *Lease;
    Leases.Remove(Key);
    return true;
}

bool UDeliveryConversationTurnSubsystem::GetLease(ADeliveryNpcCharacter* Npc, FDeliveryConversationTurnLease& OutLease)
{
    check(IsInGameThread());
    OutLease = {};
    if (!IsServerWorld() || !IsValid(Npc) || Npc->GetWorld() != GetWorld()) return false;
    PruneInvalidLeases();
    if (const auto* Lease = Leases.Find(TWeakObjectPtr<ADeliveryNpcCharacter>(Npc)))
    {
        OutLease = *Lease;
        return true;
    }
    return false;
}
