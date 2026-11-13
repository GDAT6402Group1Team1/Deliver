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
    friend class FDeliveryNpcConversationContextTest;
    friend class FDeliveryConversationFeedbackTest;
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
    // 只检查当前导航是否能完整走到目标，不启动移动或修改现有路径
    bool CanReachTarget(AActor* Target) const;
    // 找到附近可完整走到的落点，查询本身不移动 NPC
    bool FindPaceDestination(FVector& OutLocation) const;
    bool FindSideStepDestination(AActor* Target,FVector& OutLocation) const;
    // 选取远离威胁、沿导航可达且胶囊能通过的位置，不在查询中移动角色
    bool FindFleeDestination(AActor* Threat,float RemainingTravel,FVector& OutLocation) const;
    UPROPERTY(EditAnywhere,BlueprintReadOnly,Category="Delivery|步态",meta=(ClampMin="30",ClampMax="400",Units="cm/s")) float WalkSpeed=180;
    UPROPERTY(EditAnywhere,BlueprintReadOnly,Category="Delivery|步态",meta=(ClampMin="30",ClampMax="180",Units="cm/s")) float PaceSpeed=80;
    UPROPERTY(EditAnywhere,BlueprintReadOnly,Category="Delivery|步态",meta=(ClampMin="80",ClampMax="400",Units="cm/s")) float AngryApproachSpeed=240;
    UPROPERTY(EditAnywhere,BlueprintReadOnly,Category="Delivery|步态",meta=(ClampMin="40",ClampMax="140",Units="cm")) float PaceDistance=100;
    UPROPERTY(EditAnywhere,BlueprintReadOnly,Category="Delivery|步态",meta=(ClampMin="80",ClampMax="250",Units="cm")) float SideStepDistance=180;
    UPROPERTY(EditAnywhere,BlueprintReadOnly,Category="Delivery|逃跑",meta=(ClampMin="80",ClampMax="350",Units="cm")) float FleeStepDistance=240;
    UPROPERTY(EditAnywhere,BlueprintReadOnly,Category="Delivery|逃跑",meta=(ClampMin="200",ClampMax="900",Units="cm")) float FleeSafeDistance=500;
    UPROPERTY(EditAnywhere,BlueprintReadOnly,Category="Delivery|逃跑",meta=(ClampMin="200",ClampMax="1200",Units="cm")) float FleeTravelLimit=700;
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
    // bRequestReply 为 false 时只记住这句话，不安排接话或打断当前回复
    bool HearSpeech(AActor* Speaker,const FString& Text,FGuid TurnId,int32 Depth,AActor* Addressee=nullptr,FGuid ReplyToEventId=FGuid(),bool bRequestReply=true);
    // 玩家开口从第 0 轮开始，NPC 接话时逐轮增加
    int32 GetConversationDepth() const { return ConversationDepth; }
    void WakeDecision();
    bool IsDecisionRetryScheduled() const { return bDecisionRetryScheduled; }
    void ObserveFact(FName Kind,AActor* Subject,const FString& Text,EDeliveryNpcEvidenceSource Source);
protected:
    virtual void BeginPlay() override;
    virtual void EndPlay(const EEndPlayReason::Type Reason) override;
    virtual void Tick(float Dt) override;
private:
    bool FindNearbyWalkDestination(const FVector& Desired,float MaxTravel,FVector& OutLocation) const;
    float PreviousActionWalkSpeed=-1;
    UFUNCTION() void OnDecisionFinished(FName Reason);
    int32 DecisionRetryCount=0;
    bool bDecisionRetryScheduled=false;
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
