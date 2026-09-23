// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Pawn.h"
#include "DeliveryMotorbike.generated.h"

class ADeliveryCharacter;
class UBoxComponent;
class UCameraComponent;
class UDeliveryInteractableComponent;
class UInputAction;
class USkeletalMeshComponent;
class USpringArmComponent;
class UStaticMesh;
class UStaticMeshComponent;
struct FInputActionValue;

/**
 * 可骑的摩托车。走近按 F 上车，车接管操控，再按一次 F 下车。
 *
 * 移动是**运动学街机式**的：车身不模拟物理，每帧自己算速度/转向，贴着地面走，
 * 横向位移用 sweep 挡墙。理由有两条：
 *
 *  1. 美术资产是一整套静态网格 + 一个坐姿骑手，没有轮子骨骼、没有物理资产，
 *     Chaos Vehicle 那套需要的东西一样都没有，硬上等于要先回 Blender 重新绑定。
 *  2. 关卡里的交通车（BP_car_base）本来就是运动学沿样条走的，玩家车用同一套假设，
 *     不会出现"玩家车被物理弹飞、AI 车纹丝不动"这种两套世界观打架的情况。
 *
 * 骑手是模型自带的那具坐姿网格，平时藏起来，有人上车才显示——不然停在路边的空车上
 * 永远坐着一个人。玩家自己那具布娃娃在上车时停掉物理、隐藏并挂到车上，下车时放回地面。
 */
UCLASS()
class DELIVERY_API ADeliveryMotorbike : public APawn
{
	GENERATED_BODY()

public:

	ADeliveryMotorbike();

	virtual void OnConstruction(const FTransform& Transform) override;
	virtual void BeginPlay() override;
	virtual void Tick(float DeltaSeconds) override;
	virtual void SetupPlayerInputComponent(class UInputComponent* PlayerInputComponent) override;
	virtual void GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const override;

	/** 服务器：让 NewDriver 上车。成功返回 true。 */
	UFUNCTION(BlueprintCallable, Category="Motorbike")
	bool TryEnter(ADeliveryCharacter* NewDriver);

	/** 服务器：下车，把驾驶员放回车边的地面。 */
	UFUNCTION(BlueprintCallable, Category="Motorbike")
	void ExitVehicle();

	/**
	 * 服务器：被交通车撞了一下。撞够 ImpactsToDismount 次就把人掀下车。
	 *
	 * 返回 true 表示这一下真的记账了；交通车组件据此决定要不要给这辆车上冷却，
	 * 没记账（比如车上没人）就不占用它的冷却表。
	 */
	bool NotifyTrafficImpact(const FVector& CarVelocity);

	UFUNCTION(BlueprintPure, Category="Motorbike")
	ADeliveryCharacter* GetDriver() const { return Driver; }

	UFUNCTION(BlueprintPure, Category="Motorbike")
	float GetCurrentSpeed() const { return CurrentSpeed; }

	/** 车体部件网格。FBX 里摩托车是 9 个独立静态网格，导入脚本会把它们填到这里。 */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category="Motorbike|Mesh")
	TArray<TObjectPtr<UStaticMesh>> BodyMeshes;

