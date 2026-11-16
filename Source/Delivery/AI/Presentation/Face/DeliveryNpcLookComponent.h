#pragma once

#include "CoreMinimal.h"
#include "Components/ActorComponent.h"
#include "DeliveryNpcLookComponent.generated.h"

/// 服务器决定的看向目标：现在看谁、看到什么时候，同步给所有客户端
USTRUCT()
struct FDeliveryNpcLookState
{
    GENERATED_BODY()

    /// 看谁
    UPROPERTY() TObjectPtr<AActor> Target;

    /// 看到什么时候，服务器时间
    UPROPERTY() double EndTime=0;

    /// 是不是被蹭到才看的
    UPROPERTY() bool bAfterBrush=false;

    /// 是不是被喇叭吓到才看的
    UPROPERTY() bool bStartled=false;
};

/**
 * @brief NPC 的眼神：有人路过、蹭到或在近处按喇叭时短暂看过去
 *
 * 关注：服务器 Tick 每 0.2 秒查附近玩家，走近就 StartLook 看一眼，擦身而过看得久一点
 * 受惊：近处鸣笛时角色调 TryStartle，看向声源并换上受惊表情
 * 动画蓝图读 GetLookTarget 转头和眼睛；这里不改移动、不算伤害
 */
UCLASS(ClassGroup=(Delivery),meta=(BlueprintSpawnableComponent))
class DELIVERY_API UDeliveryNpcLookComponent : public UActorComponent
{
    GENERATED_BODY()
    friend class FDeliveryNpcLookTest;
public:
    UDeliveryNpcLookComponent();

    // ── 关注 ──
    // 动画和表情从这里读现在看着谁

    /// @brief 现在看着谁，过时了返回空
    AActor* GetLookTarget() const;

    /// @brief 是不是因为被蹭到在看
    bool IsAfterBrush() const;

    // ── 受惊 ──

    /// @brief 服务器：近处鸣笛时短暂看向声源，只换眼神和表情，不说话不做动作
    /// @return 看过去了返回 true；正忙、被挡住或还在冷却时返回 false
    bool TryStartle(AActor* Source);

    /// @brief 是不是正被吓着
    bool IsStartled() const;

    /// @brief 看这个人时眼睛对准的点，大约在头部
    static FVector GetLookPoint(const AActor* Target);

    // ── 配置 ──

    /// 是否看路过的玩家
    UPROPERTY(EditAnywhere,Category="注视",meta=(DisplayName="关注路过的玩家")) bool bNoticePassersby=true;

    /// 多久查一次附近的玩家
    UPROPERTY(EditAnywhere,Category="注视",meta=(ClampMin="0.1",ClampMax="1",Units="s")) float CheckInterval=.2f;

    /// 两次看人之间至少隔多久，几个人轮流走近时不会一直左右转头
    UPROPERTY(EditAnywhere,Category="注视",meta=(DisplayName="两次关注的间隔",ClampMin="0.2",ClampMax="10",Units="s")) float AttentionInterval=2.f;

    /// 走进多近会被看
    UPROPERTY(EditAnywhere,Category="注视",meta=(ClampMin="100",Units="cm")) float NoticeDistance=350;

    /// 走出多远算离开，下次走近才再看
    UPROPERTY(EditAnywhere,Category="注视",meta=(ClampMin="100",Units="cm")) float ReleaseDistance=550;

    /// 看一眼多久
    UPROPERTY(EditAnywhere,Category="注视",meta=(ClampMin="0.2",ClampMax="4",Units="s")) float LookDuration=1.1f;

    /// 同一位玩家隔多久才会再被看
    UPROPERTY(EditAnywhere,Category="注视",meta=(ClampMin="1",Units="s")) float PlayerCooldown=6;

    /// 被蹭到后看多久
    UPROPERTY(EditAnywhere,Category="擦身",meta=(ClampMin="0.2",ClampMax="4",Units="s")) float BrushLookDuration=1.8f;

    /// 身体贴近到这个距离内算蹭到
    UPROPERTY(EditAnywhere,Category="擦身",meta=(ClampMin="0",ClampMax="15",Units="cm")) float BrushMargin=5;

