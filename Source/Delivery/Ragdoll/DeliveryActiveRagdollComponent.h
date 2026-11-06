// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Components/ActorComponent.h"
#include "Ragdoll/Network/DeliveryRagdollSync.h"
#include "Ragdoll/Movement/DeliveryRagdollMovement.h"
#include "Ragdoll/Movement/DeliveryRagdollRecovery.h"
#include "Ragdoll/Combat/DeliveryRagdollPunch.h"
#include "TimerManager.h"
#include "Combat/DeliveryBoxingPose.h"
#include "DeliveryActiveRagdollComponent.generated.h"

struct FDeliveryPoseDebug;
class USoundBase;
class UCapsuleComponent;
class UPhysicsControlComponent;
class USkeletalMeshComponent;
class UAnimSequence;
class ADeliveryHandheldItem;

UENUM(BlueprintType)
enum class EDeliveryRagdollControlMode : uint8
{
	Disabled,
	Active,
	Limp,
	Recovering
};

USTRUCT(BlueprintType)

struct FDeliveryRagdollBones
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Ragdoll")
	FName Hips = TEXT("Hips");

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Ragdoll")
	FName Spine = TEXT("Spine");

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Ragdoll")
	FName Head = TEXT("Head");

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Ragdoll")
	FName LeftUpLeg = TEXT("LeftUpLeg");

	/** 迈步落点刚体；物理资产没有脚时，启动会改成该腿最远端的现有刚体（通常是小腿） */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Ragdoll")
	FName LeftFoot = TEXT("LeftFoot");

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Ragdoll")
	FName RightUpLeg = TEXT("RightUpLeg");

	/** 迈步落点刚体；物理资产没有脚时，启动会改成该腿最远端的现有刚体（通常是小腿） */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Ragdoll")
	FName RightFoot = TEXT("RightFoot");

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Ragdoll")
	FName LeftArm = TEXT("LeftArm");

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Ragdoll")
	FName RightArm = TEXT("RightArm");
};

/**
 * 全身物理角色：网格刚体持续模拟，Physics Control 当作关节肌肉，用目标位置和目标旋转去拉身体
 *
 * 走路不是动画位移，也不是 IK；髋先走、脚后追；问地的方向沿坡面法线，只处理较缓的斜面
 * 陡坡改由服务器触发短暂的全物理翻滚；台阶和用手攀爬不在步态范围内
 */
/**
 * 每次落脚时通知左右脚和落点，用于播放脚步声
 * 各客户端按本地物理模拟触发，不额外同步事件
 */
DECLARE_DYNAMIC_MULTICAST_DELEGATE_TwoParams(FOnDeliveryFootPlanted, bool, bLeft, FVector, Location);

/**
 * 管理持续模拟的物理身体，行走和出拳都通过控制目标驱动刚体
 * 物理前：网络修正、倒地恢复、髋部与脚步目标、手臂姿态
 * 物理后：服务器采集姿态，角色根位置跟随实际髋部
 * Movement 保存步态，Punch 保存出拳，Recovery 保存起身，Network 保存快照
 * 本组件负责身体运动，命中检测与伤害分别由战斗组件和拳击技能处理
 */
UCLASS(ClassGroup = (Delivery), meta = (BlueprintSpawnableComponent))

class DELIVERY_API UDeliveryActiveRagdollComponent : public UActorComponent
{
	GENERATED_BODY()

  public:
	friend class FDeliveryLegIsolationTest;
	friend class FDeliveryNetworkCorrectionTest;

#if WITH_EDITOR
    // 只读当前物理姿态，供编辑器显示实际位置与控制目标
    bool ReadPoseDebug(FDeliveryPoseDebug& Out) const;
#endif

	UDeliveryActiveRagdollComponent();

	virtual void BeginPlay() override;
	virtual void TickComponent(float DeltaTime, ELevelTick TickType,
	                           FActorComponentTickFunction* ThisTickFunction) override;
	virtual void RegisterComponentTickFunctions(bool bRegister) override;

	// 网络复制与更新入口
	virtual void GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const override;

	// 身体驱动启停
	// 身体状态与移动
	UFUNCTION(BlueprintCallable, BlueprintAuthorityOnly, Category = "Ragdoll")
	void StartRagdoll();

	UFUNCTION(BlueprintCallable, BlueprintAuthorityOnly, Category = "Ragdoll")
	void StopRagdoll();

	UFUNCTION(BlueprintCallable, BlueprintAuthorityOnly, Category = "Ragdoll")
	void SetLimp(bool bLimp);

	// 移动与跳跃入口
	UFUNCTION(BlueprintCallable, Category = "Ragdoll")
	void SetMoveInput(FVector2D RightForward);

	UFUNCTION(BlueprintCallable, Category = "Ragdoll")
	void AddImpulse(FVector Impulse, bool bVelocityChange = true);

	/** 仅服务器在确认受到一拳后调用；受击者自己承受冲量并短暂放松直立控制 */
	void ApplyMeleeImpact(const FVector& Impulse, const FVector& ImpactPoint);

	// 出拳姿态接口
	// 身体出拳控制
	/** 开始直拳：记录攻击方向和手侧 */
	void BeginBodyDrivenPunch(FVector AimDirection, float HandSide);

	bool CanDrivePunch(float HandSide) const
	{
		return bIsActive && ReplicatedControlMode == EDeliveryRagdollControlMode::Active &&
		       !Recovery.IsRecoveringArms() && BoxingPose.IsReady(HandSide > 0 ? 0 : 1);
	}

	bool IsPunchRecovering() const
	{
		return BoxingPose.HasPendingPunchRecovery();
	}

	FVector GetBodyForward() const
	{
		return FRotator(0, CurrentFacingYaw, 0).Vector();
	}

	// 挥斧动作接口
	// 斧头动作
	/** 请求斧头下劈，由服务器校验当前持物和动作状态 */
	void RequestAxeSwing();
	/** 返回当前手臂动画混合权重，动作无效时为 0 */
	float GetAxeSwingWeight() const;

	/** 下劈参考动画，仅提取双臂姿态 */
	UPROPERTY(EditDefaultsOnly, Category = "Ragdoll|Axe")
	TObjectPtr<UAnimSequence> AxeSwingAnimation;

	UPROPERTY(EditDefaultsOnly, Category = "Ragdoll|Axe", meta = (ClampMin = "0.1", ClampMax = "3", DisplayName = "斧头动画播放倍率 (AxeSwingPlayRate)", ToolTip = "同时影响动画进度与破门检测等待，不改变源动画。", UIMax = "3"))
	float AxeSwingPlayRate = 1.8f;

