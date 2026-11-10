// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Components/ActorComponent.h"
#include "Engine/EngineTypes.h"
#include "DeliveryTrafficCarComponent.generated.h"

class UDeliveryVehicleAudioComponent;
class USoundBase;
class USoundAttenuation;
class APlayerController;

DECLARE_DYNAMIC_MULTICAST_DELEGATE(FDeliveryTrafficCarRouteLostSignature);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FDeliveryTrafficHornSignature, USoundBase*, Sound);

/**
 * 辅助交通车的蓝图移动，处理路线丢失、前车避让和碰撞伤害
 * 蓝图提供路线跟随状态，并将速度乘以组件给出的减速系数
 * 脱离路线或长期停滞时请求重置，碰撞伤害仅由服务器结算
 */
UCLASS(ClassGroup = (Delivery), meta = (BlueprintSpawnableComponent))

class DELIVERY_API UDeliveryTrafficCarComponent : public UActorComponent
{
	GENERATED_BODY()
	friend class FDeliveryTrafficHornBoostTest;

  public:
	UDeliveryTrafficCarComponent();

	virtual void BeginPlay() override;
	virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;
	virtual void TickComponent(float DeltaTime, ELevelTick TickType,
	                           FActorComponentTickFunction* ThisTickFunction) override;

	/** 服务器请求鸣笛，成功发出通知返回 true；冷却、重置或没有有效声音时返回 false */
	UFUNCTION(BlueprintCallable, BlueprintAuthorityOnly, Category = "Traffic|鸣笛")
	bool TryHonk();

	/** 各客户端收到本次鸣笛后通知，可用于灯光等表现；迟加入的玩家不会重播旧鸣笛 */
	UPROPERTY(BlueprintAssignable, Category = "Traffic|鸣笛")
	FDeliveryTrafficHornSignature OnHornPlayed;

	/** 从有效的非循环声音中随机选一个，多种声音时避免连续重复；清空数组可静音 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Traffic|鸣笛")
	TArray<TObjectPtr<USoundBase>> HornSounds;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Traffic|鸣笛")
	bool bAutoHonkNearPlayers = true;

	/** 玩家当前控制的角色或载具距交通车中心多近时触发 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Traffic|鸣笛", meta = (ClampMin = "0.0", Units = "cm"))
	float HornTriggerDistance = 800.0f;

	/** 车头左右各覆盖多少度，按水平朝向检测；90 度覆盖前半面，侧面和后方不触发 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Traffic|鸣笛", meta = (ClampMin = "1.0", ClampMax = "90.0", Units = "deg"))
	float HornForwardHalfAngle = 60.0f;

	/** 玩家离开此距离后才允许下次接近触发，运行时至少比触发距离大 1 cm */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Traffic|鸣笛", meta = (ClampMin = "0.0", Units = "cm"))
	float HornRearmDistance = 1100.0f;

