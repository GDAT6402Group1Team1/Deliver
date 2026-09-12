// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "DeliveryCombatTypes.generated.h"

/** 挥拳用哪只手。 */
UENUM(BlueprintType)
enum class EMeleeHand : uint8
{
	Left,
	Right
};

/**
 * 手臂的三个姿势：站立走路的 A 姿势、出拳起手的收拳、直拳终点。
 * 位置都写成相对肩部的偏移，长度按整条手臂的比例给，换个体型不用重调。
 */
USTRUCT(BlueprintType)
struct FDeliveryArmPoseSettings
{
	GENERATED_BODY()

	/** A 姿势：手臂下垂后向外张开的角度。0 表示完全贴着身体。 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="A 姿势", meta=(ClampMin="0.0", ClampMax="80.0"))
	float RestSpreadAngle = 30.0f;

	/** A 姿势：手臂下垂后向前摆的角度。 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="A 姿势", meta=(ClampMin="-30.0", ClampMax="45.0"))
	float RestForwardAngle = 7.0f;

	/** A 姿势：手到肩的距离占整条手臂的比例。小于 1 时肘部留一点自然弯曲。 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="A 姿势", meta=(ClampMin="0.3", ClampMax="1.0"))
	float RestReach = 0.85f;

	/**
	 * 每节手指向掌心弯曲的角度。三节累加才是握拳的总量，调小就是松拳。
	 * 手指没有刚体，只能靠这个把参考姿势的张开五指卷起来；不卷的话打出去的是巴掌。
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="手型", meta=(ClampMin="0.0", ClampMax="90.0"))
	float FingerCurlAngle = 52.0f;

	/** 拇指弯曲的角度。拇指压在四指外侧，弯太多会陷进拳头里。 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="手型", meta=(ClampMin="0.0", ClampMax="90.0"))
	float ThumbCurlAngle = 26.0f;

	/**
	 * 收拳：拳头相对肩膀前后多远。负值表示收在肩前，也就是拳击的护架姿势。
	 * 拉到肩后（正值）时拳头必须绕过肩膀才能到身前，绕哪边都不像直拳：
	 * 绕外侧是抡巴掌，绕上方是手刀。
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="收拳", meta=(ClampMin="-0.6", ClampMax="0.8"))
	float WindupBack = -0.32f;

	/**
	 * 收拳：拳头比肩高多少。要和终点落在同一条射线上，比例按行程来：
	 * PunchDrop = -WindupUp * PunchReach / |WindupBack|。对不上，前送就会一边走一边找高度。
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="收拳", meta=(ClampMin="-0.3", ClampMax="0.6"))
	float WindupUp = 0.02f;

	/**
	 * 收拳：拳头向身体外侧拉开多少。负值表示收向中线。同样按行程对齐：
	 * PunchInward = -WindupOutward * PunchReach / |WindupBack|。对不上就是横着划弧。
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="收拳", meta=(ClampMin="-0.4", ClampMax="0.7"))
	float WindupOutward = -0.016f;

	/** 直拳终点：拳头离肩多远。1 表示手臂打直。 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="直拳", meta=(ClampMin="0.5", ClampMax="1.0"))
	float PunchReach = 1.0f;

	/** 直拳终点：向身体中线收多少。和 WindupOutward 按行程对齐，拳头才走直线。 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="直拳", meta=(ClampMin="0.0", ClampMax="0.3"))
	float PunchInward = 0.05f;

	/** 直拳终点：拳头比肩低多少。负值表示比肩高。和 WindupUp 按行程对齐。 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="直拳", meta=(ClampMin="-0.2", ClampMax="0.3"))
	float PunchDrop = -0.062f;

	/**
	 * 手臂在前送过程中的哪一段才开始伸直。起手位收在肩后时要留延迟，否则手臂在还朝着
	 * 侧后方时就已经是伸的，扫出来是一个大横弧。护架起手和出拳终点几乎同向，
	 * 不需要延迟——延迟在这里只会让出手发软。
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="直拳", meta=(ClampMin="0.0", ClampMax="0.9"))
	float PunchExtendDelay = 0.0f;

	/** 出拳方向相对身体正前方最多偏多少度。再多就只能靠转身补，否则目标落在肩膀活动范围之外。 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="直拳", meta=(ClampMin="0.0", ClampMax="90.0"))
	float PunchYawLimit = 50.0f;

	/** 收拳、放手、前送、收回的速度，单位是每秒完成的姿势比例。 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="节奏", meta=(ClampMin="0.5"))
	float WindupSpeed = 5.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="节奏", meta=(ClampMin="0.5"))
	float WindupReleaseSpeed = 3.0f;

	/**
	 * 前送速度。目标点冲得比手臂能跟上的更快时，电机只会原地抽一下，
	 * 看起来就是摆了摆手；留出让整条手臂真正走完这段弧线的时间。
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="节奏", meta=(ClampMin="0.5"))
	float PunchExtendSpeed = 13.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="节奏", meta=(ClampMin="0.5"))
	float PunchRetractSpeed = 4.0f;

	/** 三个阶段的肌肉强度，单位是自然频率（Hz）。站着可以松，出拳必须硬。 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="肌肉", meta=(ClampMin="0.0"))
	float RestStrength = 5.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="肌肉", meta=(ClampMin="0.0"))
	float WindupStrength = 16.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="肌肉", meta=(ClampMin="0.0"))
	float PunchStrength = 39.0f;

	/** 上臂的强度倍率。肩膀要比小臂硬，才是肩带着手走；否则就是小臂在甩巴掌。 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="肌肉", meta=(ClampMin="0.1", ClampMax="4.0"))
	float UpperArmStrengthScale = 1.95f;

	/**
	 * 拳头的强度倍率。手腕软了拳头会拖在手臂后面甩过去，打上去像拍；
	 * 但绷得比小臂还硬又会把整条手臂带着走。留在小臂之下、接近它就行。
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="肌肉", meta=(ClampMin="0.1", ClampMax="2.0"))
	float HandStrengthScale = 0.85f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="肌肉", meta=(ClampMin="0.0"))
	float DampingRatio = 1.1f;

	/**
	 * 肩关节允许的摆动和扭转上限（度）。物理资产给的默认锥角只有几十度，
	 * 而收拳要把上臂抬到肩前、直拳还要再往前送，限位一卡住就只剩小臂能动。
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="关节", meta=(ClampMin="20.0", ClampMax="170.0"))
	float ShoulderSwingLimit = 110.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="关节", meta=(ClampMin="10.0", ClampMax="170.0"))
	float ShoulderTwistLimit = 75.0f;

	/** 肘关节限位。收拳时肘部要折到接近 130 度。 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="关节", meta=(ClampMin="20.0", ClampMax="170.0"))
	float ElbowSwingLimit = 150.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="关节", meta=(ClampMin="10.0", ClampMax="170.0"))
	float ElbowTwistLimit = 45.0f;
};
