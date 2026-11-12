#pragma once
#include "CoreMinimal.h"
#include "GameFramework/Character.h"
#include "Combat/DeliveryImpactReceiver.h"
#include "Vehicle/DeliveryHornReceiver.h"
#include "AI/DeliveryNpcTypes.h"
#include "DeliveryResidentCharacter.generated.h"
class UDeliveryNpcAgentComponent;
class UDeliveryResidentCombatComponent;
class UDeliveryNpcPresentationComponent;
class UDeliveryVehicleHoldComponent;
class UDeliveryInteractableComponent;
class UBehaviorTree;
class UCapsuleComponent;

// 正式居民只组装组件和协调动作，认知、战斗、车辆占用、对白表现各有唯一修改者
UCLASS()
class DELIVERY_API ADeliveryResidentCharacter : public ACharacter, public IDeliveryImpactReceiver, public IDeliveryHornReceiver
{
    GENERATED_BODY()
public:
    ADeliveryResidentCharacter();
    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category="Delivery|居民") TObjectPtr<UCapsuleComponent> ImpactSensor;
    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category="Delivery|居民") TObjectPtr<UDeliveryNpcAgentComponent> Agent;
    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category="Delivery|居民") TObjectPtr<UDeliveryResidentCombatComponent> Combat;
    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category="Delivery|居民") TObjectPtr<UDeliveryNpcPresentationComponent> Presentation;
    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category="Delivery|居民") TObjectPtr<UDeliveryInteractableComponent> ConversationInteractable;
    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="Delivery|居民") TObjectPtr<UBehaviorTree> BehaviorTree;
    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category="Delivery|居民") FText DisplayName;
    // 只查询当前执行条件，不预约目标、不修改其他组件
    bool CanExecute(EDeliveryNpcAction Action, AActor* Target) const;
    // true 只表示执行器已开始，不代表攻击命中或导航已到达
    bool BeginApprovedAction(const FDeliveryNpcPublicState& State);
    // 只清理仍属于此编号的动作，旧任务结束不能释放新任务的占用
    void EndApprovedAction(FGuid ActionId);
    void SetHeldVehicle(UDeliveryVehicleHoldComponent* Hold);
    UDeliveryVehicleHoldComponent* GetHeldVehicle() const { return HeldVehicle.Get(); }
    virtual void ReceiveImpact(const FVector& Impulse,const FVector& Point) override;
    virtual void ReceiveImpactFrom(const FVector& Impulse,const FVector& Point,AActor* Source) override;
    virtual bool IsKnockedDown() const override;
    virtual void ReceiveHorn(float Multiplier) override;
    virtual void ReceiveHornFrom(float Multiplier,AActor* Vehicle,AActor* Driver) override;
    // 只能由已校验的交谈入口或服务器听觉入口调用
    bool HearSpeech(AActor* Speaker,const FString& Text,FGuid TurnId,int32 Depth);
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