	/** 同一辆车的所有玩家和手动请求共用冷却，至少等待本次声音结束 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Traffic|鸣笛", meta = (ClampMin = "0.1", Units = "s"))
	float HornCooldown = 8.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Traffic|鸣笛", meta = (ClampMin = "0.0"))
	float HornVolume = 1.0f;

	/** 每次鸣笛乘到 HornVolume 上的随机倍率，X 为最小值、Y 为最大值；两值相同可关闭随机 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Traffic|鸣笛", meta = (ClampMin = "0.0", ClampMax = "2.0"))
	FVector2D HornVolumeRange = FVector2D(0.9f, 1.1f);

	/** 每次鸣笛的随机音调，1 为原音调，X 为最小值、Y 为最大值 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Traffic|鸣笛", meta = (ClampMin = "0.5", ClampMax = "2.0"))
	FVector2D HornPitchRange = FVector2D(0.95f, 1.05f);

	/** 留空时使用默认空间衰减，8 米内全音量，向外 24 米渐弱 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Traffic|鸣笛")
	TObjectPtr<USoundAttenuation> HornAttenuation;

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
		// 自己按停车线拦下来的也算"在等红灯"：这样后车拿到的是更宽松的阻塞/停滞
		// 时限，不会把合法的排队当成死锁而压上来。
		// 用“或”而不是直接写 bStoppedAtIntersection，是因为蓝图每帧都会用它自己
		// 的探测结果覆盖那个值，而我们停得更早、蓝图那条前瞻往往根本探不到路口。
		// 为行人让行同样算"合法地长时间不动"：和红灯一个道理，后车要拿到更宽松的
		// 阻塞/停滞时限，否则队尾会把前面那辆正在等人过马路的车当成死锁压上去。
		return bStoppedAtIntersection || bHoldingForRedLight || bHoldingForPedestrian;
	}

	/**
	 * 本车此刻是否正停着等行人走开
	 *
	 * 和 IsStoppedAtIntersection() 分开暴露：那个是给"该用哪个超时"用的合并状态，
	 * 这个是给调试命令和蓝图表现（喇叭、刹车灯）用的具体原因。
	 */
	UFUNCTION(BlueprintPure, Category = "Traffic|避让")
	bool IsYieldingToPedestrian() const
	{
		return bHoldingForPedestrian;
	}

	/** 连续脱离预设路线超过 RouteLostTimeout 秒时广播；蓝图绑定这个事件，把车放回起始样条起点即可形成循环车流 */
	UPROPERTY(BlueprintAssignable, Category = "Traffic|循环")
	FDeliveryTrafficCarRouteLostSignature OnRouteLost;

	/** 交通车蓝图报告的当前速度，供过街前判断斑马线是否有移动车辆 */
	float GetReportedSpeed() const { return LastKnownSpeed; }
	/** 仅正常满速行驶时接收短时鸣笛加速，不修改 MaxSpeed */
	void RequestHornSpeedBoost(float Multiplier);

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

	/** 出生点附近交通车的水平净空半径 */
	UPROPERTY(EditAnywhere, Category = "Traffic|分波生成", meta = (ClampMin = "0.0"))
	float WaveSpawnClearance = 400.0f;

	/** 出生点附近行人和玩家的水平净空半径 */
	UPROPERTY(EditAnywhere, Category = "Traffic|分波生成", meta = (ClampMin = "0.0", Units = "cm"))
	float WaveSpawnPersonClearance = 250.0f;

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

	// 红灯停车线
	/**
	 * 在路口段前留出一段固定距离停车，空出来的那段给行人和斑马线
	 *
	 * **为什么不靠蓝图现有的那套。** 蓝图的停车位置是算出来的副产品：
	 * `TraceForIntersection` 的前瞻长度和车速成正比，`Deceleration` 的刹停行程
	 * 又约等于 `DecelerationDistance`，所以"停在哪"随车速浮动、没有任何一个变量
	 * 代表停车线。要一段**固定**的净空，就只能自己按真实距离刹车。
	 *
	 * 关掉这个开关就完全回到原来的行为（整段逻辑只影响速度系数，不碰蓝图）。
	 */
	UPROPERTY(EditAnywhere, Category = "Traffic|停车线")
	bool bUseStopLine = true;

	/**
	 * 往前找路口段的距离，单位厘米，**和车速无关**
	 *
	 * 固定长度是刻意的：蓝图那条前瞻∝车速，队伍里第二辆之后的车已经被前车逼停、
	 * 前瞻缩到接近 0，根本探不到路口盒（红灯状态沿车队传递就是为了绕开这件事）。
	 * 这里要能在停住之后继续看见红灯，所以不能跟车速走。
	 * 必须大于 StopLineSetback + StopLineBrakeDistance，否则还没看见就该刹了。
	 */
	UPROPERTY(EditAnywhere, Category = "Traffic|停车线", meta = (ClampMin = "0.0"))
	float RedLightScanDistance = 1600.0f;

