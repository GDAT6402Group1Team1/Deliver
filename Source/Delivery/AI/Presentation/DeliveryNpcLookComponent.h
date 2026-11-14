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
    UPROPERTY() bool bStartled=false;
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
    // 服务器确认近处鸣笛后短暂看向声源；忙碌、被挡住或处于冷却时返回 false
    // 只改变注视和表情，不安排台词或玩法动作
    bool TryStartle(AActor* Source);
    bool IsStartled() const;
    static FVector GetLookPoint(const AActor* Target);

    UPROPERTY(EditAnywhere,Category="注视",meta=(DisplayName="关注路过的玩家")) bool bNoticePassersby=true;
    UPROPERTY(EditAnywhere,Category="注视",meta=(ClampMin="0.1",ClampMax="1",Units="s")) float CheckInterval=.2f;
    // 多人轮流走近时也留出间隔，避免人物不停地左右转头
    UPROPERTY(EditAnywhere,Category="注视",meta=(DisplayName="两次关注的间隔",ClampMin="0.2",ClampMax="10",Units="s")) float AttentionInterval=2.f;
    UPROPERTY(EditAnywhere,Category="注视",meta=(ClampMin="100",Units="cm")) float NoticeDistance=350;
    UPROPERTY(EditAnywhere,Category="注视",meta=(ClampMin="100",Units="cm")) float ReleaseDistance=550;
    UPROPERTY(EditAnywhere,Category="注视",meta=(ClampMin="0.2",ClampMax="4",Units="s")) float LookDuration=1.1f;
    UPROPERTY(EditAnywhere,Category="注视",meta=(ClampMin="1",Units="s")) float PlayerCooldown=6;
    UPROPERTY(EditAnywhere,Category="擦身",meta=(ClampMin="0.2",ClampMax="4",Units="s")) float BrushLookDuration=1.8f;
    UPROPERTY(EditAnywhere,Category="擦身",meta=(ClampMin="0",ClampMax="15",Units="cm")) float BrushMargin=5;
    UPROPERTY(EditAnywhere,Category="擦身",meta=(ClampMin="1",Units="s")) float BrushCooldown=8;
    // 高速碰撞继续交给原来的受击系统，不追加轻擦事件
    UPROPERTY(EditAnywhere,Category="擦身",meta=(ClampMin="50",Units="cm/s")) float BrushMaxSpeed=250;
    UPROPERTY(EditAnywhere,Category="即时反应",meta=(DisplayName="回应近处鸣笛")) bool bStartleAtHorn=true;
    UPROPERTY(EditAnywhere,Category="即时反应",meta=(DisplayName="鸣笛反应距离",ClampMin="50",ClampMax="600",Units="cm")) float StartleDistance=220.f;
    UPROPERTY(EditAnywhere,Category="即时反应",meta=(DisplayName="受惊持续时间",ClampMin="0.2",ClampMax="2",Units="s")) float StartleDuration=.65f;
    UPROPERTY(EditAnywhere,Category="即时反应",meta=(DisplayName="受惊间隔",ClampMin="1",ClampMax="60",Units="s")) float StartleCooldown=10.f;
    // 名称对应现有表情资产；None 表示不额外改变表情，真实命中动作仍会播放
    UPROPERTY(EditAnywhere,Category="即时反应",meta=(DisplayName="受惊表情")) FName StartleExpression=TEXT("Surprised");
    UPROPERTY(EditAnywhere,Category="即时反应",meta=(DisplayName="受击表情")) FName HitExpression=TEXT("Surprised");
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
    bool bRefreshVisits=false;
    double NextAttentionAt=0;
    double NextStartleAt=0;
    double ServerTime() const;
    bool CanLook() const;
    void StartLook(AActor* Player,bool bAfterBrush,double Now);
    void ClearLook();
};
