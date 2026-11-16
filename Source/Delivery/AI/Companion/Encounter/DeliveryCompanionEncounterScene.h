#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "DeliveryCompanionEncounterScene.generated.h"

class UBoxComponent;
class USceneComponent;
class USkeletalMeshComponent;
class UStaticMeshComponent;

/**
 * @brief 初遇被什么挡住：没有、门洞有人、抛出路线被挡
 */
UENUM(BlueprintType)
enum class EDeliveryCompanionEncounterObstruction : uint8 { None, Doorway, EjectionPath };

/**
 * @brief 保存面馆初遇的固定锚点和门状态，二狗角色负责自身阶段与移动，蓝图配置门和老板的表现
 */
UCLASS()
class DELIVERY_API ADeliveryCompanionEncounterScene : public AActor
{
    GENERATED_BODY()

public:
    /// @brief 开启复制、30 次每秒同步，创建门叶、门洞检测盒和几个定位点
    ADeliveryCompanionEncounterScene();

    /// @brief 门口触发点的世界坐标
    UFUNCTION(BlueprintPure, Category="Delivery|初遇")
    FVector GetTriggerWorldLocation() const;

    /// @brief 抛出起点的世界变换
    UFUNCTION(BlueprintPure, Category="Delivery|初遇")
    FTransform GetThrowStartTransform() const;

    /// @brief 抛出起点的世界坐标
    UFUNCTION(BlueprintPure, Category="Delivery|初遇")
    FVector GetThrowStartWorldLocation() const;

    /// @brief 抛出落点的世界坐标
    UFUNCTION(BlueprintPure, Category="Delivery|初遇")
    FVector GetThrowLandingWorldLocation() const;

    /// @brief 只接收服务器请求；返回 true 表示接受目标，门被挡住时会等待，不表示已经开关到位
    UFUNCTION(BlueprintCallable, BlueprintAuthorityOnly, Category="Delivery|初遇|门")
    bool RequestDoorOpen(bool bOpen);

    /// @brief 门已经完全打开
    UFUNCTION(BlueprintPure, Category="Delivery|初遇|门")
    bool IsDoorOpen() const;
    /// @brief 用实际门框和抛出方向判断玩家是否已经离开面馆内侧
    bool IsPastDoorway(const FVector& Position) const;

    /// @brief 区分初遇暂停与演出结束后的关门等待，附近玩家据此显示提示
    EDeliveryCompanionEncounterObstruction GetEncounterObstruction() const { return EncounterObstruction; }
    /// @brief 服务器要开门但门洞被挡住
    bool IsDoorOpeningBlocked() const { return HasAuthority() && bRequestedOpen && bDoorBlocked; }
    /// @brief 服务器写入初遇被什么挡住并同步，再调用 OnRep_EncounterObstruction
    void SetEncounterObstruction(EDeliveryCompanionEncounterObstruction Obstruction);
    FSimpleMulticastDelegate OnEncounterObstructionChanged;

    /// @brief 客户端开始抛出演出前同时确认服务器进度和本机门叶表现，等待跨 Actor 复制到齐
    UFUNCTION(BlueprintPure, Category="Delivery|初遇|门")
    bool IsDoorPresentationOpen() const;

    /// @brief 服务器也执行此事件，使实际门叶碰撞与表现使用同一份蓝图变换
    UFUNCTION(BlueprintImplementableEvent, Category="Delivery|初遇|门")
    void ApplyDoorPresentation(float Progress);

    /// @brief 老板保持在独立场景中，角色按实际开门进度与抛出阶段时间通知蓝图接上演出
    UFUNCTION(BlueprintImplementableEvent, Category="Delivery|初遇")
    void UpdateBossPresentation(double EncounterElapsed, bool bVisible);

    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category="Delivery|初遇")
    TObjectPtr<USceneComponent> SceneRoot;

    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category="Delivery|初遇")
    TObjectPtr<USceneComponent> TriggerAnchor;

    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category="Delivery|初遇")
    TObjectPtr<USceneComponent> ThrowStart;

    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category="Delivery|初遇")
    TObjectPtr<USceneComponent> ThrowLanding;

    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category="Delivery|初遇")
    TObjectPtr<UStaticMeshComponent> DoorLeaf;

    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category="Delivery|初遇")
    TObjectPtr<USkeletalMeshComponent> NoodleOwner;

    /// 蓝图把盒子放在门口并覆盖整个门叶转动范围；盒子只供查询，不参与实体碰撞
    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category="Delivery|初遇|门")
    TObjectPtr<UBoxComponent> DoorwayClearance;

    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category="Delivery|初遇|门",
        meta=(ClampMin="0.05", ClampMax="5", Units="s"))
    float DoorTravelSeconds = .6f;

    UPROPERTY(ReplicatedUsing=OnRep_DoorState, VisibleInstanceOnly, BlueprintReadOnly, Category="Delivery|初遇|门")
    float DoorProgress = 0.f;

    UPROPERTY(ReplicatedUsing=OnRep_DoorState, VisibleInstanceOnly, BlueprintReadOnly, Category="Delivery|初遇|门")
    bool bDoorBlocked = false;

    /// @brief 复制门的进度、是否被挡和初遇阻挡状态
    virtual void GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const override;

protected:
    /// @brief 按当前门的进度摆好门叶，服务器门还没到目标位置时开启 Tick
    virtual void BeginPlay() override;
    /// @brief 服务器推进门的进度，门洞有人时停住；客户端把显示进度平滑追到服务器进度
    virtual void Tick(float DeltaSeconds) override;
    /// @brief 关掉 Tick
    virtual void EndPlay(const EEndPlayReason::Type Reason) override;

private:
    /// @brief 门叶、门洞检测盒、开门时长和蓝图里的门动画都配好时返回 true
    bool HasDoorConfiguration() const;
    /// @brief 门洞检测盒里有人、车或物理物体时返回 true，配置不全也按占用处理
    bool IsDoorwayOccupied() const;
    /// @brief 写入门是否被挡并同步，没变化就不写
    void SetDoorBlocked(bool bBlocked);
    /// @brief 客户端收到门的进度：第一次或门已全开时直接对齐，否则开 Tick 平滑追上
    UFUNCTION() void OnRep_DoorState();
    UFUNCTION() void OnRep_EncounterObstruction();
    UPROPERTY(ReplicatedUsing=OnRep_EncounterObstruction)
    EDeliveryCompanionEncounterObstruction EncounterObstruction=EDeliveryCompanionEncounterObstruction::None;

    /// 目标只由服务器修改，客户端仅追随复制的实际进度
    bool bRequestedOpen = false;
    bool bReceivedDoorState = false;
    float DisplayedDoorProgress = 0.f;
};