	/**
	 * 停车线离路口段还有多远，单位厘米——**这就是给行人留出的那段净空**
	 *
	 * 斑马线应该正好画在这一段里，所以改这个数等于改斑马线宽度，两边要对上。
	 *
	 * **不能给太小**：车道段和路口段之间本来就留着 TARGET_GAP（150 cm）的空隙，
	 * 那段里 CurrentFollowSpline 是 None、车算"脱离路线"。停车线落进那条缝里的话，
	 * 车会停在那儿满 RouteLostTimeout（3 秒）然后被瞬移回出生点——
	 * 表现是"车到路口前就莫名消失又从别处开出来"。
	 */
	UPROPERTY(EditAnywhere, Category = "Traffic|停车线", meta = (ClampMin = "0.0"))
	float StopLineSetback = 400.0f;

	/**
	 * 从离停车线多远开始减速，单位厘米
	 * 给小了会"到线前一脚急刹"，给大了老远就开始爬行
	 */
	UPROPERTY(EditAnywhere, Category = "Traffic|停车线", meta = (ClampMin = "1.0"))
	float StopLineBrakeDistance = 700.0f;

	/** 有斑马线的路口，即使没有行人，接近后也把目标车速限制到此值（cm/s） */
	UPROPERTY(EditAnywhere, Category = "Traffic|停车线", meta = (ClampMin = "1.0", Units = "cm/s"))
	float CrosswalkSpeedLimit = 500.0f;

	/** 进入斑马线前多远开始限速（cm） */
	UPROPERTY(EditAnywhere, Category = "Traffic|停车线", meta = (ClampMin = "0.0", Units = "cm"))
	float CrosswalkSlowdownDistance = 1000.0f;

	/**
	 * 已经越过停车线这么多（厘米）就算"冲进去了"，不再往回拦
	 *
	 * 不留这个余量的话，绿灯时刚压过线、灯转红的那辆车会当场定在路口里，
	 * 比闯过去糟得多。
	 */
	UPROPERTY(EditAnywhere, Category = "Traffic|停车线", meta = (ClampMin = "0.0"))
	float StopLineCommitDistance = 200.0f;

	/**
	 * 找路口段用的探测球半径，单位厘米
	 * 比前车探测的小：大了会扫到横向那条路的路口段，读到相反的灯态
	 */
	UPROPERTY(EditAnywhere, Category = "Traffic|停车线", meta = (ClampMin = "1.0"))
	float StopLineTraceRadius = 40.0f;

	/**
	 * 只认和车头同向的路口段，两者方向点积的下限
	 *
	 * 路口里两条路的盒子是叠着的，而横向那条的灯**正好相反**——
	 * 不校验方向的话车会读到对面的灯态，表现是"绿灯不走、红灯硬闯"。
	 * 蓝图里那条 cast 本来就有这个问题，这里不重复它。
	 */
	UPROPERTY(EditAnywhere, Category = "Traffic|停车线", meta = (ClampMin = "-1.0", ClampMax = "1.0"))
	float StopLineSameDirectionDot = 0.5f;

	/**
	 * 只有带这个 tag 的路口段才提前停车；留空则所有红灯都提前停
	 *
	 * **为什么要按路口分**：提前停车是为了给行人让出位置，而斑马线只画在
	 * 市中心那几个主路口。其余路口没有斑马线、也没有行人要让，提前停只会
	 * 让车莫名停在路口外面。
	 *
	 * tag 由 `Content/Python/setup_crosswalks.py` 摆斑马线时打到对应的
	 * `Inter_*` actor 上——**"这里有斑马线"和"这里要提前停"是同一个事实，
	 * 所以只存一处**。分两处记迟早出现"画了斑马线车还是冲进路口"，
	 * 或者反过来"车停在没有斑马线的路口外面"。
	 *
	 * 试过改存项目设置（`Config/DefaultGame.ini`，文本、能合并），为的是躲开
	 * `.umap` 二进制没法合并那个问题。**但那没有收益**：斑马线贴花本身就必须
	 * 存在地图里，地图冲突躲不掉，所以把 tag 也放在地图里一分钱不多花，
	 * 还省掉坐标匹配半径和"改完要重启编辑器"。那个类已经删掉。
	 *
	 * 打 tag 只是**改现有 actor 的标记**，不动它的位置、不删也不加交通样条线。
	 *
	 * 没有这个 tag 的路口段**完全不进本节逻辑**，行为和加停车线之前一字不差，
	 * 所以其余路口的零回归是结构上保证的，不靠测出来。
	 *
	 * **重新生成车道会丢掉 tag**（和 `LightNumber` 一样，重建出来的是全新 actor），
	 * 跟着重跑一次 setup_crosswalks.py 即可。
	 */
	UPROPERTY(EditAnywhere, Category = "Traffic|停车线")
	FName StopLineRequiredTag = FName(TEXT("Crosswalk"));

