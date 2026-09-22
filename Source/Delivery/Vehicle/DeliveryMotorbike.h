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

	/** 美术资产的对齐节点：导入出来的网格原点在 FBX 场景原点，靠它整体挪到车轮着地、车身居中。 */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category="Components")
	TObjectPtr<USceneComponent> MeshRoot;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category="Components")
	TArray<TObjectPtr<UStaticMeshComponent>> BodyParts;

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

	/** 镜头多久没被玩家拨动就自动回到车尾后方。 */
	UPROPERTY(EditAnywhere, Category="Motorbike|表现", meta=(ClampMin="0.0"))
	float CameraRecenterDelay = 1.2f;

	UPROPERTY(EditAnywhere, Category="Motorbike|表现", meta=(ClampMin="0.0"))
	float CameraRecenterSpeed = 2.5f;

	// ---- 上下车 ----

	/** 下车时驾驶员放在车身右侧多远（负值就是左侧）。 */
	UPROPERTY(EditAnywhere, Category="Motorbike|上下车", meta=(Units="cm"))
	float ExitSideOffset = -140.0f;

	UPROPERTY(EditAnywhere, Category="Motorbike|上下车")
	FText DrivePromptText;

	UPROPERTY(EditAnywhere, Category="Motorbike|上下车")
	FText ExitPromptText;

	UPROPERTY(EditAnywhere, Category="Input")
	TObjectPtr<UInputAction> MoveAction;

	UPROPERTY(EditAnywhere, Category="Input")
	TObjectPtr<UInputAction> LookAction;

	UPROPERTY(EditAnywhere, Category="Input")
	TObjectPtr<UInputAction> MouseLookAction;

	/**
	 * 交互键（F）。用软引用晚绑：IA_Interact 是由 setup_motorbike.py 生成的，
	 * 构造函数里的 ConstructorHelpers 只在模块加载时跑一次，资产是这次会话里新建的话
	 * 就永远解析不到，必须等到绑定输入时再加载。
	 */
	UPROPERTY(EditAnywhere, Category="Input")
	TSoftObjectPtr<UInputAction> InteractAction;

private:

	void MoveInput(const FInputActionValue& Value);
	void LookInput(const FInputActionValue& Value);
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
	void UpdateCameraRecenter(float DeltaSeconds);
	void PushExitPrompt();

	/** 找一个能把驾驶员放下去的位置。找不到空位就放在车顶上方，总比卡进墙里好。 */
	FVector FindExitLocation() const;

	UPROPERTY(ReplicatedUsing=OnRep_Driver)
	TObjectPtr<ADeliveryCharacter> Driver;

	float ThrottleInput = 0.0f;
	float SteerInput = 0.0f;
	float CurrentSpeed = 0.0f;
	float VerticalVelocity = 0.0f;
	float CurrentLean = 0.0f;
	double LastLookTime = -1000.0;
	bool bWasGrounded = true;
};
