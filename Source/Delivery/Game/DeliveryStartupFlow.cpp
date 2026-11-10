#include "Game/DeliveryStartupFlow.h"

#include "Delivery.h"
#include "Components/PrimitiveComponent.h"
#include "Components/CapsuleComponent.h"
#include "Engine/Engine.h"
#include "Engine/LevelStreaming.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "GameFramework/GameModeBase.h"
#include "GameFramework/PlayerStart.h"
#include "Kismet/GameplayStatics.h"
#include "Net/UnrealNetwork.h"
#include "Player/DeliveryPlayerController.h"
#include "Task/DeliveryTaskManagerComponent.h"

ADeliveryStartupFlow::ADeliveryStartupFlow()
{
    PrimaryActorTick.bCanEverTick = true;
    PrimaryActorTick.TickInterval = 0.1f;
    bReplicates = true;
    bAlwaysRelevant = true;
    GameplayLevel = TSoftObjectPtr<UWorld>(FSoftObjectPath(TEXT("/Game/Level/TestForCharacter.TestForCharacter")));
}

ADeliveryStartupFlow* ADeliveryStartupFlow::GetDeliveryStartupFlow(const UObject* Context)
{
    UWorld* World = GEngine ? GEngine->GetWorldFromContextObject(Context, EGetWorldErrorMode::ReturnNull) : nullptr;
    if (World)
        for (TActorIterator<ADeliveryStartupFlow> It(World); It; ++It)
            if (IsValid(*It)) return *It;
    return nullptr;
}

void ADeliveryStartupFlow::GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const
{
    Super::GetLifetimeReplicatedProps(OutLifetimeProps);
    DOREPLIFETIME(ADeliveryStartupFlow, bStartRequested);
    DOREPLIFETIME(ADeliveryStartupFlow, bGameStarted);
    DOREPLIFETIME(ADeliveryStartupFlow, ServerError);
}

void ADeliveryStartupFlow::BeginPlay()
{
    Super::BeginPlay();
    if (GetNetMode() == NM_DedicatedServer) bStartRequested = true;
    BeginLoading();
}

void ADeliveryStartupFlow::BeginLoading()
{
    LoadStartedAt = FPlatformTime::Seconds();
    LocalError = FText::GetEmpty();
    SetLoadingProgress(bLocalSceneReady ? 1.f : 0.f);
    SetActorTickEnabled(true);
    const FString Package = GameplayLevel.ToSoftObjectPath().GetLongPackageName();
    StreamingLevel = Package.IsEmpty() ? nullptr : UGameplayStatics::GetStreamingLevel(this, FName(*Package));
    if (!StreamingLevel)
    {
        Fail(NSLOCTEXT("DeliveryStartup", "MissingLevel", "Gameplay level is not configured as a streaming sublevel."));
        return;
    }
    StreamingLevel->OnLevelShown.AddUniqueDynamic(this, &ADeliveryStartupFlow::OnLevelShown);
    StreamingLevel->bShouldBlockOnLoad = false;
    StreamingLevel->SetShouldBeLoaded(true);
    StreamingLevel->SetShouldBeVisible(true);
    UpdatePhase();
    UE_LOG(LogDelivery, Log, TEXT("[Startup] Loading %s"), *Package);
}

void ADeliveryStartupFlow::OnLevelShown()
{
    // 可见回调不代表出生地面已完成碰撞注册，下一次检查继续确认
    PollLoading();
}

APlayerStart* ADeliveryStartupFlow::FindGameplayStart() const
{
    if (!StreamingLevel || !StreamingLevel->IsLevelVisible()) return nullptr;
    for (TActorIterator<APlayerStart> It(GetWorld()); It; ++It)
        if (It->GetLevel() == StreamingLevel->GetLoadedLevel()
            && (PlayerStartTag.IsNone() || It->PlayerStartTag == PlayerStartTag)) return *It;
    return nullptr;
}

