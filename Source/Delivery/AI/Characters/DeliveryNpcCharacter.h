#pragma once
#include "CoreMinimal.h"
#include "GameFramework/Character.h"
#include "Combat/DeliveryImpactReceiver.h"
#include "Vehicle/DeliveryHornReceiver.h"
#include "AI/Decision/DeliveryNpcTypes.h"
#include "Engine/DataTable.h"
#include "DeliveryNpcCharacter.generated.h"
class UDeliveryNpcAgentComponent;
class UDeliveryNpcCombatComponent;
class UDeliveryNpcPresentationComponent;
class UDeliveryVehicleHoldComponent;
class UDeliveryInteractableComponent;
class UBehaviorTree;
class UCapsuleComponent;
class UDeliveryNpcLookComponent;

// 创建 NPC 使用的组件，并根据动作类型调用移动、战斗和对白功能
// 接收玩家发言和附近事件，安排下一次模型请求及失败重试
UCLASS()
class DELIVERY_API ADeliveryNpcCharacter : public ACharacter, public IDeliveryImpactReceiver, public IDeliveryHornReceiver
{
    GENERATED_BODY()
public:
    ADeliveryNpcCharacter();
    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category="Delivery|居民") TObjectPtr<UCapsuleComponent> ImpactSensor;
    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category="Delivery|居民") TObjectPtr<UDeliveryNpcAgentComponent> Agent;
    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category="Delivery|居民") TObjectPtr<UDeliveryNpcCombatComponent> Combat;
    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category="Delivery|居民") TObjectPtr<UDeliveryNpcPresentationComponent> Presentation;
    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category="Delivery|居民") TObjectPtr<UDeliveryNpcLookComponent> Look;
    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category="Delivery|居民") TObjectPtr<UDeliveryInteractableComponent> ConversationInteractable;
    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="Delivery|居民") TObjectPtr<UBehaviorTree> BehaviorTree;
    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category="Delivery|居民") FText DisplayName;
    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category="Delivery|人物",meta=(RowType="/Script/Delivery.DeliveryNpcProfileRow")) FDataTableRowHandle Profile;
    // 从配置表更新姓名、性格和声音，未选择有效行时保留原设置
    UFUNCTION(BlueprintCallable,CallInEditor,Category="Delivery|人物") bool ApplyProfile();
    // 检查 NPC 当前能否执行这个动作，只返回检查结果
    bool CanExecute(EDeliveryNpcAction Action, AActor* Target) const;
    static constexpr float MoveAcceptanceRadius=140.f;
    // 使用 UE 导航的到达判断，同时考虑双方碰撞体和高度
    bool HasReachedMoveTarget(AActor* Target) const;
    // 返回 true 表示动作已开始；攻击是否命中、移动是否到达要等待执行结果
    bool ExecuteAction(const FDeliveryNpcPublicState& State);
    // 停止指定编号的动作，较早任务的结束通知不会影响当前动作
    void StopAction(FGuid ActionId);
    void SetHeldVehicle(UDeliveryVehicleHoldComponent* Hold);
    UDeliveryVehicleHoldComponent* GetHeldVehicle() const { return HeldVehicle.Get(); }
    virtual void ReceiveImpact(const FVector& Impulse,const FVector& Point) override;
    virtual void ReceiveImpactFrom(const FVector& Impulse,const FVector& Point,AActor* Source) override;
    virtual bool IsKnockedDown() const override;
    virtual void ReceiveHorn(float Multiplier) override;
    virtual void ReceiveHornFrom(float Multiplier,AActor* Vehicle,AActor* Driver) override;
    // 接收玩家或其他 NPC 的发言，返回 true 表示已听见，还不表示已经回答
    // 玩家发言先经过交谈组件检查，再从服务器调用这里
    bool HearSpeech(AActor* Speaker,const FString& Text,FGuid TurnId,int32 Depth);
    // 玩家开口从第 0 轮开始，NPC 接话时逐轮增加
    int32 GetConversationDepth() const { return ConversationDepth; }
    void WakeDecision();
    void ObserveFact(FName Kind,AActor* Subject,const FString& Text,EDeliveryNpcEvidenceSource Source);
protected:
    virtual void BeginPlay() override;
    virtual void EndPlay(const EEndPlayReason::Type Reason) override;
    virtual void Tick(float Dt) override;
private:
    UFUNCTION() void OnDecisionFinished(FName Reason);
    int32 DecisionRetryCount=0;
    bool CanTalk(APawn* Interactor,bool bCheckAvailability);
    bool OpenConversation(APawn* Interactor);
    TWeakObjectPtr<UDeliveryVehicleHoldComponent> HeldVehicle;
    double LastHorn = -100;
    int32 HornPressure = 0;
    double LastHornObservation = -100;
    double NextDecision = 0;
    int32 ConversationDepth = 0;
    FGuid ExecutingAction;
    FGuid LastFinishedAction;
    bool bDecisionNeeded=true;
};
