// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Components/ActorComponent.h"
#include "Engine/EngineTypes.h"
#include "DeliveryTrafficCarComponent.generated.h"

DECLARE_DYNAMIC_MULTICAST_DELEGATE(FDeliveryTrafficCarRouteLostSignature);

/**
 * 挂在 BP_car_base 上的辅助组件，负责两件蓝图侧不好独立维护的事：
 *
 * 1. 循环：车辆按预设的样条路径链（一段一段 TrafficLine/Intersection 接力）行驶，
 *    正常情况下每次走到一段样条末尾都会通过 Overlap 拿到下一段样条继续跟随。
 *    如果走完了所有预设路径、没能接上下一段（比如路网没铺完、或者这就是路网的
 *    终点），车辆会脱离样条、按最后一次的朝向继续往前"裸奔"，直接冲出地图/画面。
 *    本组件不接管移动，只需要蓝图每帧用 UpdateRouteFollowState() 告诉它
 *    "这一帧还有没有在跟随某条预设样条"；连续脱离路线超过 RouteLostTimeout 秒，
 *    就广播 OnRouteLost，蓝图绑定后把车放回起始样条的起点，形成一直循环的路口车流。
 *
 * 2. 避让：TickComponent 里做前方扫描，检测到前方也挂了本组件的车辆（即另一辆
 *    BP_car_base）时，把 GetSpeedMultiplier() 平滑降到 0；蓝图在算目标速度时
 *    乘上这个系数即可跟着减速到静止，前车让开后再平滑加回正常速度。
 */
UCLASS(ClassGroup=(Delivery), meta=(BlueprintSpawnableComponent))
class DELIVERY_API UDeliveryTrafficCarComponent : public UActorComponent
{
	GENERATED_BODY()

public:

	UDeliveryTrafficCarComponent();

	virtual void TickComponent(float DeltaTime, ELevelTick TickType, FActorComponentTickFunction* ThisTickFunction) override;

	/**
	 * 蓝图行驶 Tick 时每帧调用一次：这一帧车辆是否仍然挂在某条预设样条上跟随行驶。
	 * 走到样条末尾、成功 Overlap 接上下一段车道时传 true；如果没接上、车辆已经
	 * 脱离样条按最后方向直行（也就是"不跟随任何预定路线"），传 false。
	 */
	UFUNCTION(BlueprintCallable, Category = "Traffic|循环")
	void UpdateRouteFollowState(bool bIsFollowingRoute);

	/** 蓝图把车重置回起始样条之后调用，清空脱离路线的计时，避免刚重置完又立刻重复触发。 */
	UFUNCTION(BlueprintCallable, Category = "Traffic|循环")
	void NotifyResetToStart();

	/** 连续脱离预设路线超过 RouteLostTimeout 秒时广播；蓝图绑定这个事件，把车放回起始样条起点即可形成循环车流。 */
	UPROPERTY(BlueprintAssignable, Category = "Traffic|循环")
	FDeliveryTrafficCarRouteLostSignature OnRouteLost;

	/** 蓝图算目标速度时乘上这个系数：1 表示正常速度，0 表示因为前车挡路而完全停住。 */
	UFUNCTION(BlueprintPure, Category = "Traffic|避让")
	float GetSpeedMultiplier() const { return SpeedMultiplier; }

protected:

	/** 连续脱离预设路线（裸奔）超过这么久（秒）就触发 OnRouteLost，让路口车流循环起来。 */
	UPROPERTY(EditAnywhere, Category = "Traffic|循环", meta = (ClampMin = "0.1"))
	float RouteLostTimeout = 5.0f;

	/** 前方探测的最大距离：车头基准点到探测终点的长度。放大一些能提前发现前车，给减速留够距离，不会靠得太近才急刹。 */
	UPROPERTY(EditAnywhere, Category = "Traffic|避让", meta = (ClampMin = "0.0"))
	float ForwardTraceDistance = 1000.0f;

	/** 探测用的球半径，近似车身宽度的一半，避免侧向擦过的车也被算成挡路。 */
	UPROPERTY(EditAnywhere, Category = "Traffic|避让", meta = (ClampMin = "0.0"))
	float ForwardTraceRadius = 150.0f;

	/** 探测起点相对 Actor 原点的前向偏移，避免车头自己的碰撞体把射线一开始就挡住。 */
	UPROPERTY(EditAnywhere, Category = "Traffic|避让", meta = (ClampMin = "0.0"))
	float TraceStartForwardOffset = 150.0f;

	/** 探测用的碰撞通道，需要和车辆碰撞体实际所在的通道匹配，默认按 WorldDynamic 处理。 */
	UPROPERTY(EditAnywhere, Category = "Traffic|避让")
	TEnumAsByte<ECollisionChannel> TraceChannel = ECC_WorldDynamic;

	/**
	 * 速度系数每秒变化的最大幅度：减速和恢复都按这个速率平滑过渡，不是瞬间切换。
	 * 调小可以让减速更柔和（配合更大的 ForwardTraceDistance 提前发现前车、慢慢刹，
	 * 而不是探测到才急刹）；1.0 大约对应"1 秒内从正常速度降到停止"。
	 */
	UPROPERTY(EditAnywhere, Category = "Traffic|避让", meta = (ClampMin = "0.01"))
	float SpeedMultiplierChangeRate = 0.6f;

	/**
	 * 被前车挡住连续超过这么久（秒）就强制放行，忽略这一次的"前方有车"判定。
	 * 两条车道汇入同一点时，双方都在对方的前方扫描范围内、都减速到 0 之后彼此的
	 * 相对位置不再变化，检测结果会永远是"前面有车"，形成死锁——谁也不让谁。
	 * 这个超时相当于简化版的路权仲裁：卡太久就放弃避让，先走的车会移开，
	 * 后续帧对方自然也能通过。
	 */
	UPROPERTY(EditAnywhere, Category = "Traffic|避让", meta = (ClampMin = "0.0"))
	float MaxBlockedTime = 3.0f;

	/** 打开后在场景里画出前方探测用的胶囊范围，方便在编辑器里调探测距离和半径。 */
	UPROPERTY(EditAnywhere, Category = "Traffic|避让")
	bool bDrawDebugTrace = false;

private:

	bool bIsCurrentlyOnRoute = true;
	float RouteLostElapsedTime = 0.0f;

	float SpeedMultiplier = 1.0f;

	/** 被前车挡住已经持续了多久，用于 MaxBlockedTime 死锁超时判定。 */
	float BlockedElapsedTime = 0.0f;

	/** 前方是否有另一辆挂了本组件的车挡路。 */
	bool IsCarAhead() const;
};