protected:

	/** 车体部件的固定槽位数。用定额的默认子对象而不是运行时 NewObject，避免构造脚本反复重建组件。 */
	static constexpr int32 MaxBodyParts = 12;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category="Components")
	TObjectPtr<UBoxComponent> CollisionBox;

	/**
	 * 只负责转弯侧倾（Roll）。侧倾必须是绕"行驶方向"转，也就是 actor 的 +X。
	 *
	 * 和下面的 MeshAlign 分成两层不是洁癖：FRotator 的施加顺序是 Roll→Pitch→Yaw，
	 * 如果把侧倾和车头朝向修正写进同一个组件，Roll 会绕"修正之前"的局部 X 轴转，
	 * 而那根轴在修正 90 度之后是车的横向——结果就是本该压弯，实际变成点头。
	 */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category="Components")
	TObjectPtr<USceneComponent> MeshRoot;

	/**
	 * 美术资产的对齐节点：把模型摆正到"车头朝 +X、车身居中、车轮贴着原点平面"。
	 * 导入出来的网格顶点在 FBX 场景绝对坐标里，车头还朝着 +Y，都靠这一层纠正。
	 * 具体数值由 setup_motorbike.py 量出来写进 CDO，不要手填。
	 */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category="Components")
	TObjectPtr<USceneComponent> MeshAlign;

	/**
	 * 龙头：前轮 / 前叉 / 车把挂在这一层，按转向输入绕竖直轴转。
	 *
	 * 挂在 MeshAlign 下面而不是 MeshRoot：这样它继承了车头朝向修正，"局部 +Yaw = 往右打把"
	 * 直接成立；同时也继承 MeshRoot 的侧倾，压弯时前叉跟着一起倒，不会单独立着。
	 */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category="Components")
	TObjectPtr<USceneComponent> SteerPivot;

	/**
	 * 车把单独一层，和 SteerPivot 同一根轴、但只转一部分角度。
	 *
	 * 因为骑手是固定的参考姿势、手不会跟着车把走，车把转多少就脱手多少。
	 * 街机赛车游戏的常规做法就是"轮子打满、车把几乎不动"，视觉上并不违和，
	 * 而且比上 IK 便宜得多。前轮前叉照样打满，转向感不受影响。
	 */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category="Components")
	TObjectPtr<USceneComponent> BarPivot;

	/** 骑手单独一层，绕**自己胯部**的竖轴微微跟转，不是绕转向轴。 */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category="Components")
	TObjectPtr<USceneComponent> RiderPivot;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category="Components")
	TArray<TObjectPtr<UStaticMeshComponent>> BodyParts;

	/** 打满角度跟转的槽位（前轮、前叉）。由 setup_motorbike.py 按几何认出来填，别手填。 */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category="Motorbike|Mesh")
	TArray<int32> SteeringPartIndices;

	/** 只跟转一部分的槽位（车把）。 */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category="Motorbike|Mesh")
	TArray<int32> HandlebarPartIndices;

	/** 转向轴位置，MeshAlign 局部空间。同样由脚本量出来填。 */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category="Motorbike|Mesh")
	FVector SteerPivotLocation = FVector::ZeroVector;

	/** 骑手胯部位置（脚本从 hips 骨骼读出来），骑手绕这根竖轴扭身。 */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category="Motorbike|Mesh")
	FVector RiderPivotLocation = FVector::ZeroVector;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category="Components")
	TObjectPtr<USkeletalMeshComponent> RiderMesh;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category="Components")
	TObjectPtr<USpringArmComponent> CameraBoom;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category="Components")
	TObjectPtr<UCameraComponent> FollowCamera;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category="Components")
	TObjectPtr<UDeliveryInteractableComponent> Interactable;

	// ---- 行驶参数 ----

	UPROPERTY(EditAnywhere, Category="Motorbike|行驶", meta=(ClampMin="0.0", Units="cm/s"))
	float MaxSpeed = 1500.0f;

	UPROPERTY(EditAnywhere, Category="Motorbike|行驶", meta=(ClampMin="0.0", Units="cm/s"))
	float MaxReverseSpeed = 400.0f;

	UPROPERTY(EditAnywhere, Category="Motorbike|行驶", meta=(ClampMin="0.0"))
	float ThrottleAcceleration = 950.0f;

	/** 往回推油门时的刹车力。比加速大得多，按一下就能明显减速。 */
	UPROPERTY(EditAnywhere, Category="Motorbike|行驶", meta=(ClampMin="0.0"))
	float BrakeDeceleration = 2200.0f;

	/** 松油门时的自然减速。 */
	UPROPERTY(EditAnywhere, Category="Motorbike|行驶", meta=(ClampMin="0.0"))
	float CoastDeceleration = 400.0f;

	UPROPERTY(EditAnywhere, Category="Motorbike|行驶", meta=(ClampMin="0.0", Units="deg/s"))
	float MaxTurnRate = 115.0f;

	/**
	 * 转向随车速淡入的参考速度：车速到这个值时转向到满。
	 * 摩托车停着不能原地转圈，所以转向量乘的是 Speed/TurnSpeedReference（上限 1）。
	 */
	UPROPERTY(EditAnywhere, Category="Motorbike|行驶", meta=(ClampMin="1.0", Units="cm/s"))
	float TurnSpeedReference = 450.0f;

	// ---- 贴地 ----

	/** 车身根点离地高度。要大于碰撞盒半高，否则盒子会插进地面。 */
	UPROPERTY(EditAnywhere, Category="Motorbike|贴地", meta=(ClampMin="0.0", Units="cm"))
	float HoverHeight = 55.0f;

	UPROPERTY(EditAnywhere, Category="Motorbike|贴地", meta=(ClampMin="0.0", Units="cm"))
	float GroundTraceUp = 150.0f;

	UPROPERTY(EditAnywhere, Category="Motorbike|贴地", meta=(ClampMin="0.0", Units="cm"))
	float GroundTraceDown = 500.0f;

	/** 贴地高度的追随速度。太大在碎石路上会抖，太小上坡会陷进去。 */
	UPROPERTY(EditAnywhere, Category="Motorbike|贴地", meta=(ClampMin="0.1"))
	float GroundSnapSpeed = 12.0f;

	UPROPERTY(EditAnywhere, Category="Motorbike|贴地")
	float GravityZ = -2200.0f;

	/** 车身俯仰跟随坡度的插值速度。 */
	UPROPERTY(EditAnywhere, Category="Motorbike|贴地", meta=(ClampMin="0.1"))
	float GroundAlignSpeed = 7.0f;

	UPROPERTY(EditAnywhere, Category="Motorbike|贴地", meta=(ClampMin="0.0", ClampMax="80.0"))
	float MaxGroundAlignAngle = 35.0f;

	// ---- 表现 ----

	/** 转弯时车身视觉上的最大侧倾角。只动 MeshRoot，不影响碰撞和行驶。 */
	UPROPERTY(EditAnywhere, Category="Motorbike|表现", meta=(ClampMin="0.0", ClampMax="60.0"))
	float MaxLeanAngle = 26.0f;

	UPROPERTY(EditAnywhere, Category="Motorbike|表现", meta=(ClampMin="0.1"))
	float LeanSpeed = 5.0f;

	/**
	 * 龙头打死时的最大转角。
	 * 别调太大：骑手是固定的参考姿势，手不会跟着车把走，角度一大就会脱手。
	 */
	UPROPERTY(EditAnywhere, Category="Motorbike|表现", meta=(ClampMin="0.0", ClampMax="60.0"))
	float MaxVisualSteerAngle = 22.0f;

	UPROPERTY(EditAnywhere, Category="Motorbike|表现", meta=(ClampMin="0.1"))
	float SteerVisualSpeed = 9.0f;

	/** 车把转前轮的百分之多少。1 = 跟前轮一样打满（手会明显脱把）。 */
	UPROPERTY(EditAnywhere, Category="Motorbike|表现", meta=(ClampMin="0.0", ClampMax="1.0"))
	float HandlebarSteerRatio = 0.4f;

	/** 骑手扭身跟转的比例。调大了脚会离开脚踏。 */
	UPROPERTY(EditAnywhere, Category="Motorbike|表现", meta=(ClampMin="0.0", ClampMax="1.0"))
	float RiderSteerRatio = 0.25f;

	/** 骑车时镜头的俯角。镜头写死在车尾后方，不跟鼠标。 */
	UPROPERTY(EditAnywhere, Category="Motorbike|表现", meta=(ClampMin="-80.0", ClampMax="20.0"))
	float CameraPitch = -12.0f;

	// ---- 上下车 ----

	/** 下车时驾驶员放在车身右侧多远（负值就是左侧）。 */
	UPROPERTY(EditAnywhere, Category="Motorbike|上下车", meta=(Units="cm"))
	float ExitSideOffset = -140.0f;

	/**
	 * 骑行中被交通车撞几次会被掀下车。默认 2：第一下只当擦碰，第二下才下车。
	 * 每次上车重新从 0 开始算，中途主动下车也清零。
	 */
	UPROPERTY(EditAnywhere, Category="Motorbike|上下车", meta=(ClampMin="1"))
	int32 ImpactsToDismount = 2;

	// ---- 被撞反应 ----

	/** 把来车速度的多少比例变成自己的击退速度。 */
	UPROPERTY(EditAnywhere, Category="Motorbike|被撞", meta=(ClampMin="0.0", ClampMax="2.0"))
	float KnockbackFraction = 0.6f;

	UPROPERTY(EditAnywhere, Category="Motorbike|被撞", meta=(ClampMin="0.0", Units="cm/s"))
	float MaxKnockbackSpeed = 700.0f;

	/** 被撞歪：车头被撞偏的角速度，按来车方向相对车身的左右分量定正负。 */
	UPROPERTY(EditAnywhere, Category="Motorbike|被撞", meta=(ClampMin="0.0", Units="deg/s"))
	float KnockYawPerHit = 140.0f;

	/** 被撞歪：车身（连带骑手）视觉上倾斜多少度。只改 MeshRoot，不影响碰撞。 */
	UPROPERTY(EditAnywhere, Category="Motorbike|被撞", meta=(ClampMin="0.0", ClampMax="60.0"))
	float KnockTiltPerHit = 32.0f;

	/** 击退/撞歪/抖动统一的指数衰减速率。越大恢复越快。 */
	UPROPERTY(EditAnywhere, Category="Motorbike|被撞", meta=(ClampMin="0.1"))
	float KnockDecay = 2.6f;

	/** 镜头震动的最大角度。 */
	UPROPERTY(EditAnywhere, Category="Motorbike|被撞", meta=(ClampMin="0.0", ClampMax="15.0"))
	float ShakeAngle = 5.0f;

	UPROPERTY(EditAnywhere, Category="Motorbike|被撞", meta=(ClampMin="0.1"))
	float ShakeFrequency = 30.0f;

	/**
	 * 被撞下车时是否把驾驶员的血清零。
	 *
	 * 清零之后 DeliverAbilitySystemComponent 会自己把角色切进 Stunned / Limp，
	 * 复用的就是"被车撞倒"那套既有表现——这里不另外写一套倒地。
	 */
	UPROPERTY(EditAnywhere, Category="Motorbike|被撞")
	bool bKnockDownDriverOnDismount = true;

	/** 被撞下车时把人抛出去的水平速度。调大人飞得更远、更不容易和车纠缠。 */
	UPROPERTY(EditAnywhere, Category="Motorbike|被撞", meta=(ClampMin="0.0", Units="cm/s"))
	float KnockDownLaunchSpeed = 900.0f;

	UPROPERTY(EditAnywhere, Category="Motorbike|被撞", meta=(ClampMin="0.0", Units="cm/s"))
	float KnockDownLaunchUp = 420.0f;

	UPROPERTY(EditAnywhere, Category="Motorbike|上下车")
	FText DrivePromptText;

	UPROPERTY(EditAnywhere, Category="Motorbike|上下车")
	FText ExitPromptText;

	UPROPERTY(EditAnywhere, Category="Input")
	TObjectPtr<UInputAction> MoveAction;

	/**
	 * 交互键（F）。用软引用晚绑：IA_Interact 是由 setup_motorbike.py 生成的，
	 * 构造函数里的 ConstructorHelpers 只在模块加载时跑一次，资产是这次会话里新建的话
	 * 就永远解析不到，必须等到绑定输入时再加载。
	 */
	UPROPERTY(EditAnywhere, Category="Input")
	TSoftObjectPtr<UInputAction> InteractAction;

