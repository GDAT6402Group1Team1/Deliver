#include "AI/Companion/DeliveryCompanionComponent.h"
#include "AI/Companion/DeliveryCompanionCharacter.h"
#include "AI/Companion/DeliveryCompanionTuning.h"
#include "AI/Actions/DeliveryNpcCombatComponent.h"
#include "AI/Decision/DeliveryNpcAgentComponent.h"
#include "AI/Conversation/DeliveryNpcConversationTrace.h"
#include "Character/DeliveryCharacter.h"
#include "Combat/DeliveryCombatEvents.h"
#include "Inventory/DeliveryPackageItem.h"
#include "Task/World/DeliveryItemComponent.h"
#include "Task/DeliveryTaskManagerComponent.h"
#include "Task/Data/DeliveryTaskDefinition.h"
#include "Vehicle/DeliveryMotorbike.h"
#include "GameFramework/PlayerState.h"
#include "GameFramework/PlayerController.h"

namespace
{
// 当前归属最多记录 8192 个事实编号，达到上限后保留去重记录并停止新增关系结算
// 不淘汰旧编号，避免长局重发旧命中或扶起时重复加减关系；解除主人后一起清空
constexpr int32 MaxRecordedMemoryEvents=8192;
// 这些事实的最新一条携带真实累计次数，后续闲聊不能把重要经历挤出上下文
bool IsImportantCompanionMemory(FName Kind)
{
    return Kind==TEXT("FirstHelp") || Kind==TEXT("HelpedUp") || Kind==TEXT("DeliveryTogether") ||
        Kind==TEXT("OwnerHit") || Kind==TEXT("OwnerHitWhileDown") || Kind==TEXT("OwnerVehicleHit");
}
}

FString UDeliveryCompanionComponent::GetRelationshipStage() const
{
    if (!IsValid(OwnerPlayerState)) return TEXT("未结识");
    if (Affinity>=85) return TEXT("铁哥们");
    if (Affinity>=50) return TEXT("刚认大哥");
    if (Affinity>=20) return TEXT("受委屈");
    return TEXT("寒心");
}

bool UDeliveryCompanionComponent::RecordMemory(FGuid EventId,FName Kind,const FString& Description,int32 AffinityDelta)
{
    if (!GetOwner()->HasAuthority() || !CanCommand(OwnerPlayerState) || !EventId.IsValid() || Kind.IsNone() ||
        Description.IsEmpty() || RecordedMemoryEvents.Contains(EventId)) return false;
    if (RecordedMemoryEvents.Num()>=MaxRecordedMemoryEvents)
    {
        if (!bReportedMemoryLimit)
        {
            bReportedMemoryLimit=true;
            UE_LOG(LogTemp,Warning,TEXT("CompanionMemory %s reached %d facts for the current owner; new memories and affinity changes are paused"),
                *GetNameSafe(GetOwner()),MaxRecordedMemoryEvents);
        }
        return false;
    }
    RecordedMemoryEvents.Add(EventId);
    FDeliveryNpcCompanionMemory Memory;
    Memory.EventId=EventId; Memory.Kind=Kind; Memory.Description=Description.Left(256);
    Memories.Add(MoveTemp(Memory));
    if (Memories.Num()>24)
    {
        for (int32 Index=0;Index<Memories.Num();++Index)
        {
            bool bKeep=IsImportantCompanionMemory(Memories[Index].Kind);
            for (int32 Later=Index+1;bKeep && Later<Memories.Num();++Later)
                if (Memories[Later].Kind==Memories[Index].Kind) bKeep=false;
            if (!bKeep) { Memories.RemoveAt(Index); break; }
        }
    }
    Affinity=FMath::Clamp(Affinity+AffinityDelta,0,100);
    GetOwner()->ForceNetUpdate();
    return true;
}

void UDeliveryCompanionComponent::RecordRecruitMemory()
{
    if (Memories.ContainsByPredicate([](const FDeliveryNpcCompanionMemory& Memory){ return Memory.Kind==TEXT("FirstHelp"); })) return;
    RecordMemory(FGuid::NewGuid(),TEXT("FirstHelp"),TEXT("现在这位主人亲手扶起了我，我认他为主人，准备陪他一起跑单"),0);
}

bool UDeliveryCompanionComponent::RecordAssistedRecovery(FGuid DownEpisode,APawn* Helper)
{
    auto* Npc=GetCompanion();
    const auto* PC=IsValid(Helper)?Cast<APlayerController>(Helper->GetController()):nullptr;
    if (!IsValid(Npc) || !Npc->HasAuthority() || !PC || PC->GetPawn()!=Helper || !CanCommand(PC->PlayerState) ||
        Npc->GetEncounterState().Phase!=EDeliveryCompanionEncounterPhase::Joined ||
        !Npc->Combat->WasRecoveryAssistedBy(DownEpisode,Helper)) return false;
    const int32 NewCount=AssistedRecoveryCount<MAX_int32?AssistedRecoveryCount+1:MAX_int32;
    const FString Description=FString::Printf(TEXT("我这次倒地后，主人亲手扶我恢复站立；截至这次，认主后主人一共成功扶起过我 %d 次"),NewCount);
    if (!RecordMemory(DownEpisode,TEXT("HelpedUp"),Description,GetTuning().HelpUpAffinity)) return false;
    AssistedRecoveryCount=NewCount;
    // 与长期记忆使用同一真实完成编号，起身开始、被打断或自动恢复都不产生这条事实
    FDeliveryNpcObservation Fact;
    Fact.EventId=DownEpisode; Fact.Kind=TEXT("CompanionHelpedUp"); Fact.Subject=Helper;
    Fact.Source=EDeliveryNpcEvidenceSource::ActionResult; Fact.Description=Description; Fact.Lifetime=45;
    // Combat 随后发布原 Recovered 事件并唤醒模型，一次请求会同时看到这两条事实
    Npc->Agent->ObserveEvent(Fact);
    return true;
}

