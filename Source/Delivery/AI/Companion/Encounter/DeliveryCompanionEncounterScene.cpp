#include "AI/Companion/Encounter/DeliveryCompanionEncounterScene.h"

#include "Components/BoxComponent.h"
#include "Components/SceneComponent.h"
#include "Components/SkeletalMeshComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/World.h"
#include "Net/UnrealNetwork.h"

// ── 初遇场景 ──
// 面馆门口的初遇场景 Actor：开关门、抛出路线和门口是否被挡住

/// @brief 开启复制、30 次每秒同步，创建门叶、门洞检测盒和几个定位点
ADeliveryCompanionEncounterScene::ADeliveryCompanionEncounterScene()
{
    bReplicates = true;
    SetReplicateMovement(false);
    SetNetUpdateFrequency(30.f);
    SetMinNetUpdateFrequency(30.f);
    PrimaryActorTick.bCanEverTick = true;
    PrimaryActorTick.bStartWithTickEnabled = false;

    SceneRoot = CreateDefaultSubobject<USceneComponent>(TEXT("SceneRoot"));
    SetRootComponent(SceneRoot);
    TriggerAnchor = CreateDefaultSubobject<USceneComponent>(TEXT("TriggerAnchor"));
    TriggerAnchor->SetupAttachment(SceneRoot);
    ThrowStart = CreateDefaultSubobject<USceneComponent>(TEXT("ThrowStart"));
    ThrowStart->SetupAttachment(SceneRoot);
    ThrowLanding = CreateDefaultSubobject<USceneComponent>(TEXT("ThrowLanding"));
    ThrowLanding->SetupAttachment(SceneRoot);

    DoorLeaf = CreateDefaultSubobject<UStaticMeshComponent>(TEXT("DoorLeaf"));
    DoorLeaf->SetupAttachment(SceneRoot);
    DoorLeaf->SetMobility(EComponentMobility::Movable);
    DoorLeaf->SetCollisionProfileName(TEXT("BlockAllDynamic"));
    DoorLeaf->SetGenerateOverlapEvents(false);
    DoorLeaf->SetCanEverAffectNavigation(false);

    NoodleOwner = CreateDefaultSubobject<USkeletalMeshComponent>(TEXT("NoodleOwner"));
    NoodleOwner->SetupAttachment(SceneRoot);
    NoodleOwner->SetCollisionEnabled(ECollisionEnabled::NoCollision);
    NoodleOwner->SetGenerateOverlapEvents(false);
    NoodleOwner->SetCanEverAffectNavigation(false);
    NoodleOwner->SetHiddenInGame(true);

    DoorwayClearance = CreateDefaultSubobject<UBoxComponent>(TEXT("DoorwayClearance"));
    DoorwayClearance->SetupAttachment(SceneRoot);
    DoorwayClearance->SetCollisionEnabled(ECollisionEnabled::NoCollision);
    DoorwayClearance->SetGenerateOverlapEvents(false);
    DoorwayClearance->SetCanEverAffectNavigation(false);
    DoorwayClearance->SetHiddenInGame(true);
    // 未在资产中配置门口范围时拒绝移动，防止默认盒子漏掉门叶经过的位置
    DoorwayClearance->SetBoxExtent(FVector::ZeroVector);
}

/// @brief 门口触发点的世界坐标
FVector ADeliveryCompanionEncounterScene::GetTriggerWorldLocation() const
{
    return TriggerAnchor->GetComponentLocation();
}

/// @brief 抛出起点的世界变换
FTransform ADeliveryCompanionEncounterScene::GetThrowStartTransform() const
{
    return ThrowStart->GetComponentTransform();
}

/// @brief 抛出起点的世界坐标
FVector ADeliveryCompanionEncounterScene::GetThrowStartWorldLocation() const
{
    return ThrowStart->GetComponentLocation();
}

/// @brief 抛出落点的世界坐标
FVector ADeliveryCompanionEncounterScene::GetThrowLandingWorldLocation() const
{
    return ThrowLanding->GetComponentLocation();
}