bool ADeliveryStartupFlow::HasSpawnGround() const
{
    APlayerStart* Start = FindGameplayStart();
    if (!Start) return false;
    FHitResult Hit;
    FCollisionObjectQueryParams Objects(ECC_WorldStatic);
    FCollisionQueryParams Query(SCENE_QUERY_STAT(DeliveryStartupGround), false, Start);
    const FVector Origin = Start->GetActorLocation();
    return GetWorld()->LineTraceSingleByObjectType(Hit, Origin,
        Origin - FVector(0, 0, GroundTraceDistance), Objects, Query) && Hit.bBlockingHit
        && Hit.GetComponent() && Hit.GetComponent()->GetCollisionResponseToChannel(ECC_Pawn) == ECR_Block;
}

bool ADeliveryStartupFlow::FindSpawnTransform(APlayerController* Player, FTransform& OutTransform) const
{
    AGameModeBase* Mode = GetWorld()->GetAuthGameMode();
    UClass* PawnClass = Mode ? Mode->GetDefaultPawnClassForController(Player) : nullptr;
    const APawn* PawnTemplate = PawnClass ? PawnClass->GetDefaultObject<APawn>() : nullptr;
    if (!PawnTemplate) return false;
    // 默认对象的组件未注册，GetSimpleCollisionCylinder 在打包游戏里可能返回零
    const auto* Capsule = Cast<UCapsuleComponent>(PawnTemplate->GetRootComponent());
    if (!Capsule) return false;
    float Radius, HalfHeight;
    Capsule->GetScaledCapsuleSize(Radius, HalfHeight);
    const float Spacing = FMath::Max(200.f, Radius * 2.f + 50.f);
    for (TActorIterator<APlayerStart> It(GetWorld()); It; ++It)
    {
        APlayerStart* Start = *It;
        if (Start->GetLevel() != StreamingLevel->GetLoadedLevel()
            || (!PlayerStartTag.IsNone() && Start->PlayerStartTag != PlayerStartTag)) continue;
        const FVector Origin = Start->GetActorLocation();
        const FRotator Rotation = Start->GetActorRotation();
        // 优先原点，其次两个八方向圆环；最多检查 17 个位置，不无限搜索整张地图
        for (int32 Index = 0; Index < 17; ++Index)
        {
            FVector Candidate = Origin;
            if (Index > 0)
            {
                const float Distance = Spacing * (1 + (Index - 1) / 8);
                if (Distance > SpawnSearchRadius) continue;
                const float Angle = (Index - 1) % 8 * UE_PI / 4.f;
                Candidate += FVector(FMath::Cos(Angle), FMath::Sin(Angle), 0) * Distance;
            }
            FHitResult Ground;
            FCollisionQueryParams Query(SCENE_QUERY_STAT(DeliveryStartupSpawn), false, Start);
            if (!GetWorld()->LineTraceSingleByObjectType(Ground, Candidate + FVector(0, 0, HalfHeight),
                Candidate - FVector(0, 0, GroundTraceDistance), FCollisionObjectQueryParams(ECC_WorldStatic), Query)
                || !Ground.bBlockingHit || !Ground.GetComponent()
                || Ground.GetComponent()->GetCollisionResponseToChannel(ECC_Pawn) != ECR_Block
                || Ground.ImpactNormal.Z < 0.7f) continue;
            if (Index > 0) Candidate.Z = Ground.ImpactPoint.Z + HalfHeight + 2.f;
            if (GetWorld()->EncroachingBlockingGeometry(PawnTemplate, Candidate, Rotation)) continue;
            OutTransform = FTransform(Rotation, Candidate);
            return true;
        }
    }
    return false;
}

void ADeliveryStartupFlow::PollLoading()
{
    if (!GetLoadingError().IsEmpty()) { UpdatePhase(); return; }
    if (!bLocalSceneReady && StreamingLevel && StreamingLevel->IsLevelLoaded()
        && StreamingLevel->IsLevelVisible() && HasSpawnGround())
    {
        bLocalSceneReady = true;
        SetLoadingProgress(1.f);
        UE_LOG(LogDelivery, Log, TEXT("[Startup] Scene ready, ground collision confirmed"));
        OnSceneReady.Broadcast();
    }
    if (!bLocalSceneReady && FPlatformTime::Seconds() - LoadStartedAt > LoadTimeoutSeconds)
    {
        Fail(NSLOCTEXT("DeliveryStartup", "Timeout", "Loading timed out. Check the gameplay level, PlayerStart and ground collision."));
        return;
    }
    UpdateLoadingProgress();
    if (bLocalSceneReady)
    {
        for (FConstPlayerControllerIterator It = GetWorld()->GetPlayerControllerIterator(); It; ++It)
        {
            auto* Player = Cast<ADeliveryPlayerController>(It->Get());
            if (!Player || !Player->IsLocalController() || ReportedLocalPlayers.Contains(Player)) continue;
            if (HasAuthority()) ReportPlayerReady(Player);
            else Player->ServerReportStartupReady(this);
            ReportedLocalPlayers.Add(Player);
        }
        if (HasAuthority()) TryStartPlayers();
    }
    UpdatePhase();
}