	UPROPERTY(EditDefaultsOnly, Category = "Ragdoll|Axe", meta = (ClampMin = "0.01", DisplayName = "斧头淡入时间 (AxeSwingBlendIn)", ToolTip = "单位是源动画秒数，实际等待受播放倍率影响。", Units = "s", UIMax = "0.5"))
	float AxeSwingBlendIn = 0.12f;

	UPROPERTY(EditDefaultsOnly, Category = "Ragdoll|Axe", meta = (ClampMin = "0.01", DisplayName = "斧头淡出时间 (AxeSwingBlendOut)", ToolTip = "单位是源动画秒数。", Units = "s", UIMax = "0.6"))
	float AxeSwingBlendOut = 0.2f;

	UFUNCTION(Server, Reliable)
	void ServerRequestAxeSwing();

	UFUNCTION(NetMulticast, Reliable)
	void MulticastStartAxeSwing(ADeliveryHandheldItem* Axe, float ServerStartTime);

	/** 检查持物、动作互斥和下劈间隔是否满足启动条件 */
	bool CanStartAxeSwing() const;
	/** 采样当前下劈姿态；仅在返回 真 时使用 Out 中的方向数据 */
	bool SampleAxeSwing(FDeliveryAnimationArmPose& Out);
	/** 返回按播放速度换算的动画时间，单位为秒；尚未开始或世界无效时为 -1 */
	float GetAxeSwingTime() const;
	/** 在服务器执行下劈的破门检测 */
	void TryAxeDoorHit();
	TWeakObjectPtr<ADeliveryHandheldItem> SwingAxe;
	/** 服务器起始时间，单位为秒；负值表示尚未开始 */
	float AxeSwingStartTime = -1.f;
	FTimerHandle AxeDoorHitTimer;

	/** 释放直拳：标记前送阶段 */
	void ReleaseBodyDrivenPunch();

	/** 清除额外的出拳姿势，回到普通站立/移动控制 */
	void EndBodyDrivenPunch();

	UFUNCTION(BlueprintPure, Category = "Ragdoll")

	bool IsRagdollActive() const
	{
		return bIsActive;
	}

	UFUNCTION(BlueprintPure, Category = "Ragdoll")

	EDeliveryRagdollControlMode GetControlMode() const
	{
		return ReplicatedControlMode;
	}

	// 脚步通知与声音
	/** 脚落地时广播；音效、脚印、扬尘都可以接这里 */
	UPROPERTY(BlueprintAssignable, Category = "Delivery|Ragdoll")
	FOnDeliveryFootPlanted OnFootPlanted;

	/**
	 * 脚步声的候选音效，播放时避免连续选择同一条
	 * 配置多条音效可减少重复感
	 */
	UPROPERTY(EditDefaultsOnly, Category = "Audio|Footstep")
	TArray<TObjectPtr<USoundBase>> FootstepSounds;

	/**
	 * 触发脚步声所需的最小水平步距，单位为厘米
	 * 站立时为保持平衡产生的小幅补步不播放声音
	 */
	UPROPERTY(EditDefaultsOnly, Category = "Audio|Footstep", meta = (ClampMin = "0.0"))
	float FootstepMinStride = 18.0f;

	/** 到这个迈步距离时音量给满（cm）；碎步轻、大步重，不是一刀切同一个音量 */
	UPROPERTY(EditDefaultsOnly, Category = "Audio|Footstep", meta = (ClampMin = "1.0"))
	float FootstepFullStride = 70.0f;

	UPROPERTY(EditDefaultsOnly, Category = "Audio|Footstep", meta = (ClampMin = "0.0"))
	float FootstepVolume = 1.0f;

	/**
	 * 左右脚的变调；两只脚故意给不同的音高：吸盘那种卡通音效一高一低
	 * 交替才听得出"左右左右"的节奏，两边同音会糊成一串
	 */
	UPROPERTY(EditDefaultsOnly, Category = "Audio|Footstep", meta = (ClampMin = "0.1"))
	float FootstepLeftPitch = 0.94f;

	UPROPERTY(EditDefaultsOnly, Category = "Audio|Footstep", meta = (ClampMin = "0.1"))
	float FootstepRightPitch = 1.06f;

	// 着地与跳跃
	/** 盆骨是否贴在站立高度附近；跳跃过程中恒为 假，二段跳就是被这里挡住的 */
	UFUNCTION(BlueprintPure, Category = "Ragdoll|跳跃")
	bool IsGrounded() const;

	/** 起跳；不在地上、正在跳、或处于瘫软状态时返回 假，不做任何事 */
	UFUNCTION(BlueprintCallable, Category = "Ragdoll|跳跃")
	bool TryStartJump();

	UFUNCTION(BlueprintPure, Category = "Ragdoll|跳跃")

	bool IsJumping() const
	{
		return Movement.bJumping;
	}

  protected:
	// 组件与骨骼配置
	UPROPERTY(VisibleAnywhere, Category = "Ragdoll")
	TObjectPtr<UPhysicsControlComponent> PhysicsControl;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Ragdoll")
	FDeliveryRagdollBones Bones;

	UPROPERTY(EditAnywhere, Category = "Ragdoll")
	bool bStartOnBeginPlay = true;

	UPROPERTY(EditAnywhere, Category = "Ragdoll")
	bool bPlaceOnGroundAtStart = true;

	// 移动与身体跟随
	UPROPERTY(EditAnywhere, Category = "Ragdoll|移动", meta = (ClampMin = "0.0", DisplayName = "目标移动速度 (DesiredMoveSpeed)", ToolTip = "期望的水平移动速度；实际速度受物理和步态影响。先小幅调整并观察是否跟得上。", Units = "cm/s", UIMax = "600"))
	float DesiredMoveSpeed = 360.0f;

	/** 用速度误差估计髋部沿坡领前目标时，向前看多长时间；时间越长，目标离当前髋越远 */
	UPROPERTY(EditAnywhere, Category = "Ragdoll|移动", meta = (ClampMin = "0.0", DisplayName = "髋部目标提前时间 (TargetLeadTime)", ToolTip = "按速度误差估计目标提前量；增大可能让身体更急于前冲。", Units = "s", UIMax = "0.3"))
	float TargetLeadTime = 0.08f;