/// @brief 门叶、门洞检测盒、开门时长和蓝图里的门动画都配好时返回 true
bool ADeliveryCompanionEncounterScene::HasDoorConfiguration() const
{
    if (!IsValid(DoorLeaf) || !DoorLeaf->GetStaticMesh() || !DoorLeaf->IsQueryCollisionEnabled() ||
        DoorLeaf->GetCollisionResponseToChannel(ECC_Pawn) != ECR_Block || !IsValid(DoorwayClearance) ||
        !FMath::IsFinite(DoorTravelSeconds) || DoorTravelSeconds <= 0.f ||
        !GetClass()->IsFunctionImplementedInScript(GET_FUNCTION_NAME_CHECKED(ADeliveryCompanionEncounterScene, ApplyDoorPresentation)))
    {
        return false;
    }
    const FVector Extent = DoorwayClearance->GetScaledBoxExtent();
    return !Extent.ContainsNaN() && Extent.GetMin() > UE_SMALL_NUMBER;
}

/// @brief 只接收服务器请求；返回 true 表示接受目标，门被挡住时会等待，不表示已经开关到位
bool ADeliveryCompanionEncounterScene::RequestDoorOpen(bool bOpen)
{
    if (!HasAuthority() || IsActorBeingDestroyed() || !GetWorld() || !HasDoorConfiguration()) return false;

    bRequestedOpen = bOpen;
    const float Target = bOpen ? 1.f : 0.f;
    const bool bMoving = !FMath::IsNearlyEqual(DoorProgress, Target);
    if (!bMoving) SetDoorBlocked(false);
    SetActorTickEnabled(bMoving);
    ForceNetUpdate();
    return true;
}

/// @brief 门已经完全打开
bool ADeliveryCompanionEncounterScene::IsDoorOpen() const
{
    return DoorProgress >= 1.f - UE_KINDA_SMALL_NUMBER;
}

/// @brief 用实际门框和抛出方向判断玩家是否已经离开面馆内侧
bool ADeliveryCompanionEncounterScene::IsPastDoorway(const FVector& Position) const
{
    const FVector Outward=(GetThrowLandingWorldLocation()-GetThrowStartWorldLocation()).GetSafeNormal2D();
    const FVector FromDoor=Position-DoorwayClearance->GetComponentLocation();
    const FVector Extent=DoorwayClearance->GetScaledBoxExtent();
    const FVector LocalOutward=DoorwayClearance->GetComponentQuat().UnrotateVector(Outward);
    const double DoorwayDepth=FVector::DotProduct(Extent,LocalOutward.GetAbs());
    // 离开面馆附近也可以关门，以免从另一条路离开的玩家一直保留通行
    return FVector::DotProduct(Outward,FromDoor)>DoorwayDepth || FromDoor.SizeSquared2D()>FMath::Square(900.f);
}

/// @brief 客户端开始抛出演出前同时确认服务器进度和本机门叶表现，等待跨 Actor 复制到齐
bool ADeliveryCompanionEncounterScene::IsDoorPresentationOpen() const
{
    return HasActorBegunPlay() && IsDoorOpen() && DisplayedDoorProgress >= 1.f - UE_KINDA_SMALL_NUMBER;
}

/// @brief 门洞检测盒里有人、车或物理物体时返回 true，配置不全也按占用处理
bool ADeliveryCompanionEncounterScene::IsDoorwayOccupied() const
{
    const UWorld* World = GetWorld();
    if (!World || !HasDoorConfiguration()) return true;

    FCollisionObjectQueryParams Objects;
    Objects.AddObjectTypesToQuery(ECC_Pawn);
    Objects.AddObjectTypesToQuery(ECC_Vehicle);
    Objects.AddObjectTypesToQuery(ECC_PhysicsBody);
    Objects.AddObjectTypesToQuery(ECC_WorldDynamic);
    FCollisionQueryParams Query(SCENE_QUERY_STAT(CompanionDoorClearance), false, this);
    // 忽略本场景的门叶和老板，但二狗及其他玩家仍会阻挡门的转动
    return World->OverlapAnyTestByObjectType(DoorwayClearance->GetComponentLocation(),
        DoorwayClearance->GetComponentQuat(), Objects,
        FCollisionShape::MakeBox(DoorwayClearance->GetScaledBoxExtent()), Query);
}

/// @brief 写入门是否被挡并同步，没变化就不写
void ADeliveryCompanionEncounterScene::SetDoorBlocked(bool bBlocked)
{
    if (bDoorBlocked == bBlocked) return;
    bDoorBlocked = bBlocked;
    ForceNetUpdate();
}