void ADeliveryStartupFlow::Tick(float DeltaSeconds)
{
    Super::Tick(DeltaSeconds);
    if (LoadStartedAt > 0) PollLoading();
}

void ADeliveryStartupFlow::UpdateLoadingProgress()
{
    if (LoadStartedAt <= 0 || !GetLoadingError().IsEmpty()) return;
    if (bLocalSceneReady) { SetLoadingProgress(1.f); return; }
    const float Elapsed = static_cast<float>(FPlatformTime::Seconds() - LoadStartedAt);
    const float Alpha = FMath::Clamp(Elapsed / FMath::Max(EstimatedLoadSeconds, 0.1f), 0.f, 1.f);
    const float Remaining = 1.f - Alpha;
    // 三次缓出让末段逐渐放慢，加载期间调大预计耗时也不让条倒退
    const float Estimate = 0.9f * (1.f - Remaining * Remaining * Remaining);
    SetLoadingProgress(FMath::Max(LoadingProgress, Estimate));
}

void ADeliveryStartupFlow::SetLoadingProgress(float Progress)
{
    Progress = FMath::Clamp(Progress, 0.f, 1.f);
    if (FMath::IsNearlyEqual(LoadingProgress, Progress, 0.00001f)) return;
    LoadingProgress = Progress;
    OnLoadingProgressChanged.Broadcast(LoadingProgress);
}

bool ADeliveryStartupFlow::RequestStartGame(APlayerController* Player)
{
    // 本地单人或监听主机决定开局，远端玩家只报告加载就绪
    if (!HasAuthority() || !Player || Player->GetWorld() != GetWorld()
        || !Player->IsLocalController() || !GetLoadingError().IsEmpty()) return false;
    if (bGameStarted || bStartRequested) return true;
    bStartRequested = true;
    ForceNetUpdate();
    if (LoadStartedAt > 0) PollLoading();
    return true;
}

void ADeliveryStartupFlow::ReportPlayerReady(APlayerController* Player)
{
    if (HasAuthority() && IsValid(Player) && Player->GetWorld() == GetWorld())
        ReadyPlayers.Add(Player);
}

bool ADeliveryStartupFlow::CanSpawnPlayer(APlayerController* Player) const
{
    return (bGameStarted || bSpawningPlayers) && bLocalSceneReady && ReadyPlayers.Contains(Player);
}

void ADeliveryStartupFlow::TryStartPlayers()
{
    if (!bStartRequested || !GetLoadingError().IsEmpty() || bSpawningPlayers) return;
    TArray<APlayerController*> Players;
    for (FConstPlayerControllerIterator It = GetWorld()->GetPlayerControllerIterator(); It; ++It)
    {
        APlayerController* Player = It->Get();
        if (!IsValid(Player)) continue;
        if (!bGameStarted && !ReadyPlayers.Contains(Player)) return;
        if (ReadyPlayers.Contains(Player) && !SpawnedPlayers.Contains(Player)) Players.Add(Player);
    }
    if (Players.IsEmpty()) return;
    AGameModeBase* Mode = GetWorld()->GetAuthGameMode();
    APlayerStart* Start = FindGameplayStart();
    if (!Mode || !Start) return;
    bSpawningPlayers = true;
    UpdatePhase();
    bool bFailed = false;
    TArray<APlayerController*> CreatedPlayers;
    for (APlayerController* Player : Players)
    {
        if (!Player->GetPawn())
        {
            FTransform SpawnTransform;
            if (!FindSpawnTransform(Player, SpawnTransform)) { bFailed = true; break; }
            Mode->RestartPlayerAtTransform(Player, SpawnTransform);
            if (Player->GetPawn()) CreatedPlayers.Add(Player);
        }
        if (!Player->GetPawn()) { bFailed = true; break; }
        SpawnedPlayers.Add(Player);
    }
    bSpawningPlayers = false;
    if (bFailed)
    {
        // 首次开局部分生成失败时回收本次角色，避免菜单下已有玩家开始物理模拟
        if (!bGameStarted)
            for (APlayerController* Player : CreatedPlayers)
            {
                if (APawn* Pawn = Player->GetPawn()) { Player->UnPossess(); Pawn->Destroy(); }
                SpawnedPlayers.Remove(Player);
            }
        Fail(NSLOCTEXT("DeliveryStartup", "SpawnFailed", "Player could not spawn. Check the PlayerStart clearance and pawn class."));
        return;
    }
    if (!bGameStarted)
    {
        bGameStarted = true;
        ForceNetUpdate();
        if (auto* Tasks = UDeliveryTaskManagerComponent::Get(this)) Tasks->StartGameplay();
        UE_LOG(LogDelivery, Log, TEXT("[Startup] Game started; task clock released"));
    }
}

