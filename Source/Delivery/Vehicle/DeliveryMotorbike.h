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
class UMaterialInterface;
class UPoseableMeshComponent;
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

	/** 轮子转轴的固定槽位数。摩托车两个轮，留 4 个余量给以后的三轮/挂斗。 */
	static constexpr int32 MaxWheels = 4;

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

	/**
	 * 轮子转轴。和 SteerPivot/BarPivot/RiderPivot 一样是"挂点"：自己摆到轮心上，
	 * 轮子网格把这段偏移减回去，于是网格留在原地但从此绕轮心转。
	 *
	 * 前轮的转轴要挂在 SteerPivot 下面（跟着龙头转再自转），后轮挂在 MeshAlign 下面。
	 * 挂哪边由 ApplyBodyMeshes 按这个轮子在不在 SteeringPartIndices 里自己决定。
	 */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category="Components")
	TArray<TObjectPtr<USceneComponent>> WheelPivots;

	/** 打满角度跟转的槽位（前轮、前叉）。由 setup_motorbike.py 按几何认出来填，别手填。 */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category="Motorbike|Mesh")
	TArray<int32> SteeringPartIndices;

	/** 只跟转一部分的槽位（车把）。 */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category="Motorbike|Mesh")
	TArray<int32> HandlebarPartIndices;

	/**
	 * 哪几个槽位是轮子。由 setup_motorbike.py 按几何认（正圆且窄），别手填。
	 * 实测两个轮子的包围盒都是 61.3 x 61.3、宽 27.7，圆度 1.00，和别的部件差得很开。
	 */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category="Motorbike|Mesh")
	TArray<int32> WheelPartIndices;

	/** 每个轮子的轮心，和 WheelPartIndices 一一对应，MeshAlign 局部空间。 */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category="Motorbike|Mesh")
	TArray<FVector> WheelCenters;

	/** 轮子半径（导入空间 cm）。决定同样车速下轮子转多快，脚本按包围盒量。 */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category="Motorbike|Mesh", meta=(ClampMin="1.0", Units="cm"))
	float WheelRadius = 30.0f;

	/** 转向轴位置，MeshAlign 局部空间。同样由脚本量出来填。 */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category="Motorbike|Mesh")
	FVector SteerPivotLocation = FVector::ZeroVector;

	/** 骑手胯部位置（脚本从 hips 骨骼读出来），骑手绕这根竖轴扭身。 */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category="Motorbike|Mesh")
	FVector RiderPivotLocation = FVector::ZeroVector;

	/**
	 * 骑手网格。用 PoseableMesh 而不是 SkeletalMesh：
	 *
	 * 骑手根本不需要动画——坐姿就写在这份资产的参考姿势里，本来就是"停在参考姿势上"。
	 * 而要让脖子跟着转向偏一点，就得在 C++ 里改单根骨骼；SkeletalMeshComponent 没有
	 * 改单根骨骼的接口，它每帧都会把单节点动画（= 参考姿势）重新求值盖回去，
	 * 想改只能配 AnimBP + Transform(Modify)Bone，而 AnimGraph 必须在编辑器里手连。
	 * PoseableMesh 就是为"纯 C++ 摆骨骼、不跑动画"准备的，SetBoneTransformByName 直接可用。
	 */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category="Components")
	TObjectPtr<UPoseableMeshComponent> RiderMesh;

	/**
	 * 有人骑的时候，把骑手网格的材质换成驾驶员自己身上那套。
	 *
	 * 车模型自带的骑手那个 tripo_mat 里**一张贴图都没有**，进游戏就是一块灰白。
	 *
	 * 能直接换是因为两边是同一个基础角色：玩家网格（Characters/A/renwu）和骑手
	 * 槽位名完全一致（tripo_mat_c9b1ab96 / 材质 / 材质_001…材质_006 / Eyes_Black），
	 * UV 也是同一套，所以按名字对应贴过去就是玩家本人的样子，不会错位。
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Motorbike|表现")
	bool bUseDriverMaterials = true;

	/**
	 * 手动指定骑手每个槽位的材质，按槽位下标对应。填了的槽位优先于上面的自动映射，
	 * 留空（None）的槽位仍然走自动映射。美术想单独给骑手配一套时用这个，不用改代码。
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Motorbike|表现")
	TArray<TObjectPtr<UMaterialInterface>> RiderMaterialOverrides;

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
	float MaxGroundAlignAngle = 40.0f;

	// ---- 爬坡越障 ----

	/**
	 * 能直接跨上去的台阶高度（马路牙子、门槛）。
	 *
	 * 水平推进是带 sweep 的，碰到台阶就是一堵墙、只会掉速。这里做的是和角色移动
	 * 一样的"抬起来 → 往前 → 落下去"三步：抬 MaxStepHeight、走完这一帧剩下的位移、
	 * 再往下探回地面。三步里任何一步撞住就整体撤回，当成撞墙处理。
	 * 别调太大：它同时也是"能凭空抬多高"的上限，过大时车会爬上本该挡住它的矮墙。
	 */
	UPROPERTY(EditAnywhere, Category="Motorbike|爬坡越障", meta=(ClampMin="0.0", Units="cm"))
	float MaxStepHeight = 45.0f;

	/**
	 * 最陡能爬的坡度。比这更陡的面当墙，跨上去也不算站住。
	 * 和 MaxGroundAlignAngle 保持一致：能爬上去的坡，车身姿态就该跟着贴上去。
	 */
	UPROPERTY(EditAnywhere, Category="Motorbike|爬坡越障", meta=(ClampMin="0.0", ClampMax="80.0"))
	float MaxClimbAngle = 40.0f;

	/**
	 * 贴地高度往上追的速度下限（cm/s）。
	 *
	 * 只靠 GroundSnapSpeed 的指数插值上坡会一直欠着一截：坡越陡、车越快，欠得越多，
	 * 而下一帧的水平 sweep 是在这个"偏低"的位置做的，于是直接撞在坡面上掉速——
	 * 表现就是"一点爬坡能力都没有"。上坡时保证至少这个爬升率，就不会被自己绊住。
	 */
	UPROPERTY(EditAnywhere, Category="Motorbike|爬坡越障", meta=(ClampMin="0.0", Units="cm/s"))
	float MaxClimbRate = 600.0f;

	/**
	 * 贴地射线往前看多远。上坡/上台阶时提前抬车，免得等撞上了才反应。
	 * 实际前瞻距离取"这一帧位移的两倍"和这个值里的小者，所以停着不会凭空浮起来。
	 */
	UPROPERTY(EditAnywhere, Category="Motorbike|爬坡越障", meta=(ClampMin="0.0", Units="cm"))
	float GroundLookAhead = 90.0f;

	/** 真撞墙（跨不上去）时车速剩下多少。原来写死 0.3。 */
	UPROPERTY(EditAnywhere, Category="Motorbike|爬坡越障", meta=(ClampMin="0.0", ClampMax="1.0"))
	float WallHitSpeedScale = 0.3f;

	/** 离地多高还算"贴着地"，超过就交给重力。要大于 MaxStepHeight，否则刚跨上台阶就被判成起飞。 */
	UPROPERTY(EditAnywhere, Category="Motorbike|贴地", meta=(ClampMin="0.0", Units="cm"))
	float GroundStickTolerance = 80.0f;

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
	float HandlebarSteerRatio = 0.65f;

	/** 骑手扭身跟转的比例。调大了脚会离开脚踏。 */
	UPROPERTY(EditAnywhere, Category="Motorbike|表现", meta=(ClampMin="0.0", ClampMax="1.0"))
	float RiderSteerRatio = 0.45f;

	/**
	 * 脖子跟着转向额外偏转的比例（叠在骑手扭身之上）。
	 *
	 * 人转弯是先看向弯心、身体再跟上，只转身不转头像个木头人。脖子这一层很便宜：
	 * 头是骨骼链末端，转它不影响手和脚的位置，不会像扭身那样把脚扭离脚踏。
	 */
	UPROPERTY(EditAnywhere, Category="Motorbike|表现", meta=(ClampMin="0.0", ClampMax="2.0"))
	float RiderNeckSteerRatio = 0.6f;

	/** 脖子骨骼名。这份资产是 Mixamo 骨架，所以带 mixamorig: 前缀。换模型时改这里。 */
	UPROPERTY(EditAnywhere, Category="Motorbike|表现")
	FName RiderNeckBone = TEXT("mixamorig:Neck");

	/**
	 * 骑车时鼠标能不能转视角。
	 *
	 * 关掉 = 回到最早那套**固定车尾视角**：弹簧臂 `bUsePawnControlRotation=false`、只继承 Yaw、
	 * 俯仰吃 `CameraPitch` 的相对角度，鼠标完全不参与，"W 永远是往屏幕里开"。
	 * 那套的全部设置都还留在代码里（见 ApplyCameraMode），这个开关就是回退按钮，
	 * 在 BP_Motorbike 上取消勾选即可，不用重编。
	 *
	 * 开着 = 自由视角：弹簧臂吃控制旋转，鼠标绕车转圈、可以回头看。
	 * 代价是 W 按的是**车头**方向而不是屏幕里的方向——镜头转到侧面时按 W，车还是往它自己
	 * 车头那边开。载具游戏普遍如此，但和固定视角的手感确实不一样，所以才留了开关。
	 */
	UPROPERTY(EditAnywhere, Category="Motorbike|表现")
	bool bFreeLookCamera = true;

	/**
	 * 轮子自转的方向。
	 *
	 * 推导：MeshAlign 局部 +Y 是车头方向，轮轴是局部 X，绕它转就是 Roll。
	 * UE 里正 Roll 把 +Z 转向 -Y（"正 Roll 往左倒"那条的同一个约定），
	 * 也就是轮子顶部往后走 = 倒着滚，所以前进要取负号。
	 * 万一我这个符号推反了，把它改成 +1 就好，不用重编。
	 */
	UPROPERTY(EditAnywhere, Category="Motorbike|表现")
	float WheelSpinSign = -1.0f;

	/** 骑车时镜头的俯角。固定视角下是写死的俯角；自由视角下只当上车那一刻的起始俯角。 */
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
	float KnockDownLaunchSpeed = 1400.0f;

	UPROPERTY(EditAnywhere, Category="Motorbike|被撞", meta=(ClampMin="0.0", Units="cm/s"))
	float KnockDownLaunchUp = 550.0f;

	/**
	 * 抛射力度随来车速度淡入的参考车速：来车到这个速度就是满力度（上面那两个值）。
	 * 关卡里的交通车 MaxSpeed 是 600~1500 随机的，取 1200 让大部分快车都接近满力度、
	 * 慢车明显轻一些，而不是不管多慢都一个飞法。
	 */
	UPROPERTY(EditAnywhere, Category="Motorbike|被撞", meta=(ClampMin="1.0", Units="cm/s"))
	float KnockDownSpeedReference = 1200.0f;

	/**
	 * 来车速度接近 0 时的最低抛射比例。不能给 0：那样慢慢蹭一下人就原地瘫软，
	 * 看着像自己躺下的，不像被撞的。
	 */
	UPROPERTY(EditAnywhere, Category="Motorbike|被撞", meta=(ClampMin="0.0", ClampMax="1.0"))
	float KnockDownLaunchMinScale = 0.35f;

	UPROPERTY(EditAnywhere, Category="Motorbike|上下车")
	FText DrivePromptText;

	UPROPERTY(EditAnywhere, Category="Motorbike|上下车")
	FText ExitPromptText;

	/**
	 * "按 F 下车"提示在上车后显示多久。
	 * 一直挂着的话它会长在屏幕中间挡视野，而这条信息只在刚上车那会儿有用。
	 */
	UPROPERTY(EditAnywhere, Category="Motorbike|上下车", meta=(ClampMin="0.0", Units="s"))
	float ExitPromptDuration = 2.0f;

	UPROPERTY(EditAnywhere, Category="Input")
	TObjectPtr<UInputAction> MoveAction;

	/**
	 * 局内切换视角的按键。默认 P——扫过 IMC_Default 的名字表，里面只有
	 * A/D/E/F/J/S/SpaceBar/W（J 是电话），P 是空的。
	 *
	 * 故意用 BindKey 直接绑键，而不是再建一个 InputAction + 在 IMC_Default 上映射：
	 * 那个资产从来没被提交过，每次 git 拉取都会把映射冲掉（F 键就这么没过两次），
	 * 直接绑键没有这个失败模式。物品栏 1~5 也是同样的理由直接绑的。
	 */
	UPROPERTY(EditAnywhere, Category="Input")
	FKey CameraToggleKey;

	/** 手柄右摇杆看向。只有 bFreeLookCamera 开着才起作用。 */
	UPROPERTY(EditAnywhere, Category="Input")
	TObjectPtr<UInputAction> LookAction;

	/** 鼠标看向。和角色身上用的是同两个 IA，手感一致。 */
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

	/** 局内切一次视角模式，并在屏幕上提示现在是哪种。 */
	void ToggleCameraMode();

	/** 按 bFreeLookCamera 把弹簧臂配成自由视角或固定车尾视角。上车时和 BeginPlay 各调一次。 */
	void ApplyCameraMode();
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

	/** 把驾驶员身上的材质映射到骑手网格的槽位上；没人骑就还原成资产自带的。 */
	void ApplyRiderMaterials();

	void UpdateSpeed(float DeltaSeconds);
	void UpdateSteering(float DeltaSeconds);
	void UpdateGroundAndMove(float DeltaSeconds);

	/**
	 * 水平推进被挡住时试着跨上去。成功返回 true（车已经在台阶上了），
	 * 失败会把车放回调用前的位置，调用方按撞墙处理。
	 */
	bool TryStepUp(const FVector& RemainingDelta, const FHitResult& BlockingHit);
	void UpdateLean(float DeltaSeconds);
	void UpdateSteerVisual(float DeltaSeconds);

	/** 轮子按车速自转。 */
	void UpdateWheelSpin(float DeltaSeconds);

	/** 脖子跟着转向偏一点。单独一个函数是因为它改的是骨骼，不是组件变换。 */
	void UpdateRiderNeck();
	void UpdateImpactReaction(float DeltaSeconds);
	/** 把 NoticeText 推给浮窗子系统（约定是每帧推一次，停推 0.25 秒自动消失）。 */
	void PushNotice();

	/** 让某句提示在屏幕上停留 Seconds 秒。上车提示和切视角提示共用这一套。 */
	void ShowNotice(const FText& Text, float Seconds);

	/** 把人打倒：血清零交给既有的晕倒流程，再补一记冲量把他抛离车身。 */
	void KnockDownDriver(ADeliveryCharacter* Rider, const FVector& LaunchDirection, float LaunchScale);

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

	/** 轮子累计转角（度）。一直累加、不取模——FRotator 自己会归一化。 */
	float WheelSpinAngle = 0.0f;

	/** 上一帧位置。远端客户端上 CurrentSpeed 恒为 0，只能靠位移反推车速来转轮子。 */
	FVector LastWheelLocation = FVector::ZeroVector;
	bool bWheelLocationValid = false;
	bool bWasGrounded = true;

	/** 本次骑行已经被交通车撞了几下。只在服务器上维护，上/下车时归零。 */
	int32 TrafficImpactCount = 0;

	/** 被撞之后的残余状态，全部按 KnockDecay 指数衰减回 0。 */
	FVector KnockVelocity = FVector::ZeroVector;
	float KnockYawRate = 0.0f;
	float KnockTilt = 0.0f;
	float ShakeAmount = 0.0f;
	float ShakePhase = 0.0f;

	/** 当前这句提示还剩多少秒。上车那一帧由 ExitPromptDuration 起算，只在本机上用。 */
	float NoticeRemaining = 0.0f;
	FText NoticeText;
	bool bHadDriverLastFrame = false;

	/**
	 * 脖子骨骼在参考姿势下的组件空间变换。转头是在它之上叠一个偏转，所以必须先记住原值——
	 * 每帧读"当前值"再转会一直累加，头会一路转到背后去。
	 */
	FTransform NeckRefTransform = FTransform::Identity;
	bool bNeckRefCached = false;
};
