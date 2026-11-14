#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "DeliveryCompanionEncounterScene.generated.h"

class UBoxComponent;
class USceneComponent;
class USkeletalMeshComponent;
class UStaticMeshComponent;

// 保存面馆初遇的固定锚点和门状态，二狗角色负责自身阶段与移动，蓝图配置门和老板的表现
UCLASS()
class DELIVERY_API ADeliveryCompanionEncounterScene : public AActor
{
    GENERATED_BODY()

public:
    ADeliveryCompanionEncounterScene();

    UFUNCTION(BlueprintPure, Category="Delivery|初遇")
    FVector GetTriggerWorldLocation() const;

    UFUNCTION(BlueprintPure, Category="Delivery|初遇")
    FTransform GetThrowStartTransform() const;

    UFUNCTION(BlueprintPure, Category="Delivery|初遇")
    FVector GetThrowStartWorldLocation() const;

    UFUNCTION(BlueprintPure, Category="Delivery|初遇")
    FVector GetThrowLandingWorldLocation() const;

    // 只接收服务器请求；返回 true 表示接受目标，门被挡住时会等待，不表示已经开关到位
    UFUNCTION(BlueprintCallable, BlueprintAuthorityOnly, Category="Delivery|初遇|门")
    bool RequestDoorOpen(bool bOpen);

    UFUNCTION(BlueprintPure, Category="Delivery|初遇|门")
    bool IsDoorOpen() const;

    // 客户端开始抛出演出前同时确认服务器进度和本机门叶表现，等待跨 Actor 复制到齐
    UFUNCTION(BlueprintPure, Category="Delivery|初遇|门")
    bool IsDoorPresentationOpen() const;

    // 服务器也执行此事件，使实际门叶碰撞与表现使用同一份蓝图变换
    UFUNCTION(BlueprintImplementableEvent, Category="Delivery|初遇|门")
    void ApplyDoorPresentation(float Progress);

    // 老板保持在独立场景中，角色按实际开门进度与抛出阶段时间通知蓝图接上演出
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

    // 蓝图把盒子放在门口并覆盖整个门叶转动范围；盒子只供查询，不参与实体碰撞
    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category="Delivery|初遇|门")
    TObjectPtr<UBoxComponent> DoorwayClearance;

    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category="Delivery|初遇|门",
        meta=(ClampMin="0.05", ClampMax="5", Units="s"))
    float DoorTravelSeconds = .6f;

    UPROPERTY(ReplicatedUsing=OnRep_DoorState, VisibleInstanceOnly, BlueprintReadOnly, Category="Delivery|初遇|门")
    float DoorProgress = 0.f;

    UPROPERTY(ReplicatedUsing=OnRep_DoorState, VisibleInstanceOnly, BlueprintReadOnly, Category="Delivery|初遇|门")
    bool bDoorBlocked = false;

    virtual void GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const override;

protected:
    virtual void BeginPlay() override;
    virtual void Tick(float DeltaSeconds) override;
    virtual void EndPlay(const EEndPlayReason::Type Reason) override;

private:
    bool HasDoorConfiguration() const;
    bool IsDoorwayOccupied() const;
    void SetDoorBlocked(bool bBlocked);
    UFUNCTION() void OnRep_DoorState();

    // 目标只由服务器修改，客户端仅追随复制的实际进度
    bool bRequestedOpen = false;
    bool bReceivedDoorState = false;
    float DisplayedDoorProgress = 0.f;
};
