// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Components/ActorComponent.h"
#include "Engine/EngineTypes.h"
#include "DeliveryTrafficCarComponent.generated.h"

DECLARE_DYNAMIC_MULTICAST_DELEGATE(FDeliveryTrafficCarRouteLostSignature);

/**
 * 辅助交通车的蓝图移动，处理路线丢失、前车避让和碰撞伤害
 * 蓝图提供路线跟随状态，并将速度乘以组件给出的减速系数
 * 脱离路线或长期停滞时请求重置，碰撞伤害仅由服务器结算
 */
UCLASS(ClassGroup = (Delivery), meta = (BlueprintSpawnableComponent))

class DELIVERY_API UDeliveryTrafficCarComponent : public UActorComponent
{
	GENERATED_BODY()

  public:
	UDeliveryTrafficCarComponent();

	virtual void BeginPlay() override;
	virtual void TickComponent(float DeltaTime, ELevelTick TickType,
	                           FActorComponentTickFunction* ThisTickFunction) override;

	// 蓝图行驶状态
	/** 蓝图每帧报告是否仍沿预设样条行驶，成功衔接下一段时也视为跟随路线 */
	UFUNCTION(BlueprintCallable, Category = "Traffic|循环")
	void UpdateRouteFollowState(bool bIsFollowingRoute);

	/** 蓝图把车重置回起始样条之后调用，清空脱离路线的计时，避免刚重置完又立刻重复触发 */
	UFUNCTION(BlueprintCallable, Category = "Traffic|循环")
	void NotifyResetToStart();

	/**
	 * 接收蓝图计算的车速和速度上限
	 * 车辆达到正常速度后才累计路线丢失时间，避免起步阶段被反复重置
	 */
	UFUNCTION(BlueprintCallable, Category = "Traffic|循环")
	void UpdateSpeedState(float InCurrentSpeed, float InMaxSpeed);

  private:
	/** 请求缩小并重置车辆，路线丢失和长期停滞共用此流程 */
	void BeginRouteReset();

  public:
	/** 接收蓝图的红灯停车状态，停车期间使用更长的阻塞超时 */
	UFUNCTION(BlueprintCallable, Category = "Traffic|避让")
	void UpdateStoppedAtIntersection(bool bInStoppedAtIntersection);

	/**
	 * 查询本车或前车是否因红灯停车
	 * 将等待红灯的状态沿车队传递，避免队尾误判为普通堵塞而强行通过
	 */
	UFUNCTION(BlueprintPure, Category = "Traffic|避让")

	bool IsStoppedAtIntersection() const
	{
		return bStoppedAtIntersection;
	}

	/** 连续脱离预设路线超过 RouteLostTimeout 秒时广播；蓝图绑定这个事件，把车放回起始样条起点即可形成循环车流 */
	UPROPERTY(BlueprintAssignable, Category = "Traffic|循环")
	FDeliveryTrafficCarRouteLostSignature OnRouteLost;

	/** 蓝图算目标速度时乘上这个系数：1 表示正常速度，0 表示因为前车挡路而完全停住 */
	UFUNCTION(BlueprintPure, Category = "Traffic|避让")

	float GetSpeedMultiplier() const
	{
		return SpeedMultiplier;
	}

  protected:
	// 分波生成
	/**
	 * 按间隔在原出生点生成后续车辆，达到波次数后停止
	 * 延迟生成可确保后续车辆在出生前不会运行驾驶逻辑
	 */
	UPROPERTY(EditAnywhere, Category = "Traffic|分波生成")
	bool bSpawnExtraWaves = true;

	/** 关卡开始多久（秒）出第二波 */
	UPROPERTY(EditAnywhere, Category = "Traffic|分波生成", meta = (ClampMin = "0.0"))
	float SecondWaveDelay = 15.0f;

	/**
	 * 第二波成功生成后，到第三波生成的间隔，单位为秒
	 * 出生点被占用时会重试，后续波次也随实际生成时间顺延
	 */
	UPROPERTY(EditAnywhere, Category = "Traffic|分波生成", meta = (ClampMin = "0.0"))
	float ThirdWaveDelay = 30.0f;

