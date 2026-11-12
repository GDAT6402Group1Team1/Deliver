#pragma once
#include "CoreMinimal.h"
#include "Components/ActorComponent.h"
#include "DeliveryNpcCombatComponent.generated.h"

// 在服务器处理 NPC 挥拳、受击和起身，同一拳只判定一次命中
// 倒地和起身状态同步给客户端，由表现组件播放相应动画
UCLASS(ClassGroup=(Delivery),meta=(BlueprintSpawnableComponent))
class DELIVERY_API UDeliveryNpcCombatComponent : public UActorComponent
{
    GENERATED_BODY()
public:
    UDeliveryNpcCombatComponent();
    // 检查距离、遮挡和身体状态，攻击冷却在 StartStrike 中检查
    bool CanStrike(AActor* Target) const;
    // 返回 true 表示开始挥拳，是否命中要等到命中时间再判断
    bool StartStrike(AActor* Target,FGuid ActionId);
    // 清除攻击目标和编号，取消后不再判定尚未发生的命中
    void CancelStrike();
    // 在服务器处理击退和倒地，不等待模型回复
    void ReceiveImpact(const FVector& Impulse,AActor* Source);
    bool SetDeliberateProne(bool bProne);
    UFUNCTION(BlueprintPure) bool IsDown() const { return bDown; }
    // 主动躺下保留说话能力，被击倒时仍立即停止对白
    bool IsDeliberatelyProne() const { return bDown && bDeliberate; }
    UFUNCTION(BlueprintPure) bool IsRecovering() const { return bRecovering; }
    UFUNCTION(BlueprintPure) bool IsStriking() const { return AttackId.IsValid(); }
    UPROPERTY(EditAnywhere,Category="Delivery|居民战斗",meta=(ClampMin="50",ClampMax="250",Units="cm")) float StrikeRange=160;
    UPROPERTY(EditAnywhere,Category="Delivery|居民战斗",meta=(ClampMin="1",ClampMax="10",Units="s")) float RecoverySeconds=4;
    virtual void GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& Out) const override;
protected:
    virtual void TickComponent(float Dt,ELevelTick Type,FActorComponentTickFunction* Fn) override;
    virtual void EndPlay(const EEndPlayReason::Type Reason) override;
private:
    UPROPERTY(ReplicatedUsing=OnRep_Down) bool bDown=false;
    UPROPERTY(Replicated) bool bDeliberate=false;
    UPROPERTY(Replicated) bool bRecovering=false;
    UFUNCTION() void OnRep_Down();
    FGuid AttackId;
    TWeakObjectPtr<AActor> AttackTarget;
    double HitAt=0;
    double RecoverAt=0;
    double NextStrikeAt=0;
};