	/** 打开后画出停车线的位置和探测线 */
	UPROPERTY(EditAnywhere, Category = "Traffic|停车线")
	bool bDrawDebugStopLine = false;

	// 行人让行
	/**
	 * 前方有行人时停下，等人走开再走
	 *
	 * **主要是这种情况**：绿灯了，但斑马线上还有人没走完。红灯那条（停车线）只管
	 * 灯，灯一变绿它就完全放手，所以必须另有一条按"前方有没有人"判断的——
	 * 两件事的触发条件不一样，合不到一个判断里。
	 *
	 * 关掉这个开关就完全回到原来的行为（只影响速度系数，不碰蓝图）。
	 */
	UPROPERTY(EditAnywhere, Category = "Traffic|行人让行")
	bool bYieldToPedestrians = true;

	/**
	 * 往前找行人的距离，单位厘米，**和车速无关**
	 *
	 * 和停车线同一个理由：队伍里第二辆之后的车已经被前车逼停，跟车速走的前瞻会
	 * 缩到接近 0、停住之后就再也看不见人，于是刚起步又要急刹。
	 * 要大于 PedestrianStopDistance + 一段减速行程，否则来不及停。
	 */
	UPROPERTY(EditAnywhere, Category = "Traffic|行人让行", meta = (ClampMin = "0.0"))
	float PedestrianScanDistance = 1400.0f;

	/**
	 * 找行人用的探测球半径，单位厘米
	 *
	 * **这个数定义了"算在我这条车道上"**。给大了会替隔壁车道和路边人行道上的人
	 * 停车（表现是"车莫名停在空路上"）；给小了斑马线上侧着走的人探不到。
	 * 默认按车身半宽（约 68）加上行人胶囊半径再留点余量。
	 */
	UPROPERTY(EditAnywhere, Category = "Traffic|行人让行", meta = (ClampMin = "1.0"))
	float PedestrianScanRadius = 100.0f;

	/**
	 * 停在离行人多远的位置，单位厘米（从探测起点算，所以实际车头间距比它小）
	 *
	 * 斑马线深 StopLineSetback（400）：这个数给到 450 的话，停的位置大致在
	 * 行人所在位置往后 4.5 米，也就是人还在斑马线远侧时车停在近侧边缘外。
	 */
	UPROPERTY(EditAnywhere, Category = "Traffic|行人让行", meta = (ClampMin = "0.0"))
	float PedestrianStopDistance = 450.0f;

	/*
	 * **没有"等够就强行通过"这个旋钮，这是刻意的。**
	 *
	 * 车队死锁那条兜底（卡够 MaxBlockedTime 就放弃避让）是为了解两辆车互为前车的
	 * 死锁；等行人不是死锁，是在等一件一定会结束的事——人走过去就完了。
	 * 给它配超时的唯一理由是"人也在让车"造成的互相礼让，而行人侧的规则是
	 * **人不让车**（人只看信号灯：人行红灯不下马路，人行绿灯也要等斑马线上的车走净），
	 * 所以那个死锁在规则上不存在，不需要兜底。
	 *
	 * 代价说清楚：行人要是**永远**站在车道上（比如游走路线摆得压到路面上），
	 * 这条路就一直堵着。那是摆点问题，用 Delivery.Pedestrian.Dump 查，
	 * 不靠车自己撞过去遮掉。
	 *
	 * 配套地，等人期间**不累计停滞计时**，否则车会先被停滞检测瞬移回出生点，
	 * 表现是"车等人等到自己消失了"。
	 */

