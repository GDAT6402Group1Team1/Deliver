#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "DeliveryStartupFlow.generated.h"

class APlayerController;
class APlayerStart;
class ULevelStreaming;
class UDeliveryStartupWidget;

UENUM(BlueprintType)
enum class EDeliveryStartupPhase : uint8
{
    Loading,
    Ready,
    WaitingForPlayers,
    Starting,
    Playing,
    Failed
};

DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FDeliveryStartupChanged, EDeliveryStartupPhase, Phase);
DECLARE_DYNAMIC_MULTICAST_DELEGATE(FDeliveryStartupEvent);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FDeliveryStartupFailed, FText, Reason);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FDeliveryStartupProgressChanged, float, Progress);

// 启动关卡的世界服务，管理各端加载和服务器开局，HUD 负责菜单布局与输入
UCLASS()
class DELIVERY_API ADeliveryStartupFlow : public AActor
{
    GENERATED_BODY()
public:
    ADeliveryStartupFlow();
    virtual void Tick(float DeltaSeconds) override;
    virtual void GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const override;

    UFUNCTION(BlueprintPure, Category="Delivery|Startup", meta=(WorldContext="WorldContextObject"))
    static ADeliveryStartupFlow* GetDeliveryStartupFlow(const UObject* WorldContextObject);

    // 开始请求被接收不代表立即开局，加载完成和所有连接玩家就绪后才执行
    UFUNCTION(BlueprintCallable, Category="Delivery|Startup")
    bool RequestStartGame(APlayerController* Player);

    UFUNCTION(BlueprintCallable, Category="Delivery|Startup")
    bool RetryLoading();

    UFUNCTION(BlueprintPure, Category="Delivery|Startup")
    EDeliveryStartupPhase GetPhase() const { return Phase; }
    UFUNCTION(BlueprintPure, Category="Delivery|Startup")
    bool IsSceneReady() const { return bLocalSceneReady; }
    UFUNCTION(BlueprintPure, Category="Delivery|Startup")
    bool IsStartRequested() const { return bStartRequested; }
    UFUNCTION(BlueprintPure, Category="Delivery|Startup")
    bool HasGameStarted() const { return bGameStarted; }
    UFUNCTION(BlueprintPure, Category="Delivery|Startup")
    FText GetLoadingError() const;
    // 本机显示用的估算值 0~1，未就绪最多 0.9，不能用于决定开局
    UFUNCTION(BlueprintPure, Category="Delivery|Startup")
    float GetLoadingProgress() const { return LoadingProgress; }

    UPROPERTY(BlueprintAssignable, Category="Delivery|Startup")
    FDeliveryStartupChanged OnPhaseChanged;
    UPROPERTY(BlueprintAssignable, Category="Delivery|Startup")
    FDeliveryStartupEvent OnSceneReady;
    UPROPERTY(BlueprintAssignable, Category="Delivery|Startup")
    FDeliveryStartupEvent OnGameStarted;
    UPROPERTY(BlueprintAssignable, Category="Delivery|Startup")
    FDeliveryStartupFailed OnLoadingFailed;
    UPROPERTY(BlueprintAssignable, Category="Delivery|Startup")
    FDeliveryStartupProgressChanged OnLoadingProgressChanged;

    // 此关卡必须已作为 Blueprint Streaming 子关卡加入启动地图，不同步加载软引用
    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category="Delivery|Startup")
    TSoftObjectPtr<UWorld> GameplayLevel;
    // 用户制作的 UMG 继承 DeliveryStartupWidget，留空时只运行后台接口
    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category="Delivery|Startup")
    TSoftClassPtr<UDeliveryStartupWidget> MenuWidgetClass;
    UPROPERTY(EditAnywhere, Category="Delivery|Startup", meta=(ClampMin="10", Units="s"))
    float LoadTimeoutSeconds = 180.f;
    // 估算进度从零缓动到 90% 的时间，真实就绪时立即到 100%，不会延迟开局
    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category="Delivery|Startup", meta=(ClampMin="0.1", Units="s"))
    float EstimatedLoadSeconds = 20.f;
    UPROPERTY(EditAnywhere, Category="Delivery|Startup", meta=(ClampMin="100", Units="cm"))
    float GroundTraceDistance = 3000.f;
    // 多人共用出生点时，在此半径内寻找有地面且不与角色或障碍重叠的位置
    UPROPERTY(EditAnywhere, Category="Delivery|Startup", meta=(ClampMin="0", Units="cm"))
    float SpawnSearchRadius = 400.f;
    UPROPERTY(EditAnywhere, Category="Delivery|Startup")
    FName PlayerStartTag;

    // 由拥有连接的 PC 转发，服务器不接收客户端指定的出生位置或任务数据
    void ReportPlayerReady(APlayerController* Player);
    bool CanSpawnPlayer(APlayerController* Player) const;
    APlayerStart* FindGameplayStart() const;

protected:
    virtual void BeginPlay() override;
    virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;

private:
    friend class FDeliveryStartupFlowTest;
    friend class FDeliveryStartupProgressTest;
    void BeginLoading();
    void PollLoading();
    void UpdateLoadingProgress();
    void SetLoadingProgress(float Progress);
    void UpdatePhase();
    void TryStartPlayers();
    bool HasSpawnGround() const;
    bool FindSpawnTransform(APlayerController* Player, FTransform& OutTransform) const;
    void Fail(const FText& Reason);
    void SetPhase(EDeliveryStartupPhase NewPhase);
    UFUNCTION()
    void OnLevelShown();
    UFUNCTION()
    void OnRep_Startup();

    UPROPERTY(ReplicatedUsing=OnRep_Startup)
    bool bStartRequested = false;
    UPROPERTY(ReplicatedUsing=OnRep_Startup)
    bool bGameStarted = false;
    UPROPERTY(ReplicatedUsing=OnRep_Startup)
    FText ServerError;
    UPROPERTY(Transient)
    TObjectPtr<ULevelStreaming> StreamingLevel;
    EDeliveryStartupPhase Phase = EDeliveryStartupPhase::Loading;
    FText LocalError;
    bool bLocalSceneReady = false;
    bool bSpawningPlayers = false;
    double LoadStartedAt = 0;
    // 每端独立估算，不复制主机进度给客户端
    float LoadingProgress = 0.f;
    TSet<TWeakObjectPtr<APlayerController>> ReadyPlayers;
    TSet<TWeakObjectPtr<APlayerController>> SpawnedPlayers;
    TSet<TWeakObjectPtr<APlayerController>> ReportedLocalPlayers;
};