bool UDeliveryCompanionComponent::RecordDeliveryMemory(UDeliveryTaskDefinition* Task,APlayerState* Deliverer)
{
    const auto* Manager=UDeliveryTaskManagerComponent::Get(this);
    if (!GetOwner()->HasAuthority() || !Task || Deliverer!=OwnerPlayerState || RememberedDeliveries.Contains(Task) ||
        !Manager || Manager->GetTaskStatus(Task)!=EDeliveryTaskStatus::Completed) return false;
    const auto* Package=GetCarriedTaskPackage();
    const bool bDeliveredByCompanion=Package && Package->GetDeliveryItemComponent()->OwningTask==Task;
    auto* OwnerPawn=GetOwnerPawn();
    const bool bNearby=OwnerPawn && FVector::DistSquared(OwnerPawn->GetActorLocation(),GetOwner()->GetActorLocation())<=FMath::Square(900.f) &&
        DeliveryNpcConversationTrace::HasClearPath(GetOwner(),OwnerPawn);
    if (!bDeliveredByCompanion && !bNearby) return false;
    const int32 NewCount=DeliveryTogetherCount<MAX_int32?DeliveryTogetherCount+1:MAX_int32;
    const FString Description=bDeliveredByCompanion?
        FString::Printf(TEXT("我替主人把订单 %s 的真实包裹送到了，交付和奖励已经由游戏确认；截至这次，我们累计一起完成了 %d 单"),*Task->TaskId.ToString(),NewCount):
        FString::Printf(TEXT("主人完成了订单 %s，我当时就在旁边陪着，交付和奖励已经由游戏确认；截至这次，我们累计一起完成了 %d 单"),*Task->TaskId.ToString(),NewCount);
    if (!RecordMemory(FGuid::NewGuid(),TEXT("DeliveryTogether"),Description,bDeliveredByCompanion?
        GetTuning().CompanionDeliveryAffinity:GetTuning().DeliveryTogetherAffinity)) return false;
    RememberedDeliveries.Add(Task);
    DeliveryTogetherCount=NewCount;
    return true;
}

void UDeliveryCompanionComponent::RecordOwnerPunch(FGuid PunchId,bool bWasDown)
{
    const int32 NewCount=OwnerPunchCount<MAX_int32?OwnerPunchCount+1:MAX_int32;
    const int32 NewDownCount=bWasDown && OwnerDownPunchCount<MAX_int32?OwnerDownPunchCount+1:OwnerDownPunchCount;
    const FString Description=FString::Printf(TEXT("%s；我没有还手，仍然跟着主人。截至这次，主人累计实际打中我 %d 拳，其中倒地后 %d 拳"),
        bWasDown?TEXT("我已经倒地，主人又实际打中了我"):TEXT("主人实际打中了我"),NewCount,NewDownCount);
    if (RecordMemory(PunchId,bWasDown?TEXT("OwnerHitWhileDown"):TEXT("OwnerHit"),Description,
        -(bWasDown?GetTuning().OwnerDownHitAffinityLoss:GetTuning().OwnerHitAffinityLoss)))
    { OwnerPunchCount=NewCount; OwnerDownPunchCount=NewDownCount; }
}

void UDeliveryCompanionComponent::RecordOwnerVehicleHit(AActor* Source)
{
    const auto* Bike=Cast<ADeliveryMotorbike>(Source);
    const auto* Driver=Bike?Bike->GetDriver():nullptr;
    const auto* Npc=GetCompanion();
    if (!Driver || Driver->GetTaskPlayerState()!=OwnerPlayerState || !Npc || !Npc->Combat->IsDown()) return;
    const FGuid DownEpisode=Npc->Combat->GetDownEpisodeId();
    if (!DownEpisode.IsValid() || RememberedVehicleKnockdowns.Contains(DownEpisode)) return;
    const int32 NewCount=OwnerVehicleHitCount<MAX_int32?OwnerVehicleHitCount+1:MAX_int32;
    if (RecordMemory(FGuid::NewGuid(),TEXT("OwnerVehicleHit"),
        FString::Printf(TEXT("主人开车实际把我撞倒了，我没有离开，也没有换主人；截至这次，主人累计开车撞倒过我 %d 次"),NewCount),
        -GetTuning().OwnerVehicleHitAffinityLoss))
    { RememberedVehicleKnockdowns.Add(DownEpisode); OwnerVehicleHitCount=NewCount; }
}

void UDeliveryCompanionComponent::ClearCompanionMemories()
{
    Memories.Reset(); RecordedMemoryEvents.Reset(); RememberedDeliveries.Reset(); RememberedVehicleKnockdowns.Reset();
    bReportedMemoryLimit=false;
    AssistedRecoveryCount=0; DeliveryTogetherCount=0; OwnerPunchCount=0; OwnerDownPunchCount=0; OwnerVehicleHitCount=0;
    bCaterpillarOffered=false; bCaterpillarGiven=false; bGivingCaterpillar=false;
    bAnnouncedCaterpillarOpportunity=false;
    CaterpillarOpportunityId.Invalidate(); CaterpillarOpportunityExpiresAt=0; NextCaterpillarOpportunityAt=0;
}