	/** 髋部领前目标相对当前髋的最大距离，避免电机一次性把身体拉得太远 */
	UPROPERTY(EditAnywhere, Category = "Ragdoll|移动", meta = (ClampMin = "0.0", DisplayName = "髋部目标最大提前距离 (MaxTargetLead)", ToolTip = "限制移动目标离当前髋部的距离，避免一次拉得太远。", Units = "cm", UIMax = "60"))
	float MaxTargetLead = 18.0f;

	UPROPERTY(EditAnywhere, Category = "Ragdoll|移动", meta = (ClampMin = "0.0", ClampMax = "20.0", DisplayName = "起步前倾角 (AccelerationLeanAngle)", ToolTip = "只用于起步前倾，过大会前扑。", Units = "deg", UIMax = "20"))
	float AccelerationLeanAngle = 4.5f;

	/** 起步时短暂前倾；不要按当前速度误差一直前倾，否则下坡失速会形成前扑的正反馈 */
	UPROPERTY(EditAnywhere, Category = "Ragdoll|移动", meta = (ClampMin = "0.0", DisplayName = "起步前倾持续时间 (AccelerationLeanDuration)", ToolTip = "起步前倾逐渐衰减的时间。", Units = "s", UIMax = "1"))
	float AccelerationLeanDuration = 0.4f;

	UPROPERTY(EditAnywhere, Category = "Ragdoll|移动", meta = (ClampMin = "0.1", DisplayName = "转向响应速度 (TurnResponsiveness)", ToolTip = "目标朝向的平滑响应，过快会突然扭身。", UIMax = "15"))
	float TurnResponsiveness = 5.0f;

	UPROPERTY(EditAnywhere, Category = "Ragdoll|移动", meta = (ClampMin = "0.0", ClampMax = "8.0"))
	float BouncyPelvisWobbleAngle = 2.2f;

	/** 拖拽倒地角色时稍微屈膝、前倾，让单手能接近地面的身体；不影响普通物品托举 */
	UPROPERTY(EditAnywhere, Category = "Ragdoll|抓取", meta = (ClampMin = "0.0", ClampMax = "45.0"))
	float DragReachCrouchHeight = 25.0f;

	UPROPERTY(EditAnywhere, Category = "Ragdoll|抓取", meta = (ClampMin = "0.0", ClampMax = "20.0"))
	float DragReachLeanAngle = 8.0f;

	UPROPERTY(EditAnywhere, Category = "Ragdoll|移动", meta = (ClampMin = "0.0", ClampMax = "20.0"))
	float LooseTorsoSwingAngle = 7.5f;

	UPROPERTY(EditAnywhere, Category = "Ragdoll|移动", meta = (ClampMin = "0.0", ClampMax = "8.0"))
	float SmoothBounceHeight = 1.8f;

	// 步幅与落脚
	UPROPERTY(EditAnywhere, Category = "Ragdoll|步态", meta = (ClampMin = "0.0", DisplayName = "目标步幅 (ControlledStrideLength)", ToolTip = "每步目标距离；移动加速时同时观察落脚是否能完成。", Units = "cm", UIMax = "160"))
	float ControlledStrideLength = 108.0f;

	UPROPERTY(EditAnywhere, Category = "Ragdoll|步态", meta = (ClampMin = "0.0", DisplayName = "站姿左右间距参数 (StableComedyStance)", ToolTip = "用于生成左右站姿落点；修改后检查是否八字过宽。", Units = "cm", UIMax = "70"))
	float StableComedyStance = 32.0f;

	UPROPERTY(EditAnywhere, Category = "Ragdoll|步态", meta = (ClampMin = "0.0", DisplayName = "抬脚高度 (ControlledStepHeight)", ToolTip = "摆动脚离地抬升高度；过大会让步态夸张。", Units = "cm", UIMax = "80"))
	float ControlledStepHeight = 38.0f;

	UPROPERTY(EditAnywhere, Category = "Ragdoll|步态", meta = (ClampMin = "0.0", DisplayName = "脚部最小侧向距离 (StableMinimumFootSide)", ToolTip = "防止脚目标过于靠近身体中线。", Units = "cm", UIMax = "40"))
	float StableMinimumFootSide = 16.0f;

	UPROPERTY(EditAnywhere, Category = "Ragdoll|步态", meta = (ClampMin = "0.0"))
	float MovingCrossingRecoveryMargin = 6.0f;

	UPROPERTY(EditAnywhere, Category = "Ragdoll|步态", meta = (ClampMin = "0.05", DisplayName = "单步持续时间 (ControlledStrideDuration)", ToolTip = "完成一次摆腿的时间，太短可能来不及落地。", Units = "s", UIMax = "1"))
	float ControlledStrideDuration = 0.34f;

	UPROPERTY(EditAnywhere, Category = "Ragdoll|步态", meta = (ClampMin = "0.0", DisplayName = "停步补脚距离 (StopRecoveryDistance)", ToolTip = "停步后脚偏离目标较远时进行补步。", Units = "cm", UIMax = "80"))
	float StopRecoveryDistance = 34.0f;

	UPROPERTY(EditAnywhere, Category = "Ragdoll|步态", meta = (ClampMin = "0.0", ClampMax = "1.0", DisplayName = "允许迈步的直立阈值 (MinimumStepUprightDot)", ToolTip = "身体向上方向与世界向上的点积下限。越低越允许倾斜时迈步。", UIMax = "1"))
	float MinimumStepUprightDot = 0.35f;

    // 正常收拳结束后，手臂持续偏离目标才进入恢复
    UPROPERTY(EditAnywhere, Category = "Ragdoll|恢复", meta = (ClampMin = "10", ClampMax = "180", Units = "deg", DisplayName = "手臂异常角度"))
    float ArmRecoveryErrorAngle = 70.f;

    UPROPERTY(EditAnywhere, Category = "Ragdoll|恢复", meta = (ClampMin = "0.1", Units = "s", DisplayName = "手臂异常确认时间"))
    float ArmRecoveryConfirmTime = .45f;

    UPROPERTY(EditAnywhere, Category = "Ragdoll|恢复", meta = (ClampMin = "0.1", Units = "s", DisplayName = "手臂恢复时间"))
    float ArmRecoveryDuration = .65f;

    // 脚交叉恢复
    UPROPERTY(EditAnywhere, Category = "Ragdoll|恢复", meta = (ClampMin = "0.1", Units = "cm", DisplayName = "交叉确认距离", ToolTip = "两脚在身体左右方向上的顺序颠倒超过此距离才累计异常时间"))
    float FootCrossDistance = 3.f;