private:

	void MoveInput(const FInputActionValue& Value);
	void InteractPressed(const FInputActionValue& Value);

	UFUNCTION(Server, Unreliable)
	void ServerSetDriveInput(float InThrottle, float InSteer);

	UFUNCTION(Server, Reliable)
	void ServerRequestExit();

	UFUNCTION()
	void HandleInteractRequested(APawn* Interactor);

	UFUNCTION()
	void OnRep_Driver();

	/** 把 BodyMeshes 里的网格铺到固定槽位上，多余的槽位清空。 */
	void ApplyBodyMeshes();

	/** 按有没有驾驶员刷新骑手网格和可交互状态。服务器和客户端都会走到。 */
	void ApplyDriverPresentation();

	void UpdateSpeed(float DeltaSeconds);
	void UpdateSteering(float DeltaSeconds);
	void UpdateGroundAndMove(float DeltaSeconds);
	void UpdateLean(float DeltaSeconds);
	void UpdateSteerVisual(float DeltaSeconds);
	void UpdateImpactReaction(float DeltaSeconds);
	void PushExitPrompt();

	/** 把人打倒：血清零交给既有的晕倒流程，再补一记冲量把他抛离车身。 */
	void KnockDownDriver(ADeliveryCharacter* Rider, const FVector& LaunchDirection);

	/**
	 * 这一次下车希望把人放在哪一侧（+1 右 / -1 左 / 0 用默认）。
	 *
	 * 被撞下车时必须指定：车和人本来会被推向同一个方向（击退和抛射用的是同一个来车方向），
	 * 车是运动学的、跑得比布娃娃快，于是一路追上去压在人身上——"人卡在摩托车底下"就是这么来的。
	 * 让人落到车被推离的那一侧，两者就分开了。
	 */
	float PendingExitSideSign = 0.0f;

	/** 把控制旋转对齐到车头。镜头本身不吃控制旋转，但下车后角色要用，所以一直同步着。 */
	void SyncControlRotation();

	/** 找一个能把驾驶员放下去的位置。找不到空位就放在车顶上方，总比卡进墙里好。 */
	FVector FindExitLocation(float PreferredSideSign = 0.0f) const;

	UPROPERTY(ReplicatedUsing=OnRep_Driver)
	TObjectPtr<ADeliveryCharacter> Driver;

	float ThrottleInput = 0.0f;
	float SteerInput = 0.0f;
	float CurrentSpeed = 0.0f;
	float VerticalVelocity = 0.0f;
	float CurrentLean = 0.0f;
	float CurrentVisualSteer = 0.0f;
	bool bWasGrounded = true;

	/** 本次骑行已经被交通车撞了几下。只在服务器上维护，上/下车时归零。 */
	int32 TrafficImpactCount = 0;

	/** 被撞之后的残余状态，全部按 KnockDecay 指数衰减回 0。 */
	FVector KnockVelocity = FVector::ZeroVector;
	float KnockYawRate = 0.0f;
	float KnockTilt = 0.0f;
	float ShakeAmount = 0.0f;
	float ShakePhase = 0.0f;
};
