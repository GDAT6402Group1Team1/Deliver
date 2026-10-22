// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Pawn.h"
#include "Vehicle/DeliveryRiderPose.h"
#include "DeliveryMotorbike.generated.h"

class ADeliveryCharacter;
class ADeliveryPackageItem;
class UBoxComponent;
class UCameraComponent;
class UAudioComponent;
class UDeliveryInteractableComponent;
class UInputAction;
class UMaterialInterface;
class USkeletalMeshComponent;
class UAnimInstance;
struct FDeliveryRiderFrame;
class USpringArmComponent;
class USkeletalMesh;
class UStaticMesh;
class UStaticMeshComponent;
class USoundBase;
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
	friend class FDeliveryRiderContactsTest;
	friend class FDeliveryRiderRuntimeTest;
	friend class FDeliveryVehicleSummonTest;

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

	/**
	 * 服务器：把车挪到指定位置（召唤）。会自己往下探地面、清掉残余车速和被撞状态。
	 * 车上有人时拒绝——不能把别人正骑着的车抽走。找不到地面也拒绝，免得把车塞进虚空。
	 */
	UFUNCTION(BlueprintCallable, Category="Motorbike")
	bool SummonTo(const FVector& DesiredLocation, float DesiredYaw);

	/** 服务器：下车，把驾驶员放回车边的地面。 */
	UFUNCTION(BlueprintCallable, Category="Motorbike")
	void ExitVehicle();

	/**
	 * 服务器：把一件快递绑到车上。已经绑着别的就拒绝。
	 *
	 * 绑上之后快递**关掉物理、焊在 CargoPivot 上**，不是用物理约束拴着：
	 * 车是运动学的（`bSimulatePhysics=false`，靠 SetActorLocation 沿样条/输入走），
	 * 约束的另一端是个"无穷重且不讲道理"的刚体，Chaos 会把误差全算到快递头上
	 * ——表现是快递在车后面疯狂抽动，拖人那套就踩过这个坑（见 CLAUDE.md 抓取一节）。
	 * 焊死没有这个失败模式，而且"绑在车上"本来就该是硬连接。
	 */
	UFUNCTION(BlueprintCallable, Category="Motorbike|货物")
	bool AttachCargo(ADeliveryPackageItem* Package);

	/**
	 * 服务器：解下车上的快递并返回它；车上没货返回 null。
	 *
	 * 只解绑、不恢复物理也不放到地上——调用方才知道接下来它要进手里还是掉地上。
	 * 恢复物理这件事交给 `ADeliveryHandheldItem` 自己那套表现逻辑，两处都做会打架。
	 */
	UFUNCTION(BlueprintCallable, Category="Motorbike|货物")
	ADeliveryPackageItem* ReleaseCargo();

	/**
	 * 车上的货。过 IsValid 而不是直接判空：快递被别处销毁（交付、掉出世界、
	 * 关卡清理）时这个指针会悬着，裸判空会让"已经没货了却说有货"，
	 * 表现是下一件快递永远领不到。
	 */
	UFUNCTION(BlueprintPure, Category="Motorbike|货物")
	ADeliveryPackageItem* GetCargo() const;

	UFUNCTION(BlueprintPure, Category="Motorbike|货物")
	bool HasCargo() const { return GetCargo() != nullptr; }

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

	/** 主线程供骑手动画读取本帧输入和四个组件空间接触点。 */
	void BuildRiderAnimationFrame(FDeliveryRiderFrame& OutFrame) const;

	/** 从模型坐姿自动标定手把/脚踏目标；也可关闭自动标定后手调四个组件。 */
	UFUNCTION(BlueprintCallable, CallInEditor, Category="Motorbike|骑手动画")
	void CalibrateRiderContacts();

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Motorbike|骑手动画")
	FDeliveryRiderSettings RiderAnimationSettings;

	UPROPERTY(EditAnywhere, Category="Motorbike|骑手动画")
	bool bAutoCalibrateRiderContacts = true;

	UPROPERTY(EditAnywhere, Category="Motorbike|骑手动画")
	TSubclassOf<UAnimInstance> RiderAnimationClass;

	/**
	 * 骑手骨骼网格的兜底来源。
	 *
	 * RiderMesh 组件上的资产引用是存在 BP_Motorbike 的 CDO 里的，**组件类型一变就会丢**
	 * （SkeletalMesh → PoseableMesh 那次就丢了），于是又得重跑一次 setup_motorbike.py。
	 * 这里按软引用晚绑兜底：组件上没挂东西就自己加载这个路径，脚本变成可选的。
	 * 和 InteractAction 用 TSoftObjectPtr 是同一个理由——构造函数只在模块加载时跑一次，
	 * 软引用才能在运行时才解析。
	 */
	UPROPERTY(EditAnywhere, Category="Motorbike|Mesh")
	TSoftObjectPtr<USkeletalMesh> RiderMeshAsset;

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
	 * 货架：绑在车上的快递焊在这一层。
	 *
	 * 挂在 MeshAlign 下面，所以它继承了车头朝向修正和压弯侧倾——货物会跟着车一起倒，
	 * 而不是在压弯时保持水平"飘"在车边上。挂 MeshRoot 的话就只有侧倾没有朝向修正，
	 * 默认偏移里那个"往后"会变成"往右"。
	 *
	 * 默认位置在座位后方偏上（后座 / 尾架），由 `CargoOffset` 调。
	 */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category="Components")
	TObjectPtr<USceneComponent> CargoPivot;

	/**
	 * 货架相对 MeshAlign 的位置。MeshAlign 局部空间里 **+Y 是车头**
	 * （模型车头朝 +Y，靠 MeshAlign 的 −90° Yaw 纠正到 Pawn 的 +X），
	 * 所以"往后"是 −Y，不是 −X —— 填错的表现是快递挂在车的侧面。
	 *
	 * **留零 = 自动按车身包围盒量**（`AutoConfigureFromMeshes` 里和轮子、车把
	 * 同一套做法）：取车尾那批部件的顶面中点。换车模型不用重新手调，
	 * 也不用重跑脚本。不满意就在 BP 上填个非零值，自动标定会让开。
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Motorbike|货物")
	FVector CargoOffset = FVector::ZeroVector;

	/** 货架朝向，相对 MeshAlign。让箱子正面朝外、不至于歪着。 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Motorbike|货物")
	FRotator CargoRotation = FRotator::ZeroRotator;

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
	 * 独立骑手骨骼网格：AnimBP 的原生代理从参考坐姿生成身体惯性，再求手脚 IK。
	 * 不参与车体碰撞，也不直接修改原角色布娃娃。材质仍按驾驶者映射。
	 */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category="Components")
	TObjectPtr<USkeletalMeshComponent> RiderMesh;

	/** 双手目标跟随 BarPivot，双脚目标跟随 MeshAlign；身体骨骼晃动不移动这些点。 */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category="Components")
	TObjectPtr<USceneComponent> LeftHandGrip;
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category="Components")
	TObjectPtr<USceneComponent> RightHandGrip;
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category="Components")
	TObjectPtr<USceneComponent> LeftFootPeg;
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category="Components")
	TObjectPtr<USceneComponent> RightFootPeg;

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

	/** 行驶循环音。声音资产本身需要设置为循环，C++ 负责启停和随车速调音。 */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category="Components")
	TObjectPtr<UAudioComponent> EngineAudio;

	UPROPERTY(EditAnywhere, Category="Motorbike|Audio")
	TObjectPtr<USoundBase> EngineLoopSound;
	UPROPERTY(EditAnywhere, Category="Motorbike|Audio")
	TObjectPtr<USoundBase> EnterVehicleSound;
	UPROPERTY(EditAnywhere, Category="Motorbike|Audio")
	TObjectPtr<USoundBase> ExitVehicleSound;
	UPROPERTY(EditAnywhere, Category="Motorbike|Audio")
	TObjectPtr<USoundBase> TrafficImpactSound;
	UPROPERTY(EditAnywhere, Category="Motorbike|Audio")
	TObjectPtr<USoundBase> SummonSound;
	UPROPERTY(EditAnywhere, Category="Motorbike|Audio")
	TObjectPtr<USoundBase> CameraToggleSound;

	UPROPERTY(EditAnywhere, Category="Motorbike|Audio", meta=(ClampMin="0.1"))
	float EngineIdlePitch = 0.75f;
	UPROPERTY(EditAnywhere, Category="Motorbike|Audio", meta=(ClampMin="0.1"))
	float EngineMaxPitch = 1.5f;
	UPROPERTY(EditAnywhere, Category="Motorbike|Audio", meta=(ClampMin="0.0"))
	float EngineIdleVolume = 0.35f;
	UPROPERTY(EditAnywhere, Category="Motorbike|Audio", meta=(ClampMin="0.0"))
	float EngineMaxVolume = 1.0f;

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
	float MaxGroundAlignAngle = 45.0f;

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
	float MaxStepHeight = 60.0f;

	/**
	 * 最陡能爬的坡度。比这更陡的面当墙，跨上去也不算站住。
	 * 和 MaxGroundAlignAngle 保持一致：能爬上去的坡，车身姿态就该跟着贴上去。
	 */
	UPROPERTY(EditAnywhere, Category="Motorbike|爬坡越障", meta=(ClampMin="0.0", ClampMax="80.0"))
	float MaxClimbAngle = 45.0f;

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
	float GroundLookAhead = 120.0f;

	/** 真撞墙（跨不上去）时车速剩下多少。原来写死 0.3。 */
	UPROPERTY(EditAnywhere, Category="Motorbike|爬坡越障", meta=(ClampMin="0.0", ClampMax="1.0"))
	float WallHitSpeedScale = 0.5f;

	/**
	 * 跨越台阶时，抬起来之后至少要走完这一帧剩余位移的多少比例，才算"跨上去了"。
	 *
	 * 原来是 `TryStepUp` 里写死的 0.1：抬高之后前进不足剩余位移的 10% 就整体撤回、
	 * 当成撞墙处理。这一条是"车莫名被地形卡住"的主要来源——**一堵真墙和"台阶上方
	 * 还有点别的东西蹭一下"在这个判据下长得一模一样**，而后者只要让它慢慢往前挤就过去了。
	 * 调小 = 更宽容（接受很小的进展，不轻易判定撞墙）；
	 * 调大 = 更严格（容易被判成墙，但也更不容易贴着墙面往上爬）。
	 * 给 0 等于"只要不是完全走不动就算跨过去了"。
	 */
	UPROPERTY(EditAnywhere, Category="Motorbike|爬坡越障", meta=(ClampMin="0.0", ClampMax="1.0"))
	float StepUpMinProgress = 0.03f;

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

	/**
	 * 车把转前轮的百分之多少。手部目标跟随实际车把，不再依赖整人转角近似贴合。
	 * 增大后需验证骑手骨架能否够到，不能用无限拉长手臂来补偿不合理的目标。
	 */
	UPROPERTY(EditAnywhere, Category="Motorbike|表现", meta=(ClampMin="0.0", ClampMax="1.0"))
	float HandlebarSteerRatio = 0.5f;

	/** 历史资产兼容字段，不再执行整人扭转；新参数见 RiderAnimationSettings。 */
	UPROPERTY(EditAnywhere, Category="Motorbike|表现", meta=(ClampMin="0.0", ClampMax="1.0"))
	float RiderSteerRatio = 0.45f;

	/**
	 * 历史资产兼容字段。头部现由动画代理计算反向稳定，不再叠旧版脖子转角。
	 */
	UPROPERTY(EditAnywhere, Category="Motorbike|表现", meta=(ClampMin="0.0", ClampMax="2.0"))
	float RiderNeckSteerRatio = 0.6f;

	/** 历史资产兼容字段；换骨架需修改 DeliveryRiderPose 的骨骼映射并重新验证。 */
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
	 * 自由视角下 WASD 按**镜头**的前后左右走，而不是按车头。
	 *
	 * 关掉就是原来的开法：W 永远是车头方向、A/D 是转向量。固定视角下这个开关
	 * 不起作用——那套镜头的 yaw 本来就跟着车头，两种解释是同一回事。
	 *
	 * 做法：把摇杆/WASD 的二维输入在镜头 yaw 下解释成一个"想去的世界方向"，
	 * 再把它换算成这套车本来就有的 油门 + 转向量。不是另写一套移动，
	 * 所以爬坡、撞墙掉速、压弯那些全都照旧生效。
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Motorbike|镜头")
	bool bCameraRelativeMoveInFreeLook = true;

	/**
	 * 车头和"想去的方向"差多少度算满舵（度）。
	 * 小了会在小角度就打满、走蛇形；大了转向迟钝、过弯要压很久。
	 */
	UPROPERTY(EditAnywhere, Category = "Motorbike|镜头", meta = (ClampMin = "5.0", ClampMax = "180.0"))
	float CameraRelativeFullSteerAngle = 50.0f;

	/**
	 * 方向差很大时油门的下限（0~1）。
	 *
	 * **不能给 0。** 转向量乘的是 `|车速| / TurnSpeedReference`，车停下来就完全不能转
	 * （摩托车停着打把原地转圈很出戏，这是刻意的）。所以"想掉头"的时候如果把油门收到 0，
	 * 车会停住 → 不能转 → 永远掉不了头，卡成死局。留一个下限让它保持滚动、
	 * 边走边转，自然画出一个 U 形。
	 */
	UPROPERTY(EditAnywhere, Category = "Motorbike|镜头", meta = (ClampMin = "0.05", ClampMax = "1.0"))
	float CameraRelativeMinThrottle = 0.4f;

	/**
	 * 方向差超过这个角度（度）且车还在往前跑时，先当**刹车**再掉头。
	 *
	 * 针对的是"高速行驶中把镜头转过头再按 W"：不先刹就会以当前车速画一个巨大的弧，
	 * 看着像失控漂出去。先把速度卸掉、再原地附近掉头，和真人骑车一致。
	 * **这条和 S 没有关系**——S 是后退（见 ApplyCameraRelativeInput 里的分支），
	 * 压根不走方向换算。
	 */
	UPROPERTY(EditAnywhere, Category = "Motorbike|镜头", meta = (ClampMin = "90.0", ClampMax = "180.0"))
	float CameraRelativeBrakeAngle = 120.0f;

	/**
	 * 方向差超过这个角度（度）就进入**分段转向**：先只转到这个角度（"侧面"），
	 * 转过去之后才由真实误差收尾。同时它也是解除锁定的阈值。
	 *
	 * **为什么必须分段 + 锁定**，两个独立的颤抖来源缺一个都还会抖：
	 * 1. `Error` 用 `NormalizeAxis` 归到 `(-180, 180]`，**180 是奇点**——鼠标或车身一丝抖动
	 *    就让它在 +180 和 −180 之间翻号，`SteerInput` 被 clamp 成 ±1、满舵方向每帧左右互换。
	 *    **实测踩中的场景：车头朝前、把镜头转 180° 再按 W**——目标方向正好落在车身后方，
	 *    `Error` 恰好 180。（`S` 不会踩中：它是后退，不走这条换算。）
	 * 2. 大角度时刹车分支和掉头分支在**同一个速度阈值**（`TurnSpeedReference * 0.25`）上
	 *    来回弹：越过就刹车（油门 −1、舵角归零），刹到阈值以下又回掉头（油门 +0.4、满舵），
	 *    进出同一个阈值、没有滞环，就是个极限循环。**这一条和角度是不是 180 无关**，
	 *    所以镜头转到任何大角度再按 W/A/D 都会抖、不止正后方，而且要一直抖到误差小于
	 *    `CameraRelativeBrakeAngle` 才跳出来。
	 *
	 * 对策都是**锁定**：转向侧（`TurnAroundSign`）和"刹车还是直接掉头"（`bBrakingForTurn`）
	 * 都只在动作开始那一帧定一次，整个动作里不再重新判断。
	 * 在这个角上分段算法和常规算法给出的舵角本来就相等，所以交接处没有跳变。
	 */
	UPROPERTY(EditAnywhere, Category = "Motorbike|镜头", meta = (ClampMin = "30.0", ClampMax = "135.0"))
	float CameraRelativeStageSideAngle = 90.0f;

	/**
	 * 为掉头而刹车时，刹到多慢（cm/s）才转入掉头段。
	 *
	 * **必须远小于进入刹车的速度阈值**（`TurnSpeedReference * 0.25`，默认 112.5）。
	 * 两者取同一个值就是上面第 2 条那个极限循环：刹到阈值 → 转掉头 → 加速越过阈值
	 * → 又刹车，油门和舵角每几帧翻一次。
	 */
	UPROPERTY(EditAnywhere, Category = "Motorbike|镜头", meta = (ClampMin = "0.0"))
	float CameraRelativeBrakeExitSpeed = 25.0f;

	/**
	 * 两次撞击之间的最小间隔（秒）。比这更密的撞击直接不计数、不给击退。
	 *
	 * **为什么要在被撞者这边再挡一道**：交通车那边已经有一张 0.75 秒的冷却表
	 * （`UDeliveryTrafficCarComponent::LastImpactTimeByActor`），但那张表是
	 * **每辆车各一份**的组件成员，只能保证"同一辆车不连着撞"。多辆车可以在
	 * 同一帧各打一下，`ImpactsToDismount`（2）于是在一次碰撞里就被扣满——
	 * 实测日志里出现过同一帧两条撞击记录、来车速度完全相同（很可能是第二/三波
	 * 克隆车和本体重叠在同一位置、速度向量一致）：
	 *     [12.33.55:490][678] 被交通车撞击 1/2 次（来车速度 3053 cm/s）
	 *     [12.33.55:490][678] 被交通车撞击 2/2 次（来车速度 3053 cm/s）
	 * 玩家的体验就是"被撞一下就直接下车加晕倒"。
	 *
	 * 挡在这里才是对的层：次数是**摩托车自己**的状态，谁来撞、撞几辆都该由它说了算。
	 * 给 0 就退回原来的行为（只靠来车那边各自的冷却表）。
	 */
	UPROPERTY(EditAnywhere, Category = "Motorbike|撞击", meta = (ClampMin = "0.0"))
	float MinImpactInterval = 1.0f;

	/**
	 * 轮子要不要自转。暂时关掉——整套逻辑（认轮子、轮轴分层、按车速算转速）都留着，
	 * 想开回来只要在 BP_Motorbike 上勾一下，不用重编、也不用重跑 Setup Motorbike。
	 */
	UPROPERTY(EditAnywhere, Category="Motorbike|表现")
	bool bSpinWheels = false;

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

	/**
	 * 取件键（E）。骑车时按它**转发给驾驶员那具身体**去走它自己那套
	 * 本机探测 + 长按 + 服务器复核，而不是在车上另写一套。
	 *
	 * 那一套已经调通了（范围判定、准星排序、长按进度、服务器复核），复制一份
	 * 只会多一个要同步维护的地方；而且交付判定本来就认"玩家"不认"车"。
	 */
	UPROPERTY(EditAnywhere, Category="Input")
	TSoftObjectPtr<UInputAction> PickupAction;