    UPROPERTY(EditAnywhere, Category = "Ragdoll|恢复", meta = (ClampMin = "0.05", Units = "s", DisplayName = "交叉确认时间"))
    float FootCrossConfirmTime = .18f;

    UPROPERTY(EditAnywhere, Category = "Ragdoll|恢复", meta = (ClampMin = "0.05", Units = "s", DisplayName = "双脚稳定等待"))
    float FootRecoveryStableTime = .15f;

    UPROPERTY(EditAnywhere, Category = "Ragdoll|恢复", meta = (ClampMin = "0.5", Units = "s", DisplayName = "补步最长时间", ToolTip = "超过此时间先退出，避免无法落脚时一直阻止玩家移动"))
    float FootRecoveryMaxTime = 2.f;

	// 坡面支撑与翻滚
	/** 可行走斜坡上已落地支撑脚的切向摩擦辅助；0 关闭，不锁脚、不吸附地面 */
	UPROPERTY(EditAnywhere, Category = "Ragdoll|步态", meta = (ClampMin = "0.0", DisplayName = "坡地防滑辅助强度 (SlopeSupportTraction)", ToolTip = "对支撑脚提供切向摩擦辅助；不是锁脚或吸附。", UIMax = "30"))
	float SlopeSupportTraction = 12.0f;

	UPROPERTY(EditAnywhere, Category = "Ragdoll|步态", meta = (ClampMin = "0.0", DisplayName = "坡地防滑加速度上限 (SlopeSupportMaxAcceleration)", ToolTip = "限制防滑辅助加速度，过大可能让脚粘住。", Units = "cm/s^2", UIMax = "1800"))
	float SlopeSupportMaxAcceleration = 900.0f;

	/**
	 * 允许沿坡面行走的最大坡角（度）；坡角是地面法线与世界向上的夹角：
	 * 法线竖直分量 = cos(坡角)；58 度时 cos(58°) ≈ 0.530
	 * 原先用 0.82，对应 arccos(0.82) ≈ 35 度，较缓斜面也容易被判成走不了
	 */
	UPROPERTY(EditAnywhere, Category = "Ragdoll|步态", meta = (ClampMin = "0.0", ClampMax = "75.0"))
	float MaxWalkableSlopeDegrees = 58.0f;

	/** 顺坡移动超过此坡角时，不再强迫脚迈步，而是进入物理翻滚 */
	UPROPERTY(EditAnywhere, Category = "Ragdoll|陡坡翻滚", meta = (ClampMin = "20.0", ClampMax = "75.0"))
	float TumbleSlopeDegrees = 48.0f;

	UPROPERTY(EditAnywhere, Category = "Ragdoll|陡坡翻滚", meta = (ClampMin = "0.0"))
	float TumbleDownhillSpeed = 120.0f;

	UPROPERTY(EditAnywhere, Category = "Ragdoll|陡坡翻滚", meta = (ClampMin = "0.0"))
	float TumbleEntryDelay = 0.18f;

	UPROPERTY(EditAnywhere, Category = "Ragdoll|陡坡翻滚", meta = (ClampMin = "0.0", ClampMax = "60.0"))
	float TumbleRecoverySlopeDegrees = 30.0f;

	UPROPERTY(EditAnywhere, Category = "Ragdoll|陡坡翻滚", meta = (ClampMin = "0.0"))
	float TumbleRecoverySpeed = 140.0f;

	UPROPERTY(EditAnywhere, Category = "Ragdoll|陡坡翻滚", meta = (ClampMin = "0.0"))
	float TumbleMinimumDuration = 0.9f;

	UPROPERTY(EditAnywhere, Category = "Ragdoll|陡坡翻滚", meta = (ClampMin = "0.0"))
	float TumbleRecoveryDelay = 0.45f;

	/** 起滚时给胸部一次轻微角速度；之后只靠重力和碰撞滚动 */
	UPROPERTY(EditAnywhere, Category = "Ragdoll|陡坡翻滚", meta = (ClampMin = "0.0"))
	float TumbleStartAngularSpeed = 2.0f;

	// 身体控制强度
	UPROPERTY(EditAnywhere, Category = "Ragdoll|肌肉", meta = (ClampMin = "0.0", DisplayName = "髋部位置驱动强度 (RootLinearStrength)", ToolTip = "髋部追随目标位置的强度；不等于手臂出拳强度。", Units = "Hz", UIMax = "15"))
	float RootLinearStrength = 4.6f;

	UPROPERTY(EditAnywhere, Category = "Ragdoll|肌肉", meta = (ClampMin = "0.0", DisplayName = "髋部直立强度 (StabilizedRootAngularStrength)", ToolTip = "髋部保持目标方向的强度，过低易倒，过高会僵硬。", Units = "Hz", UIMax = "25"))
	float StabilizedRootAngularStrength = 10.5f;

	UPROPERTY(EditAnywhere, Category = "Ragdoll|肌肉", meta = (ClampMin = "0.0"))
	float LooseWaistFollowStrength = 5.8f;

	UPROPERTY(EditAnywhere, Category = "Ragdoll|肌肉", meta = (ClampMin = "0.0"))
	float LooseComedyBodyStrength = 3.4f;

	UPROPERTY(EditAnywhere, Category = "Ragdoll|肌肉", meta = (ClampMin = "0.0"))
	float UprightHeadStrength = 11.0f;

	UPROPERTY(EditAnywhere, Category = "Ragdoll|肌肉", meta = (ClampMin = "0.0", DisplayName = "头部阻尼比例 (StableHeadDampingRatio)", ToolTip = "减少头部旋转振荡。", UIMax = "3"))
	float StableHeadDampingRatio = 1.6f;

	UPROPERTY(EditAnywhere, Category = "Ragdoll|姿势", meta = (ClampMin = "0.0", ClampMax = "15.0"))
	float StandingHeadCorrectionAngle = 6.0f;

	UPROPERTY(EditAnywhere, Category = "Ragdoll|肌肉", meta = (ClampMin = "0.0"))
	float StableHipStrength = 6.5f;

	UPROPERTY(EditAnywhere, Category = "Ragdoll|肌肉", meta = (ClampMin = "0.0"))
	float ArticulatedKneeStrength = 6.0f;

	UPROPERTY(EditAnywhere, Category = "Ragdoll|肌肉", meta = (ClampMin = "0.0"))
	float ComedyArmStrength = 3.0f;