/// @brief 服务器写入初遇被什么挡住并同步，再调用 OnRep_EncounterObstruction
void ADeliveryCompanionEncounterScene::SetEncounterObstruction(EDeliveryCompanionEncounterObstruction Obstruction)
{
    if (!HasAuthority() || EncounterObstruction==Obstruction) return;
    EncounterObstruction=Obstruction;
    ForceNetUpdate();
    OnRep_EncounterObstruction();
}

/// @brief 广播 OnEncounterObstructionChanged
void ADeliveryCompanionEncounterScene::OnRep_EncounterObstruction()
{
    OnEncounterObstructionChanged.Broadcast();
}

/// @brief 按当前门的进度摆好门叶，服务器门还没到目标位置时开启 Tick
void ADeliveryCompanionEncounterScene::BeginPlay()
{
    Super::BeginPlay();
    DisplayedDoorProgress = FMath::Clamp(DoorProgress, 0.f, 1.f);
    ApplyDoorPresentation(DisplayedDoorProgress);
    if (HasAuthority()) SetActorTickEnabled(!FMath::IsNearlyEqual(DoorProgress, bRequestedOpen ? 1.f : 0.f));
}

/// @brief 服务器推进门的进度，门洞有人时停住；客户端把显示进度平滑追到服务器进度
void ADeliveryCompanionEncounterScene::Tick(float DeltaSeconds)
{
    Super::Tick(DeltaSeconds);
    if (!FMath::IsFinite(DeltaSeconds) || DeltaSeconds <= 0.f) return;

    if (HasAuthority())
    {
        const float Target = bRequestedOpen ? 1.f : 0.f;
        if (FMath::IsNearlyEqual(DoorProgress, Target))
        {
            SetDoorBlocked(false);
            SetActorTickEnabled(false);
            return;
        }

        // 开门和关门都先检查整个转动范围，占用时保留当前角度并在下一帧重查
        SetDoorBlocked(IsDoorwayOccupied());
        if (bDoorBlocked) return;
        DoorProgress = FMath::FInterpConstantTo(DoorProgress, Target, DeltaSeconds,
            1.f / FMath::Max(.05f, DoorTravelSeconds));
        if (FMath::IsNearlyEqual(DoorProgress, Target)) DoorProgress = Target;
        DisplayedDoorProgress = DoorProgress;
        ApplyDoorPresentation(DoorProgress);
        if (FMath::IsNearlyEqual(DoorProgress, Target))
        {
            ForceNetUpdate();
            SetActorTickEnabled(false);
        }
        return;
    }

    // 客户端不推测门是否能继续移动，只追到服务器已经确认的角度
    DisplayedDoorProgress = FMath::FInterpConstantTo(DisplayedDoorProgress,
        FMath::Clamp(DoorProgress, 0.f, 1.f), DeltaSeconds,
        1.f / FMath::Max(.05f, DoorTravelSeconds));
    ApplyDoorPresentation(DisplayedDoorProgress);
    if (FMath::IsNearlyEqual(DisplayedDoorProgress, DoorProgress)) SetActorTickEnabled(false);
}

/// @brief 客户端收到门的进度：第一次或门已全开时直接对齐，否则开 Tick 平滑追上
void ADeliveryCompanionEncounterScene::OnRep_DoorState()
{
    // 初次收到状态直接对齐；已确认完全打开时结束尾部插值，避免本机门叶挡住随后到达的抛出阶段
    if (!bReceivedDoorState || IsDoorOpen())
    {
        bReceivedDoorState = true;
        DisplayedDoorProgress = FMath::Clamp(DoorProgress, 0.f, 1.f);
        if (HasActorBegunPlay()) ApplyDoorPresentation(DisplayedDoorProgress);
    }
    if (HasActorBegunPlay()) SetActorTickEnabled(!FMath::IsNearlyEqual(DisplayedDoorProgress, DoorProgress));
}

/// @brief 关掉 Tick
void ADeliveryCompanionEncounterScene::EndPlay(const EEndPlayReason::Type Reason)
{
    SetActorTickEnabled(false);
    Super::EndPlay(Reason);
}

/// @brief 复制门的进度、是否被挡和初遇阻挡状态
void ADeliveryCompanionEncounterScene::GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const
{
    Super::GetLifetimeReplicatedProps(OutLifetimeProps);
    DOREPLIFETIME(ADeliveryCompanionEncounterScene, DoorProgress);
    DOREPLIFETIME(ADeliveryCompanionEncounterScene, bDoorBlocked);
    DOREPLIFETIME(ADeliveryCompanionEncounterScene, EncounterObstruction);
}