	/** 打开后画出找行人的探测范围和命中的人 */
	UPROPERTY(EditAnywhere, Category = "Traffic|行人让行")
	bool bDrawDebugPedestrianYield = false;

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
	// 距离和冷却只由服务器更新，播放交给现有车辆音效组件
	void UpdatePlayerProximityHorn(float DeltaTime);
	void ResetHornProximity();

	UFUNCTION(NetMulticast, Unreliable)
	void MulticastPlayHorn(USoundBase* Sound, float Volume, float Pitch, USoundAttenuation* Attenuation);

	UFUNCTION(NetMulticast, Unreliable)
	void MulticastStopHorn();

	UPROPERTY(Transient)
	TObjectPtr<UDeliveryVehicleAudioComponent> TrafficAudio;

	// 记控制器而不是 Pawn，上下车切换控制对象不会被当成新玩家
	TSet<TWeakObjectPtr<APlayerController>> HonkedPlayers;
	TWeakObjectPtr<USoundBase> LastHornSound;
	double NextHornTime = 0.0;
	float HornCheckElapsed = 0.0f;
	bool bOwnsTrafficAudio = false;

	friend class FDeliveryTrafficHornTest;
	friend class FDeliveryTrafficPedestrianYieldTest;
	friend class FDeliveryTrafficWaveSpawnTest;

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
	float LastKnownMaxSpeed = 0.0f;

	float SpeedMultiplier = 1.0f;
	float HornBoostUntil = -1.f;
	float HornBoostMultiplier = 1.f;
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

	/** 本车正被停车线拦着等红灯；和蓝图自己那套探测分开存，见 IsStoppedAtIntersection() */
	bool bHoldingForRedLight = false;

	/** 本车正停着等前方的行人走开，见 IsYieldingToPedestrian() */
	bool bHoldingForPedestrian = false;

	/**
	 * 上次探测到的前车是否正在为行人让行
	 *
	 * 和 bLeaderStoppedAtIntersection 分开存：那个只决定用哪个阻塞超时，
	 * 这个要**整个关掉**死锁兜底。队首在等人过马路时，后面每一辆都不许强行通过，
	 * 否则兜底会从队尾一辆辆往前把车推进斑马线。
	 */
	bool bLeaderYieldingToPedestrian = false;

	/**
	 * 返回车头到红灯停车线的剩余距离（厘米），前方没有红灯时返回 TNumericLimits<float>::Max()
	 * 负值表示已经越过停车线
	 */
	float GetDistanceToStopLine(bool bRequireRed = true);

	/**
	 * 返回前方最近一个行人到探测起点的距离（厘米），没有要让的人时返回负一
	 *
	 * 已经倒地的人不算：他起不来、让他也让不开，车会白等到超时；
	 * 路人自己的让行逻辑对躺着的人也是同样处理（"绕不过去，过几秒自己就起来了"）。
	 */
	float GetDistanceToPedestrianAhead();

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

	/** 调试用：车头到红灯停车线的剩余距离，负值表示已越过；前方无红灯时为 -1 */
	UPROPERTY(VisibleInstanceOnly, BlueprintReadOnly, Category = "Traffic|调试")
	float DebugStopLineDistance = -1.0f;

	/** 调试用：前方最近一个要让的行人的距离，-1 表示没有 */
	UPROPERTY(VisibleInstanceOnly, BlueprintReadOnly, Category = "Traffic|调试")
	float DebugPedestrianDistance = -1.0f;

	/** 调试用：前车是否正在为行人让行（自己因此也不强行通过） */
	UPROPERTY(VisibleInstanceOnly, BlueprintReadOnly, Category = "Traffic|调试")
	bool bDebugLeaderYieldingToPedestrian = false;
};