	// 物理受击
	/** 受击后重点减弱胸和躯干电机；髋、腿只轻微让位 */
	UPROPERTY(EditAnywhere, Category = "Ragdoll|受击", meta = (ClampMin = "0.0", ClampMax = "1.0", DisplayName = "受击上身强度比例 (HitReactionStrengthMultiplier)", ToolTip = "受击时临时降低上身驱动。越小越松，不直接改变伤害。", UIMax = "1"))
	float HitReactionStrengthMultiplier = 0.2f;

	/** 受击时髋部电机保留的强度；高于上半身，避免整个人飞走 */
	UPROPERTY(EditAnywhere, Category = "Ragdoll|受击", meta = (ClampMin = "0.0", ClampMax = "1.0"))
	float HitReactionPelvisStrengthMultiplier = 0.85f;

	/** 两脚仍盯住落点，但位置电机可稍微让位；给小一点，脚会被拖着走得更多 */
	UPROPERTY(EditAnywhere, Category = "Ragdoll|受击", meta = (ClampMin = "0.0", ClampMax = "1.0"))
	float HitReactionFootStrengthMultiplier = 0.45f;

	/** 受击时两脚沿被打飞的方向挪多远，做出踉跄一步；0 就是原地钉住 */
	UPROPERTY(EditAnywhere, Category = "Ragdoll|受击", meta = (ClampMin = "0.0", ClampMax = "200.0"))
	float HitReactionFootSlideDistance = 34.0f;

	/** 受击时髋部目标沿被打方向额外挪多远，让整个人真的被打退一步，不只是上身晃 */
	UPROPERTY(EditAnywhere, Category = "Ragdoll|受击", meta = (ClampMin = "0.0", ClampMax = "200.0"))
	float HitReactionPelvisSlideDistance = 30.0f;

	/** 髋部挪出去/收回来的过渡速度 */
	UPROPERTY(EditAnywhere, Category = "Ragdoll|受击", meta = (ClampMin = "1.0"))
	float HitReactionPelvisSlideSpeed = 10.0f;

	/** 胸部冲量中额外传给髋的比例，制造很短的下身后移 */
	UPROPERTY(EditAnywhere, Category = "Ragdoll|受击", meta = (ClampMin = "0.0", ClampMax = "1.0"))
	float HitReactionPelvisImpulseFraction = 0.12f;

	UPROPERTY(EditAnywhere, Category = "Ragdoll|受击", meta = (ClampMin = "0.0", DisplayName = "受击放松时间 (HitReactionDuration)", ToolTip = "受击驱动降低所持续的时间。", Units = "s", UIMax = "1.5"))
	float HitReactionDuration = 0.4f;

	/** 胸部绕髋部后仰的瞬时角速度变化（弧度/秒） */
	UPROPERTY(EditAnywhere, Category = "Ragdoll|受击", meta = (ClampMin = "0.0"))
	float HitReactionAngularVelocity = 5.5f;

	/**
	 * 额外给胸部一个纯向上的冲量，大小是水平冲量的这个倍数；挨打这段时间上半身电机很松，
	 * 这一下会把上身弹起来，靠重力自己落回去，看起来是上半身带着一跳，而不只是往后倒
	 */
	UPROPERTY(EditAnywhere, Category = "Ragdoll|受击", meta = (ClampMin = "0.0", ClampMax = "3.0"))
	float HitReactionUpwardImpulseFraction = 0.5f;

	/** 受击后转身面向来拳方向的速度；比平时转身快一点，挨打是被动的反射 */
	UPROPERTY(EditAnywhere, Category = "Ragdoll|受击", meta = (ClampMin = "0.0"))
	float HitReactionFacingTurnSpeed = 9.0f;

	// 出拳与搬运时的身体支撑
	/**
	 * 蓄力阶段把出拳侧的肩膀向后拧多少度；纯水平旋转，不弯腰
	 * 这个角度和下面的跟随角度加起来就是身体在一次出拳里横转的总量；给大了，
	 * 伸出去的手臂会被整块躯干横着带过去，看起来是在扇耳光而不是出拳
	 */
	UPROPERTY(EditAnywhere, Category = "Ragdoll|出拳", meta = (ClampMin = "0.0", ClampMax = "60.0", DisplayName = "蓄力转体角 (PunchSideStanceAngle)", ToolTip = "蓄力时胸腰向后拧转；过大容易把直拳带成横摆。", Units = "deg", UIMax = "60"))
	float PunchSideStanceAngle = 8.0f;

	/** 释放时上身向出拳侧带出多少度 */
	UPROPERTY(EditAnywhere, Category = "Ragdoll|出拳", meta = (ClampMin = "0.0", ClampMax = "45.0", DisplayName = "释放转体角 (PunchFollowThroughAngle)", ToolTip = "释放时胸腰跟随出拳转动。", Units = "deg", UIMax = "45"))
	float PunchFollowThroughAngle = 9.0f;

	/** 蓄力时胸腰向拳路反方向轻微后摆，骨盆不参与 */
	UPROPERTY(EditAnywhere, Category = "Ragdoll|出拳", meta = (ClampMin = "0.0", ClampMax = "8.0", DisplayName = "蓄力后摆角 (PunchTorsoWindupLeanAngle)", ToolTip = "胸腰朝拳路反方向轻微后摆，不转动骨盆。", Units = "deg", UIMax = "8"))
	float PunchTorsoWindupLeanAngle = 2.0f;

	/** 释放时胸腰朝拳路前摆；保持小幅度，避免压倒身体 */
	UPROPERTY(EditAnywhere, Category = "Ragdoll|出拳", meta = (ClampMin = "0.0", ClampMax = "15.0", DisplayName = "释放前摆角 (PunchTorsoFollowLeanAngle)", ToolTip = "胸腰朝拳路前摆；身体被带倒时先尝试小幅降低。", Units = "deg", UIMax = "15"))
	float PunchTorsoFollowLeanAngle = 7.0f;

	/** 拧身角度的过渡速度；目标角度是阶跃的，这里决定身体多快跟上去 */
	UPROPERTY(EditAnywhere, Category = "Ragdoll|出拳", meta = (ClampMin = "1.0", DisplayName = "转体过渡速度 (PunchTwistSpeed)", ToolTip = "胸腰旋转向目标过渡的速度。", UIMax = "20"))
	float PunchTwistSpeed = 10.0f;