private:

	void MoveInput(const FInputActionValue& Value);
	void LookInput(const FInputActionValue& Value);

	/** 局内切一次视角模式，并在屏幕上提示现在是哪种。 */
	void ToggleCameraMode();

	/** 按 bFreeLookCamera 把弹簧臂配成自由视角或固定车尾视角。上车时和 BeginPlay 各调一次。 */
	void ApplyCameraMode();
	void InteractPressed(const FInputActionValue& Value);
	void PickupPressed(const FInputActionValue& Value);
	void PickupReleased(const FInputActionValue& Value);

	UFUNCTION(Server, Unreliable)
	void ServerSetDriveInput(float InThrottle, float InSteer);

	UFUNCTION(Server, Reliable)
	void ServerRequestExit();

	/** 服务器确认的上下车/碰撞/召唤事件在所有客户端各自播放一次空间音。 */
	UFUNCTION(NetMulticast, Unreliable)
	void MulticastPlayAudioEvent(uint8 EventIndex);

	UFUNCTION()
	void HandleInteractRequested(APawn* Interactor);

	UFUNCTION()
	void OnRep_Driver();

	UFUNCTION()
	void OnRep_Cargo();

	/** 把 Cargo 焊到货架上（或在它变空时什么都不做）。两端都要跑。 */
	void ApplyCargoPresentation();

	/** 把 BodyMeshes 里的网格铺到固定槽位上，多余的槽位清空。 */
	void ApplyBodyMeshes();

	/**
	 * 没填的几何参数自己按包围盒量出来（轮子、车把、前轮前叉、转向轴）。
	 *
	 * 判据和 setup_motorbike.py 里那套**一模一样**，只是搬到了运行时：
	 * 这样脚本从"每次改完都必须跑"变成"可选的、跑了能在报告里看数字"。
	 * 只填空的那些，已经有值的一律不碰——脚本量过的、或者手工调过的都算数。
	 */
	void AutoConfigureFromMeshes();

	/** 按有没有驾驶员刷新骑手网格和可交互状态。服务器和客户端都会走到。 */
	void ApplyDriverPresentation();

	/** 把驾驶员身上的材质映射到骑手网格的槽位上；没人骑就还原成资产自带的。 */
	void ApplyRiderMaterials();

	void UpdateSpeed(float DeltaSeconds);
	void UpdateSteering(float DeltaSeconds);

	/** 把 MoveInputRaw 按镜头朝向换算成 ThrottleInput / SteerInput。每帧调一次。 */
	void ApplyCameraRelativeInput();

	/** 现在是不是按镜头方向开（开关打开 **且** 处于自由视角）。 */
	bool UsesCameraRelativeMove() const;
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
	void UpdateMotorbikeAudio();

	/** 脖子跟着转向偏一点。单独一个函数是因为它改的是骨骼，不是组件变换。 */
	void UpdateRiderMotion(float DeltaSeconds);
	void ResetRiderMotion();
	void UpdateImpactReaction(float DeltaSeconds);
	/** 把 NoticeText 推给浮窗子系统（约定是每帧推一次，停推 0.25 秒自动消失）。 */
	void PushNotice();

	/** 让某句提示在屏幕上停留 Seconds 秒。上车提示和切视角提示共用这一套。 */
	void ShowNotice(const FText& Text, float Seconds);

	/** 骑行期间常驻左下角的操作提示（切视角那条）。 */
	void PushCameraHint();

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

	/**
	 * 车上绑着的快递。复制给所有人——路过的别的玩家也该看见这车上有货。
	 *
	 * 用 OnRep 而不是在客户端自己判：附着这件事要在**客户端**也做一遍，
	 * 否则远端看到的快递还留在取货点原地（服务端的附着不会自动复制成
	 * 客户端的附着层级，`AttachToComponent` 的结果只对调用那一端生效）。
	 */
	UPROPERTY(ReplicatedUsing=OnRep_Cargo)
	TObjectPtr<ADeliveryPackageItem> Cargo;

	float ThrottleInput = 0.0f;
	float SteerInput = 0.0f;

	/**
	 * 摇杆/WASD 的原始二维输入。
	 *
	 * 镜头相对模式必须**每帧**重算油门和转向量：镜头和车头的夹角一直在变，
	 * 只在按键事件里算一次的话，松手之前方向就不会再更新。
	 * 所以这里存原始值，真正的换算放在 Tick 里（ApplyCameraRelativeInput）。
	 */
	FVector2D MoveInputRaw = FVector2D::ZeroVector;

	/**
	 * 两段式掉头锁定的转向侧（+1 右 / −1 左，0 = 没在掉头）。
	 *
	 * 锁定值只在进入掉头段的那一帧写一次，之后一路保持到转过侧面为止——
	 * 每帧重读 `Error` 的符号正是颤抖的来源（见 CameraRelativeStageSideAngle）。
	 * 松手、上下车、召唤时要和 MoveInputRaw 一起清掉，否则下一次掉头会沿用上一次的侧。
	 */
	float TurnAroundSign = 0.0f;

	/**
	 * 当前这次大角度动作是不是还在"先刹车"的阶段。
	 *
	 * 和 TurnAroundSign 一样**只在动作开始那一帧定一次**，之后一路刹到
	 * CameraRelativeBrakeExitSpeed 才转 false。每帧重判会让刹车和掉头在速度阈值上
	 * 来回弹（见 CameraRelativeStageSideAngle 的第 2 条）。
	 */
	bool bBrakingForTurn = false;

	/** 上次发给服务器的油门/转向，用来避免每帧重复发同样的值。 */
	float LastSentThrottle = 0.0f;
	float LastSentSteer = 0.0f;

	/** 上一次真正记数的撞击发生在什么时候（服务器时间）。只在服务器维护。 */
	float LastImpactTime = -1000.0f;
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

	UPROPERTY(Replicated)
	FDeliveryRiderMotion ReplicatedRiderMotion;
	FDeliveryRiderMotion LocalRiderMotion;
	FVector2D RiderImpact = FVector2D::ZeroVector;
	float RiderSampleSpeed = 0, RiderSampleZ = 0, RiderSampleVerticalSpeed = 0, RiderNetElapsed = 0;
	bool bRiderSampleValid = false;
};
