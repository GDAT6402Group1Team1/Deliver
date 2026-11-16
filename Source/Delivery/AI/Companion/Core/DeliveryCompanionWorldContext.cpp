#include "AI/Companion/Core/DeliveryCompanionComponent.h"
#include "AI/Companion/Core/DeliveryCompanionCharacter.h"
#include "AI/Characters/DeliveryNpcProfile.h"
#include "AI/Conversation/DeliveryConversationTurnSubsystem.h"
#include "World/Location/DeliveryLocationComponent.h"
#include "World/Location/DeliveryLocationRegistry.h"
#include "AIController.h"
#include "AIHelpers.h"
#include "Perception/AIPerceptionComponent.h"
#include "Perception/AISenseConfig_Sight.h"
#include "Perception/AISense_Sight.h"
#include "Engine/World.h"

namespace
{
constexpr int32 MaxObservedPlaces=8;
constexpr int32 MaxPlaceSightQueries=32;
constexpr float MaxPlaceObservationRadius=3000;
constexpr int32 MaxRememberedNeighborLines=4;
const FName NeighborLineMemoryKind(TEXT("NeighborSaidDirectly"));

// ── 世界信息 ──
// 把公开地点、街坊对二狗说过的话等现场信息写进模型请求，只读当次世界，不缓存可见性

/// @brief 把文本里的控制字符换成空格，截到 Limit 个字后去掉首尾空白
FString PublicPlaceText(const FText& Value,int32 Limit)
{
    FString Result;
    for (TCHAR Character:Value.ToString())
    {
        Result.AppendChar(Character>=32 && Character!=127?Character:TCHAR(' '));
        if (Result.Len()>=Limit) break;
    }
    return Result.TrimStartAndEnd();
}

/// @brief 三个坐标分量都是有限数时返回 true
bool IsFinitePoint(const FVector& Point)
{
    return FMath::IsFinite(Point.X) && FMath::IsFinite(Point.Y) && FMath::IsFinite(Point.Z);
}

struct FObservedPlaceCandidate
{
    TWeakObjectPtr<UDeliveryLocationComponent> Location;
    FVector Anchor=FVector::ZeroVector;
    double DistanceSquared=0;
    FString SortKey;
};
}

