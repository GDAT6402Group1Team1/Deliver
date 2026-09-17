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
 * 2. 避让：TickComponent 里做前方扫描，探测到前方也挂了本组件的车辆（即另一辆
 *    BP_car_base）时，按"离前车还有多远"把 GetSpeedMultiplier() 平滑压低；蓝图在
 *    算目标速度时乘上这个系数即可跟着减速，前车让开后再平滑加回正常速度。
 *    注意是按距离渐进而不是二值开关：探测边缘处系数还是 1（完全不减速），越靠近
 *    越小，近到 MinFollowDistance 以内才真正压到 0。这样才能"探测得远但停得近"
 *    ——探测距离只决定多早开始注意到前车，停车间距由 MinFollowDistance 单独控制。
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

	/**
	 * 蓝图每帧把当前车速和上限速度报给组件。
	 *
	 * 用途是"裸奔计时从速度恢复到 MaxSpeed 之后才开始算"：车刚被重置回出生点、或者
	 * 刚从红灯起步时速度是 0，这段时间它本来就还没接上任何样条（要靠 TraceForNewPath
	 * 现找），如果 RouteLostTimeout 从脱离那一刻就开始跑，车很可能在还没加速起来、
	 * 根本没机会找到路的时候就又被判定成"裸奔超时"，于是反复重置、永远起不来。
	 * 所以只有车速真正回到 MaxSpeed（说明它已经正常跑起来了、还是接不上路）之后，
	 * 才开始累计裸奔时间。
	 */
	UFUNCTION(BlueprintCallable, Category = "Traffic|循环")
	void UpdateSpeedState(float InCurrentSpeed, float InMaxSpeed);

	/**
	 * 蓝图每次写 StopatInter 之后调用一次，把"这辆车当前是不是被红灯拦住"同步给组件。
	 *
	 * 影响的是避让死锁超时该用哪个阈值：正常路面上两车互相礼让形成死锁时，卡
	 * MaxBlockedTime 秒就强行通过；但停红灯时前车本来就该长时间不动，用同一个
	 * 短超时会让后车在红灯还没结束时就"放弃避让"直接怼上去，所以红灯期间换成更
	 * 宽松的 MaxBlockedTimeAtIntersection。
	 */
	UFUNCTION(BlueprintCallable, Category = "Traffic|避让")
	void UpdateStoppedAtIntersection(bool bInStoppedAtIntersection);

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
	float ForwardTraceDistance = 600.0f;

	/** 探测用的球半径，近似车身宽度的一半，避免侧向擦过的车也被算成挡路。 */
	UPROPERTY(EditAnywhere, Category = "Traffic|避让", meta = (ClampMin = "0.0"))
	float ForwardTraceRadius = 60.0f;

	/** 探测起点相对 Actor 原点的前向偏移，避免车头自己的碰撞体把射线一开始就挡住。 */
	UPROPERTY(EditAnywhere, Category = "Traffic|避让", meta = (ClampMin = "0.0"))
	float TraceStartForwardOffset = 150.0f;

	/**
	 * 跟车的最小间距：探测到的前车近到这个距离以内，速度系数就压到 0（完全停住）。
	 *
	 * 和 ForwardTraceDistance 是一对：探测距离决定"多远开始注意到前车并缓慢减速"，
	 * 这个值决定"最终停在离前车多近的地方"。两者之间做线性插值，所以可以探测得很远
	 * （提前、平缓地减速）同时停得很近（不会隔着老远就杵住）。
	 *
	 * 单位是从探测起点算起的距离，不是车头到车尾的净间距——探测起点已经在车头前
	 * TraceStartForwardOffset(150)，扫描球本身还有 ForwardTraceRadius(60) 的半径，
	 * 所以实际保险杠间距大约是 本值 - 90。默认 200 对应约 110 的车距。
	 */
	UPROPERTY(EditAnywhere, Category = "Traffic|避让", meta = (ClampMin = "0.0"))
	float MinFollowDistance = 200.0f;

	/**
	 * 探测用的碰撞通道，必须和车辆碰撞盒实际"有响应"的通道匹配。
	 *
	 * 注意这里踩过坑：BP_car_base 的 Box 碰撞盒 ObjectType 是 ECC_Vehicle，而且它对
	 * WorldDynamic 的响应是 Ignore。之前这个默认值是 ECC_WorldDynamic，SweepMultiByChannel
	 * 查询时引擎按"被击中物体对该通道的响应"来筛选，Ignore 直接跳过——结果整套前车
	 * 探测从来没命中过任何东西，GetDistanceToCarAhead() 恒为 -1，避让功能等于没开。
	 * 改这个值之前先确认车辆碰撞盒对目标通道的响应是 Overlap 或 Block。
	 */
	UPROPERTY(EditAnywhere, Category = "Traffic|避让")
	TEnumAsByte<ECollisionChannel> TraceChannel = ECC_Vehicle;

	/**
	 * 速度系数每秒变化的最大幅度：减速和恢复都按这个速率平滑过渡，不是瞬间切换。
	 * 1.0 大约对应"1 秒内从正常速度降到停止"，数值越大刹得越干脆。
	 * 这个值和 ForwardTraceDistance 是配套的：车速 600 时，速率 R 对应的滑行距离约
	 * 300/R。1.5 约滑行 200，装得进 600 的探测距离；调小到 0.6 就要滑行 500，
	 * 探测距离不够长的话会先怼上前车再慢慢停下。
	 */
	UPROPERTY(EditAnywhere, Category = "Traffic|避让", meta = (ClampMin = "0.01"))
	float SpeedMultiplierChangeRate = 1.5f;

	/**
	 * 被前车挡住连续超过这么久（秒）就强制放行，忽略这一次的"前方有车"判定。
	 * 两条车道汇入同一点时，双方都在对方的前方扫描范围内、都减速到 0 之后彼此的
	 * 相对位置不再变化，检测结果会永远是"前面有车"，形成死锁——谁也不让谁。
	 * 这个超时相当于简化版的路权仲裁：卡太久就放弃避让，先走的车会移开，
	 * 后续帧对方自然也能通过。
	 */
	UPROPERTY(EditAnywhere, Category = "Traffic|避让", meta = (ClampMin = "0.0"))
	float MaxBlockedTime = 3.0f;

	/**
	 * 本车正被红灯拦住（蓝图的 StopatInter 为 true）时改用的死锁超时，比 MaxBlockedTime 宽松。
	 *
	 * 红灯期间前车是"合法地长时间不动"，不是死锁；如果还按 MaxBlockedTime（3 秒）
	 * 就放弃避让，后车会在红灯没结束时直接压上去。红绿灯周期 LightDuration 是 3 秒，
	 * 这里给到 8 秒，足够覆盖一整个红灯而又不至于真死锁时永远卡死。
	 */
	UPROPERTY(EditAnywhere, Category = "Traffic|避让", meta = (ClampMin = "0.0"))
	float MaxBlockedTimeAtIntersection = 8.0f;

	/** 打开后在场景里画出前方探测用的胶囊范围，方便在编辑器里调探测距离和半径。 */
	UPROPERTY(EditAnywhere, Category = "Traffic|避让")
	bool bDrawDebugTrace = false;