    /// 同一位玩家隔多久才会再算蹭到
    UPROPERTY(EditAnywhere,Category="擦身",meta=(ClampMin="1",Units="s")) float BrushCooldown=8;

    /// 低于这个速度才算擦身；更快的碰撞交给受击处理
    UPROPERTY(EditAnywhere,Category="擦身",meta=(ClampMin="50",Units="cm/s")) float BrushMaxSpeed=250;

    /// 近处鸣笛时会被吓到
    UPROPERTY(EditAnywhere,Category="即时反应",meta=(DisplayName="回应近处鸣笛")) bool bStartleAtHorn=true;

    /// 多近的喇叭会吓到
    UPROPERTY(EditAnywhere,Category="即时反应",meta=(DisplayName="鸣笛反应距离",ClampMin="50",ClampMax="600",Units="cm")) float StartleDistance=220.f;

    /// 受惊表情持续多久
    UPROPERTY(EditAnywhere,Category="即时反应",meta=(DisplayName="受惊持续时间",ClampMin="0.2",ClampMax="2",Units="s")) float StartleDuration=.65f;

    /// 隔多久才会再被吓
    UPROPERTY(EditAnywhere,Category="即时反应",meta=(DisplayName="受惊间隔",ClampMin="1",ClampMax="60",Units="s")) float StartleCooldown=10.f;

    /// 受惊时用的表情名，对应表情资产；None 表示不换表情
    UPROPERTY(EditAnywhere,Category="即时反应",meta=(DisplayName="受惊表情")) FName StartleExpression=TEXT("Surprised");

    /// 挨打时用的表情名
    UPROPERTY(EditAnywhere,Category="即时反应",meta=(DisplayName="受击表情")) FName HitExpression=TEXT("Surprised");

    /// 普通路人转头最多转多少度；路人的步态不受影响
    UPROPERTY(EditAnywhere,Category="普通行人姿态",meta=(ClampMin="1",ClampMax="60",Units="deg")) float HeadYawLimit=40;

    /// 普通路人抬头低头最多多少度
    UPROPERTY(EditAnywhere,Category="普通行人姿态",meta=(ClampMin="1",ClampMax="30",Units="deg")) float HeadPitchLimit=20;

    /// 普通路人转头的跟随速度
    UPROPERTY(EditAnywhere,Category="普通行人姿态",meta=(ClampMin="1",ClampMax="20")) float HeadFollowSpeed=8;

protected:
    virtual void BeginPlay() override;
    virtual void EndPlay(const EEndPlayReason::Type Reason) override;
    virtual void TickComponent(float DeltaTime,ELevelTick TickType,FActorComponentTickFunction* Function) override;
    virtual void GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const override;
private:
    // ── 关注 ──

    /// 每位玩家相对这位 NPC 的状态：在不在附近、有没有蹭到、下次什么时候能再看
    struct FPlayerVisit
    {
        bool bNearby=false;
        bool bTouching=false;
        double NextLook=0;
        double NextBrush=0;
        FVector LastPosition=FVector::ZeroVector;
        bool bHasPosition=false;
    };

    /// 现在看谁，同步给客户端
    UPROPERTY(Replicated) FDeliveryNpcLookState LookState;

    /// 每位玩家的状态
    TMap<TWeakObjectPtr<AActor>,FPlayerVisit> PlayerVisits;

    /// 上次检查时自己的位置
    FVector LastOwnerPosition=FVector::ZeroVector;

    /// 已经记过自己的位置
    bool bHasOwnerPosition=false;

    /// 下次检查时重建玩家状态
    bool bRefreshVisits=false;

    /// 下次能看人的时间
    double NextAttentionAt=0;

    /// 下次能被吓的时间
    double NextStartleAt=0;

    /// 服务器时间
    double ServerTime() const;

    /// 现在能不能分心看人：没倒地、没在出拳或挨打、没在听或说话、没在执行别的安排；二狗还要空手站定
    bool CanLook() const;

    /// 开始看这个人
    void StartLook(AActor* Player,bool bAfterBrush /* true 表示被蹭到才看的，看得久一点 */,double Now /* 服务器时间 */);

    /// 不看了
    void ClearLook();
};
