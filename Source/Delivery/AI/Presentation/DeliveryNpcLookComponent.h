#pragma once

#include "CoreMinimal.h"
#include "Components/ActorComponent.h"
#include "DeliveryNpcLookComponent.generated.h"

USTRUCT()
struct FDeliveryNpcLookState
{
    GENERATED_BODY()
    UPROPERTY() TObjectPtr<AActor> Target;
    UPROPERTY() double EndTime=0;
    UPROPERTY() bool bAfterBrush=false;
};

// 服务器决定 NPC 短暂看向谁，动画和眼睛在各台机器上平滑跟随
// 经过或轻擦时只看一眼；实际受击由战斗系统记录，这里不改变移动和伤害
UCLASS(ClassGroup=(Delivery),meta=(BlueprintSpawnableComponent))
class DELIVERY_API UDeliveryNpcLookComponent : public UActorComponent
{
    GENERATED_BODY()
    friend class FDeliveryNpcLookTest;
public:
    UDeliveryNpcLookComponent();
    AActor* GetLookTarget() const;
    bool IsAfterBrush() const;
    static FVector GetLookPoint(const AActor* Target);

    UPROPERTY(EditAnywhere,Category="注视",meta=(ClampMin="0.1",ClampMax="1",Units="s")) float CheckInterval=.2f;
    UPROPERTY(EditAnywhere,Category="注视",meta=(ClampMin="100",Units="cm")) float NoticeDistance=350;
    UPROPERTY(EditAnywhere,Category="注视",meta=(ClampMin="100",Units="cm")) float ReleaseDistance=550;
    UPROPERTY(EditAnywhere,Category="注视",meta=(ClampMin="0.2",ClampMax="4",Units="s")) float LookDuration=1.1f;
    UPROPERTY(EditAnywhere,Category="注视",meta=(ClampMin="1",Units="s")) float PlayerCooldown=6;
    UPROPERTY(EditAnywhere,Category="擦身",meta=(ClampMin="0.2",ClampMax="4",Units="s")) float BrushLookDuration=1.8f;
    UPROPERTY(EditAnywhere,Category="擦身",meta=(ClampMin="0",ClampMax="15",Units="cm")) float BrushMargin=5;
    UPROPERTY(EditAnywhere,Category="擦身",meta=(ClampMin="1",Units="s")) float BrushCooldown=8;
    // 高速碰撞继续交给原来的受击系统，不追加轻擦事件
    UPROPERTY(EditAnywhere,Category="擦身",meta=(ClampMin="50",Units="cm/s")) float BrushMaxSpeed=250;
    // 普通行人仍用原有程序化步态，这些参数只控制叠加的头部变化
    UPROPERTY(EditAnywhere,Category="普通行人姿态",meta=(ClampMin="1",ClampMax="60",Units="deg")) float HeadYawLimit=40;
    UPROPERTY(EditAnywhere,Category="普通行人姿态",meta=(ClampMin="1",ClampMax="30",Units="deg")) float HeadPitchLimit=20;
    UPROPERTY(EditAnywhere,Category="普通行人姿态",meta=(ClampMin="1",ClampMax="20")) float HeadFollowSpeed=8;

protected:
    virtual void BeginPlay() override;
    virtual void EndPlay(const EEndPlayReason::Type Reason) override;
    virtual void TickComponent(float DeltaTime,ELevelTick TickType,FActorComponentTickFunction* Function) override;
    virtual void GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const override;
private:
    struct FPlayerVisit
    {
        bool bNearby=false;
        bool bTouching=false;
        double NextLook=0;
        double NextBrush=0;
        FVector LastPosition=FVector::ZeroVector;
        bool bHasPosition=false;
    };
    UPROPERTY(Replicated) FDeliveryNpcLookState LookState;
    TMap<TWeakObjectPtr<AActor>,FPlayerVisit> PlayerVisits;
    FVector LastOwnerPosition=FVector::ZeroVector;
    bool bHasOwnerPosition=false;
    double ServerTime() const;
    bool CanLook() const;
    void StartLook(AActor* Player,bool bAfterBrush,double Now);
    void ClearLook();
};