	/** 释放时髋部少量前压；原地出拳以支撑位置为基准，避免逐帧向前漂移 */
	UPROPERTY(EditAnywhere, Category = "Ragdoll|出拳", meta = (ClampMin = "0.0", ClampMax = "60.0", DisplayName = "出拳髋部前压距离 (PunchLungeDistance)", ToolTip = "髋部向前压的距离；过大会破坏脚下支撑。", Units = "cm", UIMax = "30"))
	float PunchLungeDistance = 4.0f;

	/** 原地出拳时脚的位置支撑强度；外力仍可推动身体 */
	UPROPERTY(EditAnywhere, Category = "Ragdoll|出拳", meta = (ClampMin = "0.0", ClampMax = "1.0", DisplayName = "原地出拳脚部支撑比例 (PunchFootSupportStrength)", ToolTip = "原地出拳时脚的位置支撑；只在允许支撑的状态下生效。", UIMax = "1"))
	float PunchFootSupportStrength = 0.75f;

	/** 髋部被推离支撑点超过此距离就松开脚，避免受击时被吊住 */
	UPROPERTY(EditAnywhere, Category = "Ragdoll|出拳", meta = (ClampMin = "10.0", DisplayName = "脚部支撑解除距离 (PunchSupportBreakDistance)", ToolTip = "髋部偏离支撑点超过此距离就解除，避免受击时被脚吊住。", Units = "cm", UIMax = "100"))
	float PunchSupportBreakDistance = 45.0f;

	/** 前送和收回的速度；要比拧身快，身体先压出去，拳头才跟着有重量 */
	UPROPERTY(EditAnywhere, Category = "Ragdoll|出拳", meta = (ClampMin = "1.0"))
	float PunchLungeSpeed = 12.0f;

	/** 出拳期间胸腔和脊柱的强度；肩膀要有支点，上臂才转得动 */
	UPROPERTY(EditAnywhere, Category = "Ragdoll|出拳", meta = (ClampMin = "0.0", DisplayName = "出拳胸腰支撑强度 (PunchBraceStrength)", ToolTip = "给肩部提供躯干支点；过大可能过硬，过低肩膀跟着拳头晃。", Units = "Hz", UIMax = "30"))
	float PunchBraceStrength = 12.0f;

	/** 躯干绷紧和放松的过渡速度 */
	UPROPERTY(EditAnywhere, Category = "Ragdoll|出拳", meta = (ClampMin = "1.0", DisplayName = "胸腰支撑过渡速度 (PunchBraceSpeed)", ToolTip = "胸腰绷紧和放松的速度。", UIMax = "20"))
	float PunchBraceSpeed = 8.0f;

	/** 托举时胸和脊柱提供肩膀支点；只在持物期间渐进提高，不锁住腿 */
	UPROPERTY(EditAnywhere, Category = "Ragdoll|抓取", meta = (ClampMin = "0.0"))
	float CarryBraceStrength = 20.0f;

	/** 抱箱转身时限制目标身体角速度，避免手臂被镜头瞬间转向扭到背后 */
	UPROPERTY(EditAnywhere, Category = "Ragdoll|抓取", meta = (ClampMin = "1.0"))
	float CarryTurnRate = 160.0f;

	/** 手臂三个姿势的参数：站立走路的 A 姿势、出拳起手的收拳、直拳终点 */
	UPROPERTY(EditAnywhere, Category = "Ragdoll|出拳", meta = (DisplayName = "手臂姿势与驱动 (ArmPose)", ToolTip = "展开调整拳路、节奏、强度和关节限位。这份设置在创建身体控制时复制，修改后重新运行 PIE。"))
	FDeliveryArmPoseSettings ArmPose;

	// 腿部控制与平衡
	UPROPERTY(EditAnywhere, Category = "Ragdoll|肌肉", meta = (ClampMin = "0.0"))
	float LegPullStrength = 3.8f;

	UPROPERTY(EditAnywhere, Category = "Ragdoll|肌肉", meta = (ClampMin = "0.0"))
	float FootFacingStrength = 4.5f;

	UPROPERTY(EditAnywhere, Category = "Ragdoll|肌肉", meta = (ClampMin = "0.0"))
	float StableMuscleDampingRatio = 1.35f;

	UPROPERTY(EditAnywhere, Category = "Ragdoll|稳定", meta = (ClampMin = "0.0"))
	float RigidBodyLinearDamping = 1.5f;

	UPROPERTY(EditAnywhere, Category = "Ragdoll|稳定", meta = (ClampMin = "0.0"))
	float RigidBodyAngularDamping = 4.0f;

	UPROPERTY(EditAnywhere, Category = "Ragdoll|稳定", meta = (ClampMin = "0.0", ClampMax = "1.0"))
	float CenterOfMassCorrection = 0.8f;

	UPROPERTY(EditAnywhere, Category = "Ragdoll|稳定", meta = (ClampMin = "0.0"))
	float MaxCenterOfMassCorrection = 14.0f;

	UPROPERTY(EditAnywhere, Category = "Ragdoll|稳定", meta = (ClampMin = "0.0"))
	float BalanceResponseSpeed = 3.0f;

	UPROPERTY(EditAnywhere, Category = "Ragdoll|稳定", meta = (ClampMin = "0.0"))
	float DriveTargetSmoothingSpeed = 10.0f;

	/** 坡面法线和贴地点的过渡速度；坡顶接到平面时若直接换法线，髋目标和旋转会一帧抽掉 */
	UPROPERTY(EditAnywhere, Category = "Ragdoll|稳定", meta = (ClampMin = "0.0"))
	float GroundNormalSmoothingSpeed = 8.0f;

	// 开始站立与倒地恢复
	UPROPERTY(EditAnywhere, Category = "Ragdoll|稳定", meta = (ClampMin = "0.0", ClampMax = "2.0"))
	float StartupFootPlantDuration = 0.3f;

	/** 晕倒解除后，必须在可站地面上低速保持这段时间才重新开启电机 */
	UPROPERTY(EditAnywhere, Category = "Ragdoll|恢复", meta = (ClampMin = "0.0", DisplayName = "起身前稳定等待 (LimpRecoveryStableDuration)", ToolTip = "晕倒解除后在地面低速稳定多久才开始恢复。", Units = "s", UIMax = "1.5"))
	float LimpRecoveryStableDuration = 0.35f;

	UPROPERTY(EditAnywhere, Category = "Ragdoll|恢复", meta = (ClampMin = "0.0", DisplayName = "允许起身的最大速度 (LimpRecoveryMaxSpeed)", ToolTip = "速度高于此值时不开始稳定恢复。", Units = "cm/s", UIMax = "300"))
	float LimpRecoveryMaxSpeed = 140.0f;