	/** 出生点附近有交通车时不生成，按此间隔重试（秒） */
	UPROPERTY(EditAnywhere, Category = "Traffic|分波生成", meta = (ClampMin = "0.1"))
	float WaveSpawnRetryDelay = 2.0f;

	/** 出生点水平净空半径；只检查附近交通车，不影响其他物体 */
	UPROPERTY(EditAnywhere, Category = "Traffic|分波生成", meta = (ClampMin = "0.0"))
	float WaveSpawnClearance = 400.0f;

	/** 高架上下层不互相阻挡分波生成 */
	UPROPERTY(EditAnywhere, Category = "Traffic|分波生成", meta = (ClampMin = "0.0"))
	float WaveSpawnHeightTolerance = 200.0f;

	// 前后车速度协调
	/**
	 * 跟车时适当提高慢车的速度上限，同时降低本车上限
	 * 前车取得调整前的本车上限，避免双方持续重复调整
	 * 重置车辆时恢复原速度配置
	 */
	UPROPERTY(EditAnywhere, Category = "Traffic|跟车")
	bool bPushSlowLeader = true;

	/**
	 * 允许协调前后车速度的最大距离，单位为厘米
	 * 应小于前方探测距离，避免尚未形成跟车时就调整远处车辆
	 */
	UPROPERTY(EditAnywhere, Category = "Traffic|跟车", meta = (ClampMin = "0.0"))
	float PushSlowLeaderMaxDistance = 300.0f;

	/** 提高前车速度后，本车降低的速度上限 */
	UPROPERTY(EditAnywhere, Category = "Traffic|跟车", meta = (ClampMin = "0.0"))
	float PushSlowLeaderSelfPenalty = 100.0f;

	// 撞飞路人
	/**
	 * 撞到路人时给的冲量基准
	 * 路人不扣血、没有晕倒，被撞就是直接打飞，所以只有这一个量
	 */
	UPROPERTY(EditAnywhere, Category = "Traffic|撞击", meta = (ClampMin = "0.0"))
	float PedestrianImpactImpulse = 42000.0f;

	/** 撞飞路人时往上抬的比例，纯水平的话人会贴着地面滑出去，不像被撞飞 */
	UPROPERTY(EditAnywhere, Category = "Traffic|撞击", meta = (ClampMin = "0.0", ClampMax = "1.0"))
	float PedestrianImpactUpwardFraction = 0.35f;

	/** 协调车速时允许本车降低到的最小速度，避免连续调整后速度过低 */
	UPROPERTY(EditAnywhere, Category = "Traffic|跟车", meta = (ClampMin = "0.0"))
	float MinPushedMaxSpeed = 600.0f;

	// 出现与消失效果
	/**
	 * 车辆出现时由小变大，不修改车身材质
	 * 适用于当前使用不透明材质的车辆模型
	 */
	UPROPERTY(EditAnywhere, Category = "Traffic|出现动画")
	bool bScaleInOnSpawn = true;

	/** 从起始缩放放大到本来的缩放要多久（秒） */
	UPROPERTY(EditAnywhere, Category = "Traffic|出现动画", meta = (ClampMin = "0.01"))
	float ScaleInDuration = 0.5f;

	/** 起始缩放相对目标缩放的比例；不能给 0：0 缩放会让碰撞体退化 */
	UPROPERTY(EditAnywhere, Category = "Traffic|出现动画", meta = (ClampMin = "0.001", ClampMax = "1.0"))
	float ScaleInStartRatio = 0.01f;

	/**
	 * 缩放出现效果每帧最多推进的时间，单位为秒
	 * 限制启动卡顿造成的进度跳变，卡顿时效果的实际持续时间会延长
	 */
	UPROPERTY(EditAnywhere, Category = "Traffic|出现消失", meta = (ClampMin = "0.001"))
	float MaxScaleAnimStep = 0.05f;

	/** 重置位置前先播放缩小效果，效果结束后再通知蓝图移动车辆 */
	UPROPERTY(EditAnywhere, Category = "Traffic|出现动画")
	bool bScaleOutOnRouteLost = true;

