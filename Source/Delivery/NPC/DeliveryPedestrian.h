// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "Vehicle/DeliveryHornReceiver.h"
#include "CoreMinimal.h"
#include "Combat/DeliveryImpactReceiver.h"
#include "GameFramework/Actor.h"
#include "DeliveryPedestrian.generated.h"

class ADeliveryPedestrianRoute;
class UCapsuleComponent;
class UAnimSequence;
class UMaterialInterface;
class UMaterialInstanceDynamic;
class UPoseableMeshComponent;
class USkeletalMesh;
class USkeletalMeshComponent;

/** 行人在一次行程里的三个阶段，出现和消失各占线段的一小段，不额外计时 */
UENUM()
enum class EDeliveryPedestrianPhase : uint8
{
	/** 刚进场，正在从零放大到原尺寸 */
	Appearing,
	/** 正常行走 */
	Walking,
	/** 接近终点，正在缩回零，缩完销毁 */
	Disappearing,
	/** 被打飞或撞飞，整个人交给物理，躺一会儿 */
	KnockedDown,
	/** 躺够了，正从地上起身；起完回到 Walking 继续走 */
	StandingUp
};

/**
 * 由 ADeliveryPedestrianRoute 生成的路人：普通模式在生成点附近游走，特殊模式沿样条行走。
 * 运动学推进、不接 AI 与寻路；每帧向下打射线贴地，服务器权威并复制移动。
 * 受击倒地时才切入物理，恢复后继续原模式。
 *
 * 身体有两个固定槽位，按路线有没有配走路动画二选一，另一个隐藏：
 *   * 配了动画 → 用 SkeletalMesh 单节点播放，播放速率对齐实际速度
 *   * 没配动画 → 用 PoseableMesh 程序化摆腿（见 UpdateProceduralGait）
 * 做成两个槽位而不是运行时换组件，是因为换组件类型会丢蓝图 CDO 上那个组件的
 * 实例覆盖（摩托车骑手那次栽过，表现是"车上突然没人"）
 */
UCLASS()

class DELIVERY_API ADeliveryPedestrian : public AActor, public IDeliveryImpactReceiver, public IDeliveryHornReceiver
{
	GENERATED_BODY()
	friend class FDeliveryBikeHornTest;

  public:
	ADeliveryPedestrian();

	// IDeliveryImpactReceiver
	/**
	 * 被打飞/撞飞：默认播放跌倒、躺卧和爬起；可选真实布娃娃
	 * 只在服务器调用；客户端通过复制的 bKnockedDown 自己也切过去
	 */
	virtual void ReceiveImpact(const FVector& Impulse, const FVector& ImpactPoint) override;

	virtual bool IsKnockedDown() const override
	{
		return bKnockedDown;
	}

	virtual void Tick(float DeltaTime) override;
	virtual void GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const override;

	/**
	 * 由路线在生成后立刻调用，注入这一趟的全部参数
	 * 放在 BeginPlay 之后再调是刻意的：生成时就确定方向和速度，省掉一个"未初始化"状态
	 */
	void BeginWalk(ADeliveryPedestrianRoute* InRoute, USkeletalMesh* InMesh, UAnimSequence* InWalkAnim,
	               float InStartDistance, bool bInForward, float InSpeed, float InAnimReferenceSpeed);

	/** 只调整本次过街的步速；服务器调用，离开红灯后恢复原速度 */
	void SetCrosswalkSpeedMultiplier(float Multiplier);
	virtual void ReceiveHorn(float SpeedMultiplier) override;