/// @brief 当次读取本世界公开地点，不缓存可见性或创建新行动证据
void UDeliveryCompanionComponent::CollectObservedPlaces(TArray<FObservedPublicPlace>& Out) const
{
    Out.Reset();
    const auto* Npc=GetCompanion();
    UWorld* World=GetWorld();
    if (!IsValid(Npc) || !Npc->HasAuthority() || !World || Npc->GetWorld()!=World ||
        Npc->IsActorBeingDestroyed() || Npc->IsHidden() || Npc->IsKnockedDown()) return;
    const auto* Controller=Cast<AAIController>(Npc->GetController());
    const auto* Perception=Controller?Controller->GetAIPerceptionComponent():nullptr;
    const auto* Sight=Perception?Perception->GetSenseConfig<UAISenseConfig_Sight>():nullptr;
    const auto* Registry=World->GetSubsystem<UDeliveryLocationRegistry>();
    // 停用视觉或离开本世界时，本次上下文不能继续声称看见公开地点
    if (!Sight || !Registry || !IsValid(Controller) || Controller->GetPawn()!=Npc || Controller->GetWorld()!=World || Perception->GetWorld()!=World ||
        !Perception->IsRegistered() || !Perception->IsSenseEnabled(UAISense_Sight::StaticClass()) ||
        !FMath::IsFinite(Sight->SightRadius) || Sight->SightRadius<=0 ||
        !FMath::IsFinite(Sight->PeripheralVisionAngleDegrees) ||
        !FMath::IsFinite(Sight->PointOfViewBackwardOffset) || Sight->PointOfViewBackwardOffset<0 ||
        Sight->PointOfViewBackwardOffset>MaxPlaceObservationRadius ||
        !FMath::IsFinite(Sight->NearClippingRadius) || Sight->NearClippingRadius<0 ||
        Sight->NearClippingRadius>2*MaxPlaceObservationRadius) return;
    const float Radius=FMath::Min(Sight->SightRadius,MaxPlaceObservationRadius);
    FVector Eye=FVector::ZeroVector;
    FVector Direction=FVector::ZeroVector;
    Perception->GetLocationAndDirection(Eye,Direction);
    if (!IsFinitePoint(Eye) || !IsFinitePoint(Direction) || !Direction.Normalize()) return;
    const float PeripheralCos=FMath::Cos(FMath::DegreesToRadians(FMath::Clamp(Sight->PeripheralVisionAngleDegrees,0.f,180.f)));
    const float FarRadiusSquared=FMath::Square(Radius+Sight->PointOfViewBackwardOffset);

    TArray<FObservedPlaceCandidate> Nearby;
    for (FName RegisteredId:Registry->GetRegisteredIds())
    {
        for (auto* Location:Registry->ResolveAllComponents(RegisteredId))
        {
            const auto* PlaceActor=IsValid(Location)?Location->GetOwner():nullptr;
            if (!IsValid(PlaceActor) || PlaceActor==Npc || PlaceActor->IsHidden() || PlaceActor->IsActorBeingDestroyed() ||
                Location->GetWorld()!=World || PlaceActor->GetWorld()!=World || !Location->IsRegistered() ||
                Location->LocationId!=RegisteredId || !Location->bDescribeToNpcs ||
                PublicPlaceText(Location->NpcDisplayName,64).IsEmpty()) continue;
            const FTransform ObservationTransform=Location->GetComponentTransform();
            if (!IsFinitePoint(Location->NpcObservationOffset) || ObservationTransform.ContainsNaN() ||
                !ObservationTransform.GetRotation().IsNormalized()) continue;
            const FVector Anchor=ObservationTransform.TransformPosition(Location->NpcObservationOffset);
            if (!IsFinitePoint(Anchor)) continue;
            const double DistanceSquared=FVector::DistSquared(Eye,Anchor);
            if (!FMath::IsFinite(DistanceSquared) || DistanceSquared>FMath::Square(Radius)) continue;
            // 复用 UE 当前视锥定义，不用登记过地点代替这次确实朝它看见
            if (!FAISystem::CheckIsTargetInSightCone(Eye,Direction,PeripheralCos,Sight->PointOfViewBackwardOffset,
                FMath::Square(Sight->NearClippingRadius),FarRadiusSquared,Anchor)) continue;
            FObservedPlaceCandidate Candidate;
            Candidate.Location=Location;
            Candidate.Anchor=Anchor;
            Candidate.DistanceSquared=DistanceSquared;
            Candidate.SortKey=RegisteredId.ToString()+TEXT("|")+Location->GetPathName();
            Nearby.Add(MoveTemp(Candidate));
        }
    }
    Nearby.Sort([](const FObservedPlaceCandidate& A,const FObservedPlaceCandidate& B)
    {
        if (A.DistanceSquared!=B.DistanceSquared) return A.DistanceSquared<B.DistanceSquared;
        return A.SortKey<B.SortKey;
    });

    // 公开观察点应在可见空处，连终点命中也拒绝，不忽略建筑或门
    const FCollisionQueryParams Params(SCENE_QUERY_STAT(CompanionObservedPlace),false,Npc);
    const int32 QueryCount=FMath::Min(Nearby.Num(),MaxPlaceSightQueries);
    for (int32 Index=0;Index<QueryCount && Out.Num()<MaxObservedPlaces;++Index)
    {
        const auto& Candidate=Nearby[Index];
        const auto* Location=Candidate.Location.Get();
        if (!Location) continue;
        FHitResult Hit;
        if (World->LineTraceSingleByChannel(Hit,Eye,Candidate.Anchor,ECC_Visibility,Params)) continue;
        FObservedPublicPlace Place;
        Place.Location=Candidate.Location;
        Place.View.PlaceId=FString::Printf(TEXT("place_%d"),Out.Num());
        Place.View.Name=PublicPlaceText(Location->NpcDisplayName,64);
        Place.View.Description=PublicPlaceText(Location->NpcDescription,160);
        Place.View.DistanceCm=static_cast<float>(FMath::Sqrt(Candidate.DistanceSquared));
        Out.Add(MoveTemp(Place));
    }
}

/// @brief 调用 CollectObservedPlaces 收集此刻看得到的公开地点，写进模型请求
void UDeliveryCompanionComponent::ExtendWorldDecisionRequest(FDeliveryNpcDecisionRequest& Request) const
{
    TArray<FObservedPublicPlace> Observed;
    CollectObservedPlaces(Observed);
    Request.CompanionObservedPlaces.Reset();
    for (auto& Place:Observed) Request.CompanionObservedPlaces.Add(MoveTemp(Place.View));
}