	/** 缩小消失要多久（秒） */
	UPROPERTY(EditAnywhere, Category = "Traffic|出现动画", meta = (ClampMin = "0.01"))
	float ScaleOutDuration = 0.4f;

	// 路线丢失与停滞限制
	/** 达到正常速度后，允许连续脱离路线的时长，单位为秒 */
	UPROPERTY(EditAnywhere, Category = "Traffic|循环", meta = (ClampMin = "0.1"))
	float RouteLostTimeout = 3.0f;

	/**
	 * 车辆持续低速达到此时长后重置，单位为秒
	 * 红灯排队使用单独的较长时限，避免正常等待的车辆被重置
	 */
	UPROPERTY(EditAnywhere, Category = "Traffic|循环", meta = (ClampMin = "0.5"))
	float StalledTimeout = 12.0f;

	/**
	 * 红灯排队时允许停滞的时长，单位为秒
	 * 需覆盖红灯和整队起步时间
	 * 若小于红灯阻塞放行时限，将先重置车辆而不是强行通过
	 */
	UPROPERTY(EditAnywhere, Category = "Traffic|循环", meta = (ClampMin = "0.5"))
	float StalledTimeoutAtIntersection = 15.0f;

	/** 车速低于这个值（cm/s）算"停着"；给点余量，别用严格的 0 */
	UPROPERTY(EditAnywhere, Category = "Traffic|循环", meta = (ClampMin = "0.0"))
	float StallSpeedThreshold = 5.0f;

	// 前车检测与减速
	/**
	 * 沿车头直线探测前车的距离，单位为厘米
	 * 增大可提前减速，但弯道上过大会扫到相邻车道
	 * 最终停车距离由最小跟车间距控制
	 */
	UPROPERTY(EditAnywhere, Category = "Traffic|避让", meta = (ClampMin = "0.0"))
	float ForwardTraceDistance = 900.0f;

	/**
	 * 前车探测球的半径，单位为厘米
	 * 过大会把相邻车道的车辆也算作障碍
	 */
	UPROPERTY(EditAnywhere, Category = "Traffic|避让", meta = (ClampMin = "0.0"))
	float ForwardTraceRadius = 80.0f;

	/** 探测起点相对 Actor 原点的前向偏移，避免车头自己的碰撞体把射线一开始就挡住 */
	UPROPERTY(EditAnywhere, Category = "Traffic|避让", meta = (ClampMin = "0.0"))
	float TraceStartForwardOffset = 150.0f;

	/**
	 * 从探测起点计算的停车距离，单位为厘米，必须小于前方探测距离
	 * 车辆越接近该距离，目标速度越低
	 * 实际车身间距还受探测起点偏移、球半径和减速过程影响
	 */
	UPROPERTY(EditAnywhere, Category = "Traffic|避让", meta = (ClampMin = "0.0"))
	float MinFollowDistance = 350.0f;

	/**
	 * 前车探测使用的碰撞通道
	 * 车辆碰撞体必须对该通道响应，否则扫描无法发现前车
	 */
	UPROPERTY(EditAnywhere, Category = "Traffic|避让")
	TEnumAsByte<ECollisionChannel> TraceChannel = ECC_Vehicle;

	/**
	 * 速度系数每秒允许变化的幅度，数值越大减速和恢复越快
	 * 调低时需要足够的探测距离，否则可能尚未停稳就碰到前车
	 */
	UPROPERTY(EditAnywhere, Category = "Traffic|避让", meta = (ClampMin = "0.01"))
	float SpeedMultiplierChangeRate = 1.5f;

	/** 最后一次因前车减速后，至少保持当前跟车速度系数这么久才允许回升；再次减速立即生效并重新计时 */
	UPROPERTY(EditAnywhere, Category = "Traffic|避让", meta = (ClampMin = "0.0"))
	float FollowAccelerationPause = 1.0f;

	/**
	 * 被前车阻挡超过此时长后临时放弃避让，单位为秒
	 * 用于解除两车互相等待造成的堵塞
	 */
	UPROPERTY(EditAnywhere, Category = "Traffic|避让", meta = (ClampMin = "0.0"))
	float MaxBlockedTime = 6.0f;