	// 程序化步态
	/**
	 * 一个完整步幅走过的距离（cm），相位按走过的路程推进而不是按时间
	 * 按路程算，走得快的人步频自然就高，不需要再调一个频率参数，也不会滑步
	 */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Pedestrian|程序化步态",
	          meta = (ClampMin = "10.0", Units = "cm"))
	float StrideLength = 75.f;

	/** 大腿前后摆动的幅度（度） */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Pedestrian|程序化步态",
	          meta = (ClampMin = "0.0", Units = "deg"))
	float ThighSwingAngle = 26.f;

	/**
	 * 小腿回折的幅度（度）
	 * 膝盖只会往后弯，所以这一项是单向的；给成双向的话抬腿时小腿会向前反折
	 */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Pedestrian|程序化步态",
	          meta = (ClampMin = "0.0", Units = "deg"))
	float KneeBendAngle = 34.f;

	/** 手臂前后摆动的幅度（度），和同侧腿反相 */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Pedestrian|程序化步态",
	          meta = (ClampMin = "0.0", Units = "deg"))
	float ArmSwingAngle = 22.f;

	/**
	 * 手臂垂到体侧的角度（度），0 = 不动、保持源模型的绑定姿势
	 * **必须有这一项**：六份 FBX 的绑定姿势是 T 字形（手平举），而程序化步态
	 * 只在参考姿势上叠一个前后摆，于是人是"平举着双手前后划"的。
	 * 这里把上臂从实测的绑定方向转到体侧，摆动再叠在它上面。
	 * 90 = 完全贴着身体竖直向下，留一点余量看起来更自然
	 */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Pedestrian|程序化步态",
	          meta = (ClampMin = "0.0", ClampMax = "90.0", Units = "deg"))
	float ArmRestDownAngle = 84.f;

	/**
	 * 垂下来之后手臂离开身体的外张角（度）
	 * 给 0 的话上臂会插进躯干里，尤其是胖一点的老人模型
	 */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Pedestrian|程序化步态",
	          meta = (ClampMin = "0.0", ClampMax = "45.0", Units = "deg"))
	float ArmRestOutwardAngle = 5.f;

	/** 躯干跟着步子左右轻摆的幅度（度） */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Pedestrian|程序化步态",
	          meta = (ClampMin = "0.0", Units = "deg"))
	float TorsoRollAngle = 3.5f;

	/** 髋部每一步上下起伏的幅度（cm），一个步幅里起伏两次 */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Pedestrian|程序化步态",
	          meta = (ClampMin = "0.0", Units = "cm"))
	float HipBobHeight = 2.5f;

	// 走走停停
	/**
	 * 相邻两次停顿之间至少走多远 / 最多走多远（cm）
	 * 按**路程**而不是按时间：走得快的人不会因为"每 N 秒停一次"而显得更频繁地停
	 */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Pedestrian|走走停停",
	          meta = (ClampMin = "0.0", Units = "cm"))
	float PauseIntervalMin = 700.f;

	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Pedestrian|走走停停",
	          meta = (ClampMin = "0.0", Units = "cm"))
	float PauseIntervalMax = 2600.f;

	/** 每次停多久（秒） */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Pedestrian|走走停停",
	          meta = (ClampMin = "0.0", Units = "s"))
	float PauseDurationMin = 0.8f;

	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Pedestrian|走走停停",
	          meta = (ClampMin = "0.0", Units = "s"))
	float PauseDurationMax = 3.2f;

	/** 走到端点换路线时顺便停一下的概率，0~1。路口本来就是人会停的地方 */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Pedestrian|走走停停",
	          meta = (ClampMin = "0.0", ClampMax = "1.0"))
	float JunctionPauseChance = 0.45f;

	/** 停下时朝向随机偏开多少度（像在看橱窗），0 = 站得笔直 */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Pedestrian|走走停停",
	          meta = (ClampMin = "0.0", ClampMax = "120.0", Units = "deg"))
	float PauseLookAroundAngle = 38.f;

	/** 站住时躯干轻微晃动的幅度（度）。完全不动看着像卡住，所以不能给 0 */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Pedestrian|走走停停",
	          meta = (ClampMin = "0.0", ClampMax = "10.0", Units = "deg"))
	float IdleSwayAngle = 1.6f;

	/** 站住时一次完整晃动的周期（秒） */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Pedestrian|走走停停",
	          meta = (ClampMin = "0.5", Units = "s"))
	float IdleSwayPeriod = 3.6f;

	// 被打飞
	/**
	 * 跌倒后在地上停留多久，再开始爬起（秒）。
	 */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Pedestrian|被打飞",
	          meta = (ClampMin = "0.0", Units = "s"))
	float KnockedLieDuration = 2.f;

	/** 从站姿倾倒到地面的时间。 */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Pedestrian|被打飞",
	          meta = (ClampMin = "0.1", Units = "s"))
	float FallDuration = 0.55f;

	/** 需要真实布娃娃时可开启；普通街道路人使用稳定的程序化跌倒/爬起。 */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Pedestrian|被打飞")
	bool bUsePhysicalKnockdown = false;

	// 碰撞与让路
	/**
	 * 挨打的胶囊同时挡住玩家的身体，走过去会被挡住而不是穿过去
	 *
	 * **只挡 `PhysicsBody`（玩家布娃娃的刚体），不挡 `Pawn`。**
	 * 摩托车的碰撞盒是 Pawn 配置、而且它的水平推进是带 sweep 的，
	 * 挡 Pawn 的表现是**路人变成一堵墙把车别停**——骑车撞人应该是把人撞飞，
	 * 不是车自己停住。交通车同理（它是运动学的，靠自己那次扫掠撞人）。
	 *
	 * 代价写在这里免得以后重新纠结：全速撞上去有可能把玩家的布娃娃顶翻。
	 * 这是"人是实心的"必然带来的，不想要就关掉这个开关。
	 */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Pedestrian|碰撞")
	bool bBlockCharacters = true;

	/**
	 * 前方有人或场景障碍时停下来等，车辆由车方让行，而不是直接穿过去/把对方顶住
	 *
	 * **这一条是"实心"的另一半，不能只开碰撞不开让路。** 路人是运动学推进的
	 * （`SetActorLocationAndRotation`，不带 sweep），所以 Block 对他自己的移动
	 * 完全不起作用——他会一路推着玩家走，表现就是"被一个不动的人卡住"。
	 * CLAUDE.md 里当初"路人不参与碰撞"那条结论说的正是这个。
	 */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Pedestrian|碰撞")
	bool bYieldToObstacles = true;

	/** 往前探多远算"前面有人"（cm） */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Pedestrian|碰撞",
	          meta = (ClampMin = "0.0", Units = "cm"))
	float YieldProbeDistance = 95.f;

	/** 探测球半径（cm）；给大了会连并排走过的人都躲 */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Pedestrian|碰撞",
	          meta = (ClampMin = "1.0", Units = "cm"))
	float YieldProbeRadius = 26.f;

	/** 探测球离地高度（cm），取腰的位置，别贴着地面扫到路沿 */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Pedestrian|碰撞",
	          meta = (ClampMin = "0.0", Units = "cm"))
	float YieldProbeHeight = 70.f;

	/**
	 * 最多让多久就硬挤过去（秒）
	 *
	 * **必须有这个兜底**：两个人面对面走过来会互相判定"前面有人"、
	 * 永久互相礼让，表现是两个人贴在一起站着不动。
	 * 交通车那边同一个坑（`MaxBlockedTime`），同一个解法。
	 */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Pedestrian|碰撞",
	          meta = (ClampMin = "0.0", Units = "s"))
	float MaxYieldSeconds = 2.5f;

	// 起身
	/** 跌倒后始终爬起；路人一直留在生成点的在场名单中。 */

	/** 从躺着到站直所用的时间（秒） */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Pedestrian|起身",
	          meta = (ClampMin = "0.1", Units = "s"))
	float GetUpDuration = 1.2f;

	/**
	 * 起身过程中屈腿的最大角度（度）
	 *
	 * **曲线两端为零、中间最大**（`sin(alpha × π)`）：这样读起来是"撑着地面蹲起来"，
	 * 而不是"像块板子立起来"。给 0 就退化成纯刚体翻正，一眼就是假的。
	 */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Pedestrian|起身",
	          meta = (ClampMin = "0.0", ClampMax = "80.0", Units = "deg"))
	float GetUpCrouchAngle = 44.f;

	/** 起身过程中躯干前倾的最大角度（度），和屈腿同一条钟形曲线 */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Pedestrian|起身",
	          meta = (ClampMin = "0.0", ClampMax = "80.0", Units = "deg"))
	float GetUpTorsoLeanAngle = 32.f;

	// 调试查询
	/** 当前沿样条走到哪了（cm） */
	float GetDistanceAlongRoute() const
	{
		return DistanceAlongRoute;
	}

	bool IsWalkingForward() const
	{
		return bForward;
	}

	float GetWalkSpeed() const
	{
		return WalkSpeed;
	}

	EDeliveryPedestrianPhase GetPhase() const
	{
		return Phase;
	}

	/** 当前缩放系数（0~1），用来确认出入场曲线真的在动 */
	float GetAppearAlpha() const
	{
		return AppearAlpha;
	}

	/** 正停着 */
	bool IsPaused() const
	{
		return PauseRemaining > 0.f;
	}

	/** 还剩多久继续走，调试命令用 */
	float GetPauseRemaining() const
	{
		return PauseRemaining;
	}

	/** 已经换过几条路线，调试命令用 */
	int32 GetHandoffCount() const
	{
		return HandoffCount;
	}

	/** 还剩多久开始起身，调试命令用 */
	float GetKnockedRemaining() const
	{
		return KnockedRemaining;
	}

	/**
	 * 前方近处有别的路人、玩家或场景障碍，该让一让；不等待车辆
	 * 躺在地上的路人不算：绕不过去，而且他过几秒自己就起来了
	 * 公开是为了调试命令和回归测试能直接问它，它只读、不改状态
	 */
	bool IsBlockedAhead(bool* bFixedObstacle = nullptr) const;

	/** 正在为前方障碍让路，调试命令用 */
	bool IsYielding() const
	{
		return YieldElapsed > 0.f;
	}

	/** 正在起身，调试命令用 */
	bool IsStandingUp() const
	{
		return Phase == EDeliveryPedestrianPhase::StandingUp;
	}

	/** 起身进度 0~1，调试命令用 */
	float GetGetUpAlpha() const;
	float GetFallAlpha() const { return FMath::Clamp(FallElapsed / FMath::Max(FallDuration, 0.1f), 0.f, 1.f); }

	/** 这一趟用的是程序化步态还是动画资产，调试命令用来区分"没动作"的原因 */
	bool IsUsingProceduralGait() const
	{
		return bProceduralGait;
	}

  protected:
	virtual void BeginPlay() override;

	/** 配了走路动画时使用的身体 */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Components")
	TObjectPtr<USkeletalMeshComponent> Mesh;

	/** 没配走路动画时使用的身体，骨骼由 C++ 逐帧写 */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Components")
	TObjectPtr<UPoseableMeshComponent> PoseableBody;

	/**
	 * 挨打用的碰撞体
	 * 走路时身体是运动学的、没有物理刚体，拳头的球体扫掠扫不到任何东西，
	 * 所以要有一个查询用的胶囊替它挨打。**只查询不阻挡**：
	 * 开了物理阻挡的话路人会把玩家的布娃娃顶翻，而他自己撞上也不会让路
	 */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Components")
	TObjectPtr<UCapsuleComponent> HitBox;

  private:
	/** 把行人摆到样条上 Distance 处并贴地，朝向取样条切线 */
	void UpdateTransform(float Distance);

	/** 在生成点周围游走时，贴地并平滑转向下一个短距离目标。 */
	void UpdateRoamTransform(const FVector& DesiredLocation, float DeltaTime);
	bool IsRoamStepBlocked(const FVector& From, const FVector& To) const;
	void ReverseRoamTarget();

	/**
	 * 推进走走停停，返回这一帧实际走过的距离（停着就是 0）
	 * 停顿**不允许发生在出入场窗口里**：那两个窗口按距离算缩放，
	 * 在里面停住会卡成一个半大小的人站着不动
	 */
	float UpdateWalkAndPause(float DeltaTime, float Length);

	/** 重新抽下一次停顿还有多远 */
	void ScheduleNextPause();

	/**
	 * 现在在出入场窗口里
	 * 那两个窗口按**距离**算缩放，在里面停住会卡成一个半大小的人站着不动，
	 * 所以停顿和让路都不许发生在里面
	 */
	bool IsInAppearWindow(float Length) const;

	/** 站住时的小动作：躯干轻微晃动，别的骨骼保持参考姿势 */
	void UpdateIdleSway(float DeltaTime);

	/**
	 * 走到端点：能接上下一条就换过去继续走，接不上返回 false 由调用方退场
	 * 换线时**不重新淡入**：路人正站在新路线的端点上，照常走出入场窗口的话
	 * 会在路口原地缩到看不见再长回来
	 */
	bool TryHandoff();

	/**
	 * 向下探路面，返回命中高度
	 * 只查 WorldStatic/WorldDynamic：路人不该被玩家胶囊或车辆抬起来，
	 * 那会让他在车流经过时忽然浮空（电瓶车受击那段踩过同一个坑）
	 */
	bool TraceGroundZ(const FVector& Probe, float& OutZ) const;

	/** 推进出入场缩放 */
	void UpdateAppearScale(float Distance);

	/** 把缩放系数写进实际缩放，服务器与客户端共用这一处 */
	void ApplyAppearScale();

	/**
	 * 缩放不在 ReplicatedMovement 里（它只带位置、旋转和速度），所以要单独复制
	 * 不复制的表现是客户端一个路人都看不见：他们在服务器上被缩放成原尺寸，
	 * 客户端却停在生成时那个接近零的缩放上，而位置一直在正常更新
	 */
	UFUNCTION()
	void OnRep_AppearAlpha();
	UFUNCTION()
	void OnRep_FadeAppearance();
	void InstallFadeMaterial();

	/** 开始跌倒；普通路人用程序化动作，可选物理布娃娃。 */
	void StartKnockdown();

	/** 推进跌倒与等待，时间到了就开始爬起。 */
	void UpdateKnockdown(float DeltaTime);
	void UpdateFallAnimation(float DeltaTime);

	UFUNCTION()
	void OnRep_KnockedDown();

	/**
	 * 从布娃娃回到运动学，开始起身；服务器和客户端各自调用一次
	 * **必须先关物理再写变换**：顺序反了的话写进去的位置会被物理在同一帧拽回去
	 */
	void StartGetUp();

	/** 推进起身：刚体翻正 + 走回线上 + 蹲起姿势 */
	void UpdateGetUp(float DeltaTime);

	/** 起身结束，回到正常行走 */
	void FinishGetUp();

	/** 记下参考姿势下几根关键骨骼的组件空间旋转，程序化步态在它上面叠角度 */
	void CacheGaitReferencePose();

	/**
	 * 按走过的路程推进步态相位并写骨骼
	 * 全部在组件空间下叠加，不碰骨骼各自的局部轴向——这几份模型来自 Blender，
	 * 哪个局部轴是"摆腿轴"并不保证一致，而组件空间的 X 永远是角色的正前方
	 */
	void UpdateProceduralGait(float DistanceDelta);

	/** 在一根骨骼的参考朝向上叠加一个绕组件空间某轴的增量旋转 */
	void AddBoneSwing(FName BoneName, const FVector& Axis, float AngleDegrees);

	/**
	 * 同上，但增量是现成的四元数，用来把"垂手"和"前后摆"一次写进去
	 * 分两次 AddBoneSwing 是不行的：每一次都从参考朝向重算再整份写回，
	 * 后一次会把前一次抹掉（看起来像"垂手那一步没生效"）
	 */
	void SetBoneDelta(FName BoneName, const FQuat& Delta);

	/** 按实测的绑定方向算出"把上臂转到体侧"的增量，只在换网格时做一次 */
	void CacheArmRestDeltas();

	/** 生成这一趟的路线，持有弱引用：路线被删掉时行人自己退场而不是崩 */
	UPROPERTY()
	TWeakObjectPtr<ADeliveryPedestrianRoute> Route;

	/** 沿样条的行进距离，始终按样条自己的 0→Length 方向记，反向走的只是增量取负 */
	float DistanceAlongRoute = 0.f;

	/** 普通路人的下一个随机目的地，始终在所属生成点的活动圆内。 */
	FVector RoamTarget = FVector::ZeroVector;

	/** 普通路人出生后的放大进度，不用样条两端的出入场窗口。 */
	float RoamAppearElapsed = 0.f;

	/** true 表示沿样条 0→Length 方向走 */
	bool bForward = true;

	float WalkSpeed = 0.f;
	float BaseWalkSpeed = 0.f;

	/** 动画原本对应的行走速度，用来把播放速率对齐实际速度，避免滑步 */
	float AnimReferenceSpeed = 0.f;

	EDeliveryPedestrianPhase Phase = EDeliveryPedestrianPhase::Appearing;

	/** 当前缩放系数，0 = 看不见、1 = 原尺寸 */
	UPROPERTY(ReplicatedUsing = OnRep_AppearAlpha)
	float AppearAlpha = 0.f;

	UPROPERTY(ReplicatedUsing = OnRep_FadeAppearance)
	bool bFadeAppearance = false;
	UPROPERTY(Transient)
	TObjectPtr<UMaterialInterface> OriginalMaterial;
	UPROPERTY(Transient)
	TObjectPtr<UMaterialInstanceDynamic> FadeMaterialInstance;

	/** 出入场窗口以外 Alpha 恒为 1，不再发包，所以这一项的带宽只花在两端 */
	UPROPERTY(Replicated)
	float ReplicatedMinScale = 0.02f;

	/** 进场时记下的原始缩放，缩放曲线乘在它上面而不是直接写 1，免得抹掉手调过的大小 */
	FVector BaseScale = FVector::OneVector;

	/** 这一趟是否走程序化步态（没有配动画资产时） */
	bool bProceduralGait = false;

	/**
	 * 已经被打飞
	 * 复制给客户端，让各端切换跌倒与起身姿势；位置仍由服务器复制。
	 */
	UPROPERTY(ReplicatedUsing = OnRep_KnockedDown)
	bool bKnockedDown = false;

	/** 受击方向与可选布娃娃的冲量。 */
	UPROPERTY(Replicated)
	FVector_NetQuantize100 KnockImpulse = FVector::ZeroVector;

	/** 着力点，决定被打中的部位 */
	UPROPERTY(Replicated)
	FVector_NetQuantize100 KnockPoint = FVector::ZeroVector;

	/** 这一趟用的网格，被打飞时要换到 SkeletalMesh 槽位上去模拟 */
	UPROPERTY()
	TObjectPtr<USkeletalMesh> ActiveMesh;

	/** 躺够之前的剩余秒数，归零后起身 */
	float KnockedRemaining = 0.f;
	float FallElapsed = 0.f;
	FVector FallStartLocation = FVector::ZeroVector;
	FVector FallEndLocation = FVector::ZeroVector;
	FQuat FallStartRotation = FQuat::Identity;
	FQuat FallEndRotation = FQuat::Identity;
	bool bSimulatingKnockdown = false;

	/** 已经为前方的障碍让了多久（秒），超过 MaxYieldSeconds 就硬挤过去 */
	float YieldElapsed = 0.f;

	/** 起身已经进行了多久（秒） */
	float GetUpElapsed = 0.f;

	/**
	 * 起身的起点与终点
	 *
	 * 起点取**躺着时骨盆的位置**，不取 actor 原点：根骨被物理带着飞，
	 * actor 原点可能停在半空或者墙里，从那儿起身会看到人凭空跳一下。
	 * 样条模式回到离骨盆最近的线段点；普通模式回到所属活动圆内。
	 */
	FVector GetUpStartLocation = FVector::ZeroVector;
	FVector GetUpTargetLocation = FVector::ZeroVector;

	/**
	 * 起身的起始与目标朝向
	 *
	 * 起始朝向按**实测的身体上轴**（骨盆到脊柱的连线）构造，不写死"趴着就是
	 * pitch 90"：他可能侧躺、也可能仰面，写死一个方向会在一半的情况下
	 * 先翻到另一边再起来。
	 */
	FQuat GetUpStartRotation = FQuat::Identity;
	FQuat GetUpTargetRotation = FQuat::Identity;

	/** 这一趟的走路动画，起身之后要重新播；不存的话起完身人是定格的 */
	UPROPERTY()
	TObjectPtr<UAnimSequence> ActiveWalkAnim;

	/** 步态相位，0~1 对应一个完整步幅 */
	float GaitPhase = 0.f;

	/** 还剩多少秒继续走，大于 0 表示正停着 */
	float PauseRemaining = 0.f;

	/** 再走多远触发下一次停顿（cm） */
	float DistanceUntilPause = 0.f;

	/** 停下时在朝向上叠的偏移角，停一次抽一次 */
	float PauseYawOffset = 0.f;

	/** 站住时的晃动相位，按时间推进（步态那套是按路程，停着就不动了） */
	float IdlePhase = 0.f;

	/** 已经换过几条路线，到上限后在下一个端点退场 */
	int32 HandoffCount = 0;

	/**
	 * 这一段是不是接驳过来的
	 * 决定进场那头要不要淡入：换线进来的不淡入，否则在路口会缩掉再长回来
	 */
	bool bEnteredByHandoff = false;

	/** 参考姿势下的组件空间骨骼旋转，键是骨骼名 */
	TMap<FName, FQuat> GaitReferenceRotations;

	/**
	 * 把上臂从绑定方向（T 字形平举）转到体侧的增量，左右各一个
	 * 不写死"绕 X 轴转 90°"：那要求绑定姿势正好是标准 T 字、而且左臂正好朝 -Y，
	 * 换一份 A 字姿势的模型就会把手转到背后。这里按实测的上臂指向求解
	 */
	FQuat LeftArmRestDelta = FQuat::Identity;
	FQuat RightArmRestDelta = FQuat::Identity;

	/** 参考姿势下髋部的组件空间位置，上下起伏叠在它上面 */
	FVector HipReferenceLocation = FVector::ZeroVector;

	/** 髋部骨骼名是否解析成功；不成功就不做起伏，而不是写到一根不存在的骨头上 */
	bool bHasHipBone = false;
};