	/** 电机重新开启后，髋目标从当前离地高度升到站姿高度的时间 */
	UPROPERTY(EditAnywhere, Category = "Ragdoll|恢复", meta = (ClampMin = "0.05", DisplayName = "起身抬升时间 (LimpRecoveryRiseDuration)", ToolTip = "从当前髋部高度平滑恢复到站姿的时间。", Units = "s", UIMax = "2"))
	float LimpRecoveryRiseDuration = 0.75f;

	/** 刚恢复控制时的电机力度比例，随后随起身过程平滑回到 1 */
	UPROPERTY(EditAnywhere, Category = "Ragdoll|恢复", meta = (ClampMin = "0.0", ClampMax = "1.0", DisplayName = "起身初始强度比例 (LimpRecoveryInitialDriveStrength)", ToolTip = "起身开始的驱动比例，随后渐进恢复。", UIMax = "1"))
	float LimpRecoveryInitialDriveStrength = 0.2f;

	// 跳跃与镜头跟随
	/** 跳跃最高点相对站立高度的抬升量；默认约为角色身高的一半 */
	UPROPERTY(EditAnywhere, Category = "Ragdoll|跳跃", meta = (ClampMin = "0.0"))
	float JumpHeight = 90.0f;

	/** 一次跳跃从离地到髋目标回到站立高度的总时长；没有前摇，按下即开始 */
	UPROPERTY(EditAnywhere, Category = "Ragdoll|跳跃", meta = (ClampMin = "0.05"))
	float JumpDuration = 0.55f;

	/** 落地判定容差，按站立高度的比例算；髋部低于 StandHeight*(1+该值) 就算站在地上 */
	UPROPERTY(EditAnywhere, Category = "Ragdoll|跳跃", meta = (ClampMin = "0.0", ClampMax = "1.0"))
	float GroundedHeightTolerance = 0.35f;

	UPROPERTY(EditAnywhere, Category = "Ragdoll")
	float CapsuleHipsZOffset = 0.0f;

	UPROPERTY(EditAnywhere, Category = "Ragdoll|镜头", meta = (ClampMin = "0.0"))
	float CameraSmoothingSpeed = 7.0f;

	// 网络更新与纠正
	UPROPERTY(EditAnywhere, Category = "Ragdoll|网络", meta = (ClampMin = "5.0", ClampMax = "60.0", DisplayName = "姿态快照频率 (NetworkSnapshotRate)", ToolTip = "服务器每秒采样并发送姿态的目标频率；只调联网表现，不修复本机物理抖动。", Units = "Hz", UIMax = "60"))
	float NetworkSnapshotRate = 20.0f;

	UPROPERTY(EditAnywhere, Category = "Ragdoll|网络", meta = (ClampMin = "0.0", DisplayName = "本机位置校正速度 (OwnerCorrectionSpeed)", ToolTip = "本机姿态按服务器位置校正的响应；过快可能出现拉扯感。", UIMax = "15"))
	float OwnerCorrectionSpeed = 5.0f;

	UPROPERTY(EditAnywhere, Category = "Ragdoll|网络", meta = (ClampMin = "1.0", DisplayName = "强制校正距离 (OwnerHardCorrectionDistance)", ToolTip = "髋部误差超过此距离采用强校正。", Units = "cm", UIMax = "200"))
	float OwnerHardCorrectionDistance = 100.0f;

	UPROPERTY(EditAnywhere, Category = "Ragdoll|网络", meta = (ClampMin = "0.0", ClampMax = "0.5", DisplayName = "最大推算时间 (MaxSnapshotExtrapolation)", ToolTip = "网络数据未到时允许沿旧速度推算多久；过大容易越推越偏。", Units = "s", UIMax = "0.5"))
	float MaxSnapshotExtrapolation = 0.1f;

	UPROPERTY(EditAnywhere, Category = "Ragdoll|网络", meta = (ClampMin = "0.05", ClampMax = "1.0"))
	float ServerInputTimeout = 0.25f;

	// 同步状态
	UPROPERTY(ReplicatedUsing = OnRep_ControlMode)
	EDeliveryRagdollControlMode ReplicatedControlMode = EDeliveryRagdollControlMode::Disabled;

	UPROPERTY(ReplicatedUsing = OnRep_RagdollSnapshot)
	FDeliveryRagdollSnapshot ReplicatedSnapshot;

	UPROPERTY(Transient)
	TObjectPtr<USkeletalMeshComponent> Mesh;

	UPROPERTY(Transient)
	TObjectPtr<UCapsuleComponent> Capsule;

	/** 地面探测结果：击中点和向上的坡面法线 */
	struct FGroundHit
	{
		FVector Point = FVector::ZeroVector;
		FVector Normal = FVector::UpVector;
		FVector SurfaceVelocity = FVector::ZeroVector;
	};

	// 各职责的运行状态，只有主组件安排更新顺序
	FDeliveryRagdollMovement Movement;
	FDeliveryRagdollRecovery Recovery;
	FDeliveryRagdollPunch Punch;
	FDeliveryRagdollSync Network;