	/**
	 * 本车或前车等待红灯时使用的阻塞时限，单位为秒
	 * 需覆盖完整红灯及车队起步时间，避免红灯期间强行通过
	 */
	UPROPERTY(EditAnywhere, Category = "Traffic|避让", meta = (ClampMin = "0.0"))
	float MaxBlockedTimeAtIntersection = 16.0f;

	/** 打开后在场景里画出前方探测用的胶囊范围，方便在编辑器里调探测距离和半径 */
	UPROPERTY(EditAnywhere, Category = "Traffic|避让")
	bool bDrawDebugTrace = false;

	// 撞击伤害与击退
	/** 车不模拟物理，用它和角色有效质量估算相对速度转成的撞击速度变化 */
	UPROPERTY(EditAnywhere, Category = "Traffic|撞击", meta = (ClampMin = "1.0"))
	float VehicleEffectiveMassKg = 120.0f;

	UPROPERTY(EditAnywhere, Category = "Traffic|撞击", meta = (ClampMin = "1.0"))
	float CharacterEffectiveMassKg = 60.0f;

	/** 估算角色速度变化达到此值时直接让 HP 归零，进入已有的晕倒/回血流程 */
	UPROPERTY(EditAnywhere, Category = "Traffic|撞击", meta = (ClampMin = "0.0"))
	float KnockdownDeltaV = 300.0f;

	UPROPERTY(EditAnywhere, Category = "Traffic|撞击", meta = (ClampMin = "0.0"))
	float MinimumDamageDeltaV = 80.0f;

	UPROPERTY(EditAnywhere, Category = "Traffic|撞击", meta = (ClampMin = "0.0"))
	float DamagePerDeltaV = 0.08f;

	UPROPERTY(EditAnywhere, Category = "Traffic|撞击", meta = (ClampMin = "0.0"))
	float MaximumLightDamage = 24.0f;

	UPROPERTY(EditAnywhere, Category = "Traffic|撞击", meta = (ClampMin = "0.0"))
	float MaximumKnockbackDeltaV = 350.0f;

	/** 强撞、且受击者仍在地面时的目标起飞竖直速度；400 cm/s 约腾空 82 cm、0.8 秒落地 */
	UPROPERTY(EditAnywhere, Category = "Traffic|撞击", meta = (ClampMin = "0.0"))
	float StrongHitTakeoffSpeed = 400.0f;

	UPROPERTY(EditAnywhere, Category = "Traffic|撞击", meta = (ClampMin = "0.0"))
	float RepeatHitCooldown = 0.75f;

	/** 只用于服务器检测角色，不改变车辆对场景物件的现有碰撞 */
	UPROPERTY(EditAnywhere, Category = "Traffic|撞击")
	FVector ImpactHalfExtent = FVector(150.0f, 90.0f, 90.0f);

	UPROPERTY(EditAnywhere, Category = "Traffic|撞击")
	FVector ImpactCenterOffset = FVector(70.0f, 0.0f, 90.0f);

  private:
	// 生成与缩放更新
	/** 到点后在记录下来的出生变换处生成下一波的车 */
	void SpawnNextWave();

	/**
	 * 当前生成波次，零表示关卡中预先放置的车辆
	 * 用对象标签在延迟生成阶段传递，避免蓝图组件尚未创建时无法写入波次
	 */
	int32 GetWaveIndex() const;

	/**
	 * 缩放动画，出现和消失共用一个：
	 * 出现 = 从 ScaleInStartRatio 倍推回 ScaleInTargetScale
	 * 消失 = 反过来缩回去，缩完广播 OnRouteLost
	 */
	void UpdateScaleAnim(float RawDeltaTime);
	FVector ScaleInTargetScale = FVector::OneVector;
	float ScaleInElapsed = 0.0f;
	/** 本次出现效果经过的帧数，用于检查是否被单帧卡顿跳过 */
	int32 ScaleInTicks = 0;
	bool bScaleInRunning = false;
	float ScaleOutElapsed = 0.0f;
	bool bScaleOutRunning = false;
	/** 是否正在等待缩小完成，防止同一次异常重复请求重置 */
	bool bRouteLostPending = false;

