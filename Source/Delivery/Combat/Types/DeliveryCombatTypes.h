// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "DeliveryCombatTypes.generated.h"

/** 挥拳用哪只手 */
UENUM(BlueprintType)
enum class EMeleeHand : uint8
{
	Left,
	Right
};

/**
 * 手臂的三个姿势：站立走路的 A 姿势、出拳起手的收拳、直拳终点
 * 位置都写成相对肩部的偏移，长度按整条手臂的比例给，换个体型不用重调
 */
USTRUCT(BlueprintType)

struct FDeliveryArmPoseSettings
{
	GENERATED_BODY()

	// 待机手臂与握拳姿势
	/** A 姿势：手臂下垂后向外张开的角度；0 表示完全贴着身体 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "A 姿势",
	          meta = (ClampMin = "0.0", ClampMax = "80.0", DisplayName = "待机手臂外张角 (RestSpreadAngle)", ToolTip = "待机时手臂向外张开角度。", Units = "deg", UIMax = "80"))
	float RestSpreadAngle = 30.0f;

	/** A 姿势：手臂下垂后向前摆的角度 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "A 姿势",
	          meta = (ClampMin = "-30.0", ClampMax = "45.0", DisplayName = "待机手臂前摆角 (RestForwardAngle)", ToolTip = "待机时手臂向前摆的角度。", Units = "deg", UIMax = "45"))
	float RestForwardAngle = 7.0f;

	/** A 姿势：手到肩的距离占整条手臂的比例；小于 1 时肘部留一点自然弯曲 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "A 姿势",
	          meta = (ClampMin = "0.3", ClampMax = "1.0", DisplayName = "待机伸展比例 (RestReach)", ToolTip = "手到肩距离占手臂长度的比例；小于 1 可保留弯肘。", UIMax = "1"))
	float RestReach = 0.85f;

	/**
	 * 每节手指向掌心弯曲的角度；三节累加才是握拳的总量，调小就是松拳
	 * 手指没有刚体，只能靠这个把参考姿势的张开五指卷起来；不卷的话打出去的是巴掌
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "手型",
	          meta = (ClampMin = "0.0", ClampMax = "90.0", DisplayName = "手指弯曲角 (FingerCurlAngle)", ToolTip = "每节手指向掌心弯曲的角度，只调整手型。", Units = "deg", UIMax = "90"))
	float FingerCurlAngle = 52.0f;

	/** 拇指弯曲的角度；拇指压在四指外侧，弯太多会陷进拳头里 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "手型",
	          meta = (ClampMin = "0.0", ClampMax = "90.0", DisplayName = "拇指弯曲角 (ThumbCurlAngle)", ToolTip = "拇指弯曲过大会穿入拳头。", Units = "deg", UIMax = "90"))
	float ThumbCurlAngle = 26.0f;

	// 蓄力位置
	/**
	 * 收拳：正值把拳头拉到肩后，负值是肩前护架
	 * 后收时保留少量外侧和下方距离，避免前送经过肩关节原点导致肘部翻转
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "收拳",
	          meta = (ClampMin = "-0.6", ClampMax = "0.8", DisplayName = "蓄力后收比例 (WindupBack)", ToolTip = "以手臂长度为基准。正值向肩后收，负值在肩前；后收过多会扩大挥动。", UIMax = "0.8"))
	float WindupBack = 0.24f;

	/** 收拳：拳头比肩高多少；略低于肩，后收到前送的高度差保持很小 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "收拳",
	          meta = (ClampMin = "-0.3", ClampMax = "0.6", DisplayName = "蓄力抬高比例 (WindupUp)", ToolTip = "以手臂长度为基准，正值在肩上方，负值在下方。", UIMax = "0.6"))
	float WindupUp = -0.06f;

	/** 收拳：拳头向身体外侧拉开多少；给肩旁留出通过空间，不能拉成大幅横摆 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "收拳",
	          meta = (ClampMin = "-0.4", ClampMax = "0.7", DisplayName = "蓄力外移比例 (WindupOutward)", ToolTip = "以手臂长度为基准，给肩旁留下通过空间；过大会形成横摆。", UIMax = "0.7"))
	float WindupOutward = 0.16f;

	// 挥拳目标与方向限制
	/** 直拳终点：拳头离肩多远；1 表示手臂打直 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "直拳",
	          meta = (ClampMin = "0.5", ClampMax = "1.0", DisplayName = "直拳伸展比例 (PunchReach)", ToolTip = "1 表示使用完整手臂长度；不能用这个值绕过关节限位。", UIMax = "1"))
	float PunchReach = 1.0f;

	/** 直拳终点：向身体中线收多少；后收点到此点按位置直线插值 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "直拳",
	          meta = (ClampMin = "0.0", ClampMax = "0.3", DisplayName = "直拳内收比例 (PunchInward)", ToolTip = "终点向身体中线靠近的比例，过大可能横穿身体。", UIMax = "0.3"))
	float PunchInward = 0.05f;

	/** 直拳终点：拳头比肩低多少；负值表示比肩高 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "直拳",
	          meta = (ClampMin = "-0.2", ClampMax = "0.3", DisplayName = "直拳下移比例 (PunchDrop)", ToolTip = "正值降低终点，负值抬高终点，以手臂长度为基准。", UIMax = "0.3"))
	float PunchDrop = -0.062f;

	/**
	 * 手臂在前送过程中的哪一段才开始伸直；起手位收在肩后时要留延迟，否则手臂在还朝着
	 * 侧后方时就已经是伸的，扫出来是一个大横弧；护架起手和出拳终点几乎同向，
	 * 不需要延迟——延迟在这里只会让出手发软
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "直拳",
	          meta = (ClampMin = "0.0", ClampMax = "0.9", DisplayName = "伸直延迟比例 (PunchExtendDelay)", ToolTip = "前送过程中延后伸肘的比例；过大会显得出手迟缓。", UIMax = "0.9"))
	float PunchExtendDelay = 0.0f;

	/** 出拳方向相对身体正前方最多偏多少度；再多就只能靠转身补，否则目标落在肩膀活动范围之外 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "直拳",
	          meta = (ClampMin = "0.0", ClampMax = "90.0", DisplayName = "拳路偏转上限 (PunchYawLimit)", ToolTip = "拳路相对身体向左右偏转的最大角度。", Units = "deg", UIMax = "90"))
	float PunchYawLimit = 50.0f;

	// 动作过渡速度
	/** 收拳、放手、前送、收回的速度，单位是每秒完成的姿势比例 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "节奏", meta = (ClampMin = "0.5", DisplayName = "蓄力姿势变化速度 (WindupSpeed)", ToolTip = "每秒推进的姿势比例。增大更快后收；需与蓄力等待时间配合。", UIMax = "15"))
	float WindupSpeed = 4.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "节奏", meta = (ClampMin = "0.5", DisplayName = "蓄力姿势退出速度 (WindupReleaseSpeed)", ToolTip = "释放后旧蓄力姿势消退速度；不是整体收拳速度。", UIMax = "15"))
	float WindupReleaseSpeed = 3.0f;

	/**
	 * 前送速度；目标点冲得比手臂能跟上的更快时，电机只会原地抽一下，
	 * 看起来就是摆了摆手；留出让整条手臂真正走完这段弧线的时间
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "节奏", meta = (ClampMin = "0.5", DisplayName = "前送姿势变化速度 (PunchExtendSpeed)", ToolTip = "每秒推进伸拳比例。过快时目标先到而实际手臂跟不上。", UIMax = "20"))
	float PunchExtendSpeed = 10.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "节奏", meta = (ClampMin = "0.5", DisplayName = "收回姿势变化速度 (PunchRetractSpeed)", ToolTip = "每秒推进收拳比例；增大缩短收回过程，过大容易显得突然。", UIMax = "15"))
	float PunchRetractSpeed = 4.0f;

	// 各动作的控制强度
	/** 三个阶段的肌肉强度，单位是自然频率（Hz）；站着可以松，出拳必须硬 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "肌肉", meta = (ClampMin = "0.0", DisplayName = "待机手臂强度 (RestStrength)", ToolTip = "待机手臂的角度驱动强度。此项目使用 Physics Control 自然频率参数。", Units = "Hz", UIMax = "20"))
	float RestStrength = 5.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "肌肉", meta = (ClampMin = "0.0", DisplayName = "蓄力手臂强度 (WindupStrength)", ToolTip = "蓄力阶段维持姿势的角度驱动强度。", Units = "Hz", UIMax = "50"))
	float WindupStrength = 16.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "肌肉", meta = (ClampMin = "0.0", DisplayName = "出拳手臂强度 (PunchStrength)", ToolTip = "前送时追随目标的强度；先检查目标和限位，不要一味加大。", Units = "Hz", UIMax = "60"))
	float PunchStrength = 39.0f;

	/** 持物时持续托举用的手臂强度；低于出拳，避免肩膀把松软躯干反复拧动 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "肌肉", meta = (ClampMin = "0.0", DisplayName = "持物手臂强度 (GrabStrength)", ToolTip = "持物阶段的手臂强度，与出拳强度分开。", Units = "Hz", UIMax = "50"))
	float GrabStrength = 22.0f;

	/** 上臂的强度倍率；肩膀要比小臂硬，才是肩带着手走；否则就是小臂在甩巴掌 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "肌肉",
	          meta = (ClampMin = "0.1", ClampMax = "4.0", DisplayName = "上臂强度倍率 (UpperArmStrengthScale)", ToolTip = "相对基础手臂强度的倍率；肩膀过软时可小幅增加。", UIMax = "4"))
	float UpperArmStrengthScale = 1.95f;

	/**
	 * 拳头的强度倍率；手腕软了拳头会拖在手臂后面甩过去，打上去像拍
	 * 但绷得比小臂还硬又会把整条手臂带着走；留在小臂之下、接近它就行
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "肌肉",
	          meta = (ClampMin = "0.1", ClampMax = "2.0", DisplayName = "手腕强度倍率 (HandStrengthScale)", ToolTip = "相对基础手臂强度的倍率；过低拳头会拖后，过高可能牵动整条手臂。", UIMax = "2"))
	float HandStrengthScale = 0.85f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "肌肉", meta = (ClampMin = "0.0", DisplayName = "手臂阻尼比例 (DampingRatio)", ToolTip = "抑制来回振荡。增加可能减轻抖动，也可能让动作变钝。", UIMax = "3"))
	float DampingRatio = 1.1f;

	// 关节活动范围
	/**
	 * 肩关节允许的摆动和扭转上限（度）；物理资产给的默认锥角只有几十度，
	 * 而收拳要把上臂抬到肩前、直拳还要再往前送，限位一卡住就只剩小臂能动
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "关节",
	          meta = (ClampMin = "20.0", ClampMax = "170.0", DisplayName = "肩部摆动限位 (ShoulderSwingLimit)", ToolTip = "关节允许的摆动角度。启动时建立约束，修改后停止并重新运行。", Units = "deg", UIMax = "170"))
	float ShoulderSwingLimit = 110.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "关节",
	          meta = (ClampMin = "10.0", ClampMax = "170.0", DisplayName = "肩部扭转限位 (ShoulderTwistLimit)", ToolTip = "关节允许的扭转角度。修改后停止并重新运行。", Units = "deg", UIMax = "170"))
	float ShoulderTwistLimit = 75.0f;

	/** 肘关节限位；收拳时肘部要折到接近 130 度 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "关节",
	          meta = (ClampMin = "20.0", ClampMax = "170.0", DisplayName = "肘部摆动限位 (ElbowSwingLimit)", ToolTip = "肘部弯曲的角度范围。修改后停止并重新运行。", Units = "deg", UIMax = "170"))
	float ElbowSwingLimit = 150.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "关节",
	          meta = (ClampMin = "10.0", ClampMax = "170.0", DisplayName = "肘部扭转限位 (ElbowTwistLimit)", ToolTip = "肘部扭转范围。过大会出现不自然翻转；修改后重新运行。", Units = "deg", UIMax = "170"))
	float ElbowTwistLimit = 45.0f;
};