/// @brief 只保存服务器直连已送达二狗的街坊话语，原话仍是说话人的说法
void UDeliveryCompanionComponent::RecordDeliveredNeighborLine(const FDeliveryNpcMessageHandle& Handle)
{
    const auto* Npc=GetCompanion();
    const auto* World=GetWorld();
    if (!IsValid(Npc) || !Npc->HasAuthority() || !World || Npc->GetWorld()!=World || Npc->IsHidden() || Npc->IsKnockedDown() ||
        Npc->GetEncounterState().Phase!=EDeliveryCompanionEncounterPhase::Joined || !CanCommand(OwnerPlayerState) ||
        RecordedMemoryEvents.Contains(Handle.MessageId)) return;
    const auto* Turns=World->GetSubsystem<UDeliveryConversationTurnSubsystem>();
    FDeliveryNpcMessage Message;
    // 重新取已送达的原记录，核对同一世界代次和接收者，预备消息不能写进当局记忆
    if (!Turns || !Turns->GetNpcMessage(Handle,Npc,Message)) return;
    auto* Speaker=Message.Sender.Get();
    if (!IsValid(Speaker) || Speaker==Npc || Speaker->GetWorld()!=World || Speaker->IsHidden() ||
        Speaker->IsA<ADeliveryCompanionCharacter>() || !Speaker->Profile.DataTable || Speaker->Profile.RowName.IsNone()) return;
    const auto* Profile=Speaker->Profile.DataTable->FindRow<FDeliveryNpcProfileRow>(Speaker->Profile.RowName,TEXT("CompanionNeighborLine"),false);
    if (!Profile || Profile->DisplayName.IsEmpty()) return;
    const FString Name=PublicPlaceText(Speaker->DisplayName,48);
    if (Name.IsEmpty() || Name==TEXT("居民")) return;

    const FString Prefix=FString::Printf(TEXT("街坊%s当时直接对我说：「"),*Name);
    const FString Suffix=TEXT("」；这是他的说法，内容未经证实");
    FString Quote=Message.Text.Left(256-Prefix.Len()-Suffix.Len());
    // 按记忆长度截取时保留完整的 UTF-16 代理对
    if (!Quote.IsEmpty() && Quote[Quote.Len()-1]>=0xD800 && Quote[Quote.Len()-1]<=0xDBFF) Quote.LeftChopInline(1);
    for (TCHAR& Character:Quote)
        if (Character<32 || Character==127) Character=TEXT(' ');
    Quote.TrimStartAndEndInline();
    if (Quote.IsEmpty()) return;

    const auto FindMemory=[this](FGuid MessageId)
    {
        return Memories.IndexOfByPredicate([MessageId](const FDeliveryNpcCompanionMemory& Memory)
        { return Memory.EventId==MessageId && Memory.Kind==NeighborLineMemoryKind; });
    };
    RememberedNeighborLines.RemoveAll([&](const FRememberedNeighborLine& Line){ return FindMemory(Line.MessageId)==INDEX_NONE; });
    int32 ReplaceIndex=RememberedNeighborLines.IndexOfByPredicate([Speaker](const FRememberedNeighborLine& Line)
    { return Line.Speaker.Get()==Speaker; });
    if (ReplaceIndex==INDEX_NONE && RememberedNeighborLines.Num()>=MaxRememberedNeighborLines) ReplaceIndex=0;
    FDeliveryNpcCompanionMemory Previous;
    int32 PreviousIndex=INDEX_NONE;
    if (ReplaceIndex!=INDEX_NONE)
    {
        PreviousIndex=FindMemory(RememberedNeighborLines[ReplaceIndex].MessageId);
        Previous=MoveTemp(Memories[PreviousIndex]);
        Memories.RemoveAt(PreviousIndex);
    }
    // 同一街坊只换最近一句，替换失败保留旧记录；真实消息编号沿原当局规则去重
    if (!RecordMemory(Handle.MessageId,NeighborLineMemoryKind,Prefix+Quote+Suffix,0))
    {
        if (PreviousIndex!=INDEX_NONE) Memories.Insert(MoveTemp(Previous),PreviousIndex);
        return;
    }
    if (ReplaceIndex!=INDEX_NONE) RememberedNeighborLines.RemoveAt(ReplaceIndex);
    RememberedNeighborLines.Add({Speaker,Handle.MessageId});
}