	/** 开始运行时保存的速度上限和出生变换，用于重置车辆 */
	double OriginalMaxSpeed = 0.0;
	bool bHasOriginalMaxSpeed = false;

	FTimerHandle NextWaveTimer;
	bool bWaveSpawnWasBlocked = false;
	
	FTransform SecondWaveTransform = FTransform::Identity;

	// 服务器碰撞处理
	void ProcessVehicleImpacts(float DeltaTime);

	/** 单独检测玩家摩托车的碰撞，避免与角色身体检测重复 */
	/**
	 * 撞飞路人
	 * 路人在自己的碰撞通道上，而且不走 GAS（没有血量和晕倒），所以单独一次扫描、
	 * 单独一条结算——并进撞角色那一次会把已经调通的伤害/抛起逻辑搅乱
	 */
	void ProcessPedestrianImpacts(const FVector& Start, const FVector& End, const FQuat& Rotation,
	                              const FVector& CarVelocity, const FCollisionQueryParams& Params);

	void ProcessMotorbikeImpacts(const FVector& Start, const FVector& End, const FQuat& Rotation,
	                             const FVector& CarVelocity, const FCollisionQueryParams& Params);
	FTransform PreviousImpactTransform = FTransform::Identity;
	bool bHasPreviousImpactTransform = false;
	bool bSkipNextImpactSweep = false;
	TMap<TWeakObjectPtr<AActor>, float> LastImpactTimeByActor;

	// 行驶与等待状态
	bool bIsCurrentlyOnRoute = true;
	float RouteLostElapsedTime = 0.0f;

	/** 车速几乎为 0 持续了多久；由 TickComponent 累加，起步/重置时清零 */
	float StalledElapsedTime = 0.0f;

	/** 蓝图上一次通过 UpdateSpeedState 报上来的车速；组件自己不算移动，只能这样拿 */
	float LastKnownSpeed = 0.0f;

	float SpeedMultiplier = 1.0f;
	/** 减速结束后到允许重新加速的剩余时间 */
	float FollowAccelerationPauseRemaining = 0.0f;

	/** 被前车挡住已经持续了多久，用于死锁超时判定 */
	float BlockedElapsedTime = 0.0f;

	/** 是否已经达到正常速度，达到后才累计路线丢失时间 */
	bool bHasReachedMaxSpeed = false;

	/** 本车当前是否被红灯拦住，决定死锁超时用哪个阈值，见 UpdateStoppedAtIntersection() */
	bool bStoppedAtIntersection = false;

	/** 上次探测到的前车是否正在等待红灯，供车队逐级传递停车状态 */
	bool bLeaderStoppedAtIntersection = false;

	/**
	 * 返回前车到探测起点的距离，无目标时返回负一
	 * 同时更新前车的红灯状态，并按需协调车速
	 */
	
	float GetDistanceToCarAhead();

  public:
	// 运行诊断
	/** 调试用：当前的避让速度系数，PIE 里可以直接看，省得靠打印推断 */
	UPROPERTY(VisibleInstanceOnly, BlueprintReadOnly, Category = "Traffic|调试")
	float DebugSpeedMultiplier = 1.0f;

	/** 调试用：前方最近一辆车的距离，-1 表示没探测到 */
	UPROPERTY(VisibleInstanceOnly, BlueprintReadOnly, Category = "Traffic|调试")
	float DebugDistanceAhead = -1.0f;

	/** 调试用：被前车挡住已经持续了多久（秒） */
	UPROPERTY(VisibleInstanceOnly, BlueprintReadOnly, Category = "Traffic|调试")
	float DebugBlockedElapsed = 0.0f;

	/** 跟车加速还需等待多久（秒）；等待时减速仍可立即生效 */
	UPROPERTY(VisibleInstanceOnly, BlueprintReadOnly, Category = "Traffic|调试")
	float DebugAccelerationPauseRemaining = 0.0f;
};