private:

	bool bIsCurrentlyOnRoute = true;
	float RouteLostElapsedTime = 0.0f;

	float SpeedMultiplier = 1.0f;

	/** 被前车挡住已经持续了多久，用于死锁超时判定。 */
	float BlockedElapsedTime = 0.0f;

	/** 车速是否已经回到 MaxSpeed；只有为 true 时才累计裸奔时间，见 UpdateSpeedState()。 */
	bool bHasReachedMaxSpeed = false;

	/** 本车当前是否被红灯拦住，决定死锁超时用哪个阈值，见 UpdateStoppedAtIntersection()。 */
	bool bStoppedAtIntersection = false;

	/**
	 * 前方最近一辆挂了本组件的车的距离（从探测起点算），没有则返回 -1。
	 * 返回距离而不是 bool，是为了让减速可以随距离渐进，而不是"探测到就一脚刹死"。
	 */
	float GetDistanceToCarAhead() const;

public:

	/** 调试用：当前的避让速度系数，PIE 里可以直接看，省得靠打印推断。 */
	UPROPERTY(VisibleInstanceOnly, BlueprintReadOnly, Category = "Traffic|调试")
	float DebugSpeedMultiplier = 1.0f;

	/** 调试用：前方最近一辆车的距离，-1 表示没探测到。 */
	UPROPERTY(VisibleInstanceOnly, BlueprintReadOnly, Category = "Traffic|调试")
	float DebugDistanceAhead = -1.0f;

	/** 调试用：被前车挡住已经持续了多久（秒）。 */
	UPROPERTY(VisibleInstanceOnly, BlueprintReadOnly, Category = "Traffic|调试")
	float DebugBlockedElapsed = 0.0f;
};