	// 物理更新与姿势缓存
	FActorComponentTickFunction PostPhysicsTickFunction;
	FDeliveryBoxingPose BoxingPose;
	FQuat ReferencePelvisRotation = FQuat::Identity;
	FQuat ReferenceSpineRelativeRotation = FQuat::Identity;
	FQuat ReferenceHeadRotation = FQuat::Identity;
	FQuat ReferenceLeftArmRelativeRotation = FQuat::Identity;
	FQuat ReferenceRightArmRelativeRotation = FQuat::Identity;
	FTransform InitialMeshRelativeTransform = FTransform::Identity;
	FVector UprightInPelvisSpace = FVector::UpVector;
	float DragReachAlpha = 0.0f;
	FName PelvisControl;
	FName ChestControl;
	FName HeadControl;
	/** 胸腔以上的脊柱节；出拳时按名字绷紧，不要假定自定义 Set 名一定存在 */
	TArray<FName> TorsoControls;
	// 躯干支撑和攻击过渡
	float BraceAlpha = 0.0f;
	float AppliedBraceAlpha = -1.0f;
	float AppliedBraceTargetStrength = -1.0f;
	/** 平滑后的拧身角度；发给电机的必须是这个，不是阶跃的目标角度 */
	float PunchTwist = 0.0f;
	float PunchTorsoLean = 0.0f;
	float PunchLunge = 0.0f;
	/** 受击后退平滑值，0 到 1，驱动髋部目标沿 HitPushDirection 的偏移量 */
	float HitPushAlpha = 0.0f;
	float StandHeight = 95.0f;
	float ReferenceFacingYaw = 0.0f;
	/** 取得物理身体的稳定朝向，供远端角色的手持物显示使用 */
	UPROPERTY(Replicated)
	float CurrentFacingYaw = 0.0f;
#if !UE_BUILD_SHIPPING
	// 诊断采样
	// 只读的恢复过程诊断数据，不参与玩法判断或网络同步
	int32 DiagnosticRecoveryMode = -1;
	float DiagnosticRecoveryUntil = 0.0f;
	float DiagnosticRecoveryNextLog = 0.0f;
	float DiagnosticUpperBodyNextLog = 0.0f;
	FVector DiagnosticPrePhysicsPosition = FVector::ZeroVector;
	FVector DiagnosticPrePhysicsVelocity = FVector::ZeroVector;
	bool bDiagnosticPrePhysicsValid = false;
	void TraceRecoveryPhysics(bool bAfterPhysics, float DeltaTime);
#endif
	float HitReactionEndTime = 0.0f;
	/** 被打飞的水平方向（单位向量）；脚沿它挪，朝向取它的反向 */
	FVector HitPushDirection = FVector::ZeroVector;
	/** 受击期间身体要转过去的朝向：面对出拳的人 */
	float HitFacingYaw = 0.0f;
	bool bHasHitFacing = false;
	bool bHitFeetPlanted = false;
	bool bPunchFeetPlanted = false;
	bool bPunchSupportInterrupted = false;
	FVector PunchSupportLocation = FVector::ZeroVector;
	float PunchSupportYaw = 0.0f;
	// 身体模式与翻滚状态，集中在主组件切换
	bool bIsActive = false;
	bool bIsLimp = false;
	bool bExternalLimpRequested = false;
	bool bSlopeTumbling = false;
	float TumbleEntryTime = 0.0f;
	float TumbleElapsed = 0.0f;
	float TumbleRecoveryTime = 0.0f;
	float TumbleCooldownTime = 0.0f;

	// 网络同步回调
	UFUNCTION()
	void OnRep_ControlMode();

	UFUNCTION()
	void OnRep_RagdollSnapshot();

	// 物理初始化与重建
	void ResolveOwnerComponents();
	void ResolveConfiguredPhysicsBones();
	bool ValidateSetup() const;
	void PlaceOnGround();
	/** 取消正在进行的迈步，并关掉两只脚的世界空间位置电机；起跳和落地各调一次 */
	void CancelFootSteps();
	/**
	 * 按身体当前所在的位置重新播种髋目标、贴地点和步态状态
	 * 从 Limp 恢复时必须先调它：控制器里存的还是倒下前那一刻的目标，
	 * 直接开电机会把人拽回旧位置；不动 StandHeight，人躺着时那个值算出来是错的
	 */
	void ReseedFromCurrentPose();
	void ConfigurePhysics();
	bool CreateControls();
	void DestroyControls();
	void CacheStandingState();
	// 统一更新入口，不为内部状态对象单独注册 Tick
	void UpdateActionState();
	void UpdateBeforePhysics(float DeltaTime);
	void UpdateAfterPhysics(float DeltaTime);
	// 身体姿势更新
	void UpdateControlTargets(float DeltaTime);
	void SetHitReactionStrength(float Multiplier);
	void SetHitFeetPlanted(bool bPlant);
	void SetPunchFeetPlanted(bool bPlant);
	void BraceTorsoForAction(float DeltaTime, bool bPunch, bool bCarry);
	void UpdatePelvisTarget(float DeltaTime, const FVector& Wish);
	void UpdateJumpHeight(float DeltaTime);
	void UpdateFacingYaw(float DeltaTime, const FVector& Wish, bool bCarryingProp);
	void WriteBodyRotationTargets(float DeltaTime, const FVector& Wish, float Wobble, bool bCarryingProp,
	                              const FVector& Target);
	// 迈步与落脚
	// 按实际脚位检测持续交叉，受击和离地时退出补步恢复
	void UpdateFootRecovery(float DeltaTime);
    void UpdateArmRecovery(float DeltaTime);
	void UpdateFeet(float DeltaTime, const FVector& Wish);
	void KeepFeetOnOwnSide();
	void ApplySlopeSupportTraction(float DeltaTime);
	bool BeginStep(FDeliveryRagdollFoot& Foot, const FVector& Wish);
	/** 落脚音的实际播放；Stride 是这一脚的水平位移，用来判要不要响、响多大 */
	void PlayFootstep(bool bLeft, const FVector& Location, float Stride);

	/** 上一次用过的变体下标，用来避免连着放同一个；-1 = 还没放过 */
	int32 LastFootstepIndex = -1;
	void UpdateFootTarget(FDeliveryRagdollFoot& Foot, float DeltaTime);
	bool PlanFootLanding(FDeliveryRagdollFoot& Foot, const FVector& Wish);
	FVector GetSlopeForward(const FVector& Wish) const;
	FVector GetSlopeRight(const FVector& Wish) const;
	FQuat MakeSlopeAlignedFootRotation(const FDeliveryRagdollFoot& Foot, const FVector& SlopeForward) const;
	// 地面探测与恢复
	bool SampleGround(const FVector& Planned, const FVector& Fallback, const FVector& AlongNormal,
	                  FGroundHit& OutHit) const;
	FVector GetWholeBodyCenterOfMass() const;
	float GetUprightDot() const;
	bool TraceGround(const FVector& Around, const FVector& AlongNormal, FGroundHit& OutHit) const;
	bool TraceTumbleSurface(FGroundHit& OutHit) const;
	void UpdateSlopeTumble(float DeltaTime);
	void UpdateLimpRecovery(float DeltaTime);
	void SetLimpRecoveryDriveStrength(float Alpha);
	void ApplyLimpState(bool bLimp);
	FVector GetWishDir() const;
	float GetAimYaw() const;
	void SyncOwnerToPelvis(float DeltaTime);
	// 身体快照采集与应用
	void CaptureNetworkSnapshot();
	void ApplyNetworkSnapshot(float DeltaTime);
	// 同步平移身体和控制目标，保留当前迈步进度
	void ApplyOwnerTranslation(const FVector& Translation);
	void FinishClientRecoveryPlayback();
};