FText ADeliveryStartupFlow::GetLoadingError() const
{
    return ServerError.IsEmpty() ? LocalError : ServerError;
}

void ADeliveryStartupFlow::Fail(const FText& Reason)
{
    LocalError = Reason;
    if (HasAuthority()) { ServerError = Reason; ForceNetUpdate(); }
    UE_LOG(LogDelivery, Warning, TEXT("[Startup] %s"), *Reason.ToString());
    UpdatePhase();
}

bool ADeliveryStartupFlow::RetryLoading()
{
    if (bGameStarted || GetLoadingError().IsEmpty()) return false;
    if (!HasAuthority() && !ServerError.IsEmpty()) return false;
    if (HasAuthority()) { ServerError = FText::GetEmpty(); ForceNetUpdate(); }
    BeginLoading();
    return true;
}

void ADeliveryStartupFlow::UpdatePhase()
{
    // 正式开局后角色销毁或换 Pawn 属于玩法，不能重新打开开始菜单
    if (Phase == EDeliveryStartupPhase::Playing) return;
    if (!GetLoadingError().IsEmpty()) { SetPhase(EDeliveryStartupPhase::Failed); return; }
    if (!bLocalSceneReady) { SetPhase(EDeliveryStartupPhase::Loading); return; }
    if (bSpawningPlayers) { SetPhase(EDeliveryStartupPhase::Starting); return; }
    if (bGameStarted)
    {
        // 客户端可能先收到开始标记，后收到自己的 Pawn，菜单保留到控制对象到位
        for (FConstPlayerControllerIterator It = GetWorld()->GetPlayerControllerIterator(); It; ++It)
            if (It->IsValid() && It->Get()->IsLocalController() && !It->Get()->GetPawn())
            { SetPhase(EDeliveryStartupPhase::Starting); return; }
        SetPhase(EDeliveryStartupPhase::Playing);
    }
    else SetPhase(bStartRequested ? EDeliveryStartupPhase::WaitingForPlayers : EDeliveryStartupPhase::Ready);
}

void ADeliveryStartupFlow::SetPhase(EDeliveryStartupPhase NewPhase)
{
    if (Phase == NewPhase) return;
    Phase = NewPhase;
    OnPhaseChanged.Broadcast(Phase);
    if (Phase == EDeliveryStartupPhase::Playing) OnGameStarted.Broadcast();
    if (Phase == EDeliveryStartupPhase::Failed) OnLoadingFailed.Broadcast(GetLoadingError());
}

void ADeliveryStartupFlow::OnRep_Startup() { UpdatePhase(); }

void ADeliveryStartupFlow::EndPlay(const EEndPlayReason::Type Reason)
{
    SetActorTickEnabled(false);
    if (StreamingLevel) StreamingLevel->OnLevelShown.RemoveDynamic(this, &ADeliveryStartupFlow::OnLevelShown);
    StreamingLevel = nullptr;
    ReadyPlayers.Reset();
    SpawnedPlayers.Reset();
    ReportedLocalPlayers.Reset();
    Super::EndPlay(Reason);
}
