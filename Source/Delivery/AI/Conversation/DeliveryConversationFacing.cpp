// ── 朝向 ──
// 本机先调用 PrepareConversationFacingInput，生成转身序号并让主角准备转身，序号随打开交谈或发言的请求发给服务器
// 服务器先调用 RegisterConversationFacingInput 登记这个序号，NPC 接收这句话后再调用 ConfirmConversationFacingInput，主角才转向这位 NPC
// LastServerFacingSequence 只增不减：玩家连按几次时只有最后一次能转身，晚到的旧请求序号对不上，主角不会转回旧目标
#include "AI/Conversation/DeliveryConversationComponent.h"
#include "AI/Characters/DeliveryNpcCharacter.h"
#include "Character/DeliveryCharacter.h"
#include "Ragdoll/DeliveryActiveRagdollComponent.h"
#include "GameFramework/PlayerController.h"

/// @brief 本机：玩家按下说话键时生成新的转身序号，让当前步行的主角先做好转身准备
/// @return 新序号；不是本机玩家或组件正在结束时返回 0，表示这次不转身
uint64 UDeliveryConversationComponent::PrepareConversationFacingInput()
{
    auto* PC = Cast<APlayerController>(GetOwner());
    if (bEndingPlay || !PC || !PC->IsLocalController())
        return 0;

    // 玩家上次按键时控制的角色已经换了（例如刚上车），取消那具身体还在等的转身
    if (auto* Previous = Cast<ADeliveryCharacter>(LocalFacingPawn.Get()); Previous && Previous != PC->GetPawn())
        if (auto* Body = Previous->GetActiveRagdoll())
            Body->CancelConversationFacing();

    // LastLocalFacingSequence 加一；溢出回到 0 时再加一次，因为返回 0 表示这次不转身
    ++LastLocalFacingSequence;
    if (!LastLocalFacingSequence)
        ++LastLocalFacingSequence;
    LocalFacingPawn = PC->GetPawn();

    // 骑车时 PC->GetPawn() 是车，Cast 到 ADeliveryCharacter 失败，不调用 PrepareConversationFacing，主角不转身
    if (auto* Character = Cast<ADeliveryCharacter>(PC->GetPawn()))
        if (auto* Body = Character->GetActiveRagdoll())
            Body->PrepareConversationFacing(PC, LastLocalFacingSequence);
    return LastLocalFacingSequence;
}

/// @brief 服务器：收到发言时登记这次按键的转身序号，比已登记的旧就忽略
/// @param Resident 玩家对着说话的 NPC，可以为空或已失效，序号照样登记
/// @param ClientToken 本机给这句话的编号，确认转身时要对上
/// @param Sequence 本机生成的转身序号
/// @param PawnAtInput 按键时玩家控制的角色，换过角色的请求不登记
void UDeliveryConversationComponent::RegisterConversationFacingInput(ADeliveryNpcCharacter* Resident, FGuid ClientToken,
                                                                     uint64 Sequence, APawn* PawnAtInput)
{
    auto* PC = Cast<APlayerController>(GetOwner());
    if (bEndingPlay || !PC || !PC->HasAuthority() || !Sequence || Sequence <= LastServerFacingSequence || !IsValid(PawnAtInput) ||
        PawnAtInput != PC->GetPawn() || PawnAtInput->GetWorld() != GetWorld() || PawnAtInput->GetController() != PC)
        return;

    // 先写 LastServerFacingSequence 再检查 Resident；之后这句话就算被拒绝，旧请求调用 ConfirmConversationFacingInput 时序号也对不上，主角不转身
    LastServerFacingSequence = Sequence;
    ServerFacingClientToken = ClientToken;
    ServerFacingTarget.Reset();
    if (IsValid(Resident) && Resident->GetWorld() == GetWorld() && !Resident->IsActorBeingDestroyed())
        ServerFacingTarget = Resident;

    // 玩家换了角色，取消旧身体上还在等确认的转身
    if (auto* Previous = Cast<ADeliveryCharacter>(ServerFacingPawn.Get()); Previous && Previous != PC->GetPawn())
        if (auto* Body = Previous->GetActiveRagdoll())
            Body->CancelConversationFacing();
    ServerFacingPawn = PC->GetPawn();

    // PrepareConversationFacing 让身体清掉旧目标、记下新序号；之后 ConfirmConversationFacingInput 才告诉身体转向哪位 NPC
    if (auto* Character = Cast<ADeliveryCharacter>(PC->GetPawn()))
        if (auto* Body = Character->GetActiveRagdoll())
            Body->PrepareConversationFacing(PC, Sequence);
}

/// @brief 服务器：NPC 接收这句话后确认转身，主角转向这位 NPC
/// @param Resident 接收发言的 NPC，要和登记时的目标是同一位
/// @param ClientToken 本机编号，要和登记时的一致
/// @param Sequence 转身序号；传 0 表示用最新登记的序号
void UDeliveryConversationComponent::ConfirmConversationFacingInput(ADeliveryNpcCharacter* Resident, FGuid ClientToken,
                                                                    uint64 Sequence)
{
    auto* PC = Cast<APlayerController>(GetOwner());

    // Sequence、ServerFacingTarget、ServerFacingClientToken、ServerFacingPawn 任一和登记时不同，或者 CanTalkTo 不通过，就不转身
    if (bEndingPlay || !PC || !PC->HasAuthority() || !LastServerFacingSequence || !IsValid(Resident) ||
        Resident->GetWorld() != GetWorld() || Resident->IsActorBeingDestroyed() ||
        (Sequence && Sequence != LastServerFacingSequence) || ServerFacingTarget.Get() != Resident ||
        ServerFacingClientToken != ClientToken || ServerFacingPawn.Get() != PC->GetPawn() || !CanTalkTo(Resident))
        return;
    if (auto* Character = Cast<ADeliveryCharacter>(PC->GetPawn()))
        if (auto* Body = Character->GetActiveRagdoll())
            Body->ConfirmConversationFacing(Resident, PC, LastServerFacingSequence);
}

/// @brief 组件结束时取消本机和服务器两边还在等的转身，并清空登记
void UDeliveryConversationComponent::CancelConversationFacingInput()
{
    if (auto* Character = Cast<ADeliveryCharacter>(LocalFacingPawn.Get()))
        if (auto* Body = Character->GetActiveRagdoll())
            Body->CancelConversationFacing();
    if (auto* Character = Cast<ADeliveryCharacter>(ServerFacingPawn.Get()))
        if (auto* Body = Character->GetActiveRagdoll())
            Body->CancelConversationFacing();
    LocalFacingPawn.Reset();
    ServerFacingPawn.Reset();
    ServerFacingTarget.Reset();
    ServerFacingClientToken.Invalidate();
}
