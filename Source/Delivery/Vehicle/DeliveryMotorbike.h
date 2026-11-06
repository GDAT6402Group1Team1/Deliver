// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Pawn.h"
#include "Vehicle/DeliveryRiderPose.h"
#include "Events/DeliveryEventComponent.h"
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
class UDeliveryVehicleAudioComponent;
struct FInputActionValue;

/** 后备箱盖的动画阶段；装和卸共用同一条时间线，只是快递走的方向相反 */
UENUM(BlueprintType)
enum class EDeliveryTrunkPhase : uint8
{
	Idle,
	Loading,
	Unloading
};

/**
 * 可驾驶的摩托车，按 F 键上下车
 * 车辆不进行刚体模拟，由代码计算速度、转向、贴地和碰撞移动
 * 上车时隐藏玩家身体并显示骑手，下车时恢复玩家身体
 */
UCLASS()

class DELIVERY_API ADeliveryMotorbike : public APawn
{
	GENERATED_BODY()
	friend class FDeliveryRiderContactsTest;
	friend class FDeliveryRiderRuntimeTest;
	friend class FDeliveryVehicleSummonTest;
	friend class FDeliveryMotorbikeTrunkTest;

  public:
	ADeliveryMotorbike();

	virtual void OnConstruction(const FTransform& Transform) override;
	virtual void BeginPlay() override;
	virtual void Tick(float DeltaSeconds) override;

	// 驾驶输入绑定
	virtual void SetupPlayerInputComponent(class UInputComponent* PlayerInputComponent) override;
	virtual void GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const override;

    UInputAction* GetCameraToggleAction() const;
    bool IsFreeLookCamera() const { return bFreeLookCamera; }

	// 上车与下车接口
	// 上下车与召唤
	/** 服务器：让 NewDriver 上车；成功返回 真 */
	UFUNCTION(BlueprintCallable, Category = "Motorbike")
	bool TryEnter(ADeliveryCharacter* NewDriver);

	// 召唤与撞击接口
	/**
	 * 服务器：把车挪到指定位置（召唤）；会自己往下探地面、清掉残余车速和被撞状态
	 * 车上有人时拒绝——不能把别人正骑着的车抽走；找不到地面也拒绝，免得把车塞进虚空
	 */
	UFUNCTION(BlueprintCallable, Category = "Motorbike")
	bool SummonTo(const FVector& DesiredLocation, float DesiredYaw);

	/** 服务器：下车，把驾驶员放回车边的地面 */
	UFUNCTION(BlueprintCallable, Category = "Motorbike")
	void ExitVehicle();

	// 车载快递
	/**
	 * 服务器将快递固定到货架，已有其他快递时拒绝
	 * 固定后关闭快递物理，避免物理快递与代码控制的车辆互相拉扯
	 */
	UFUNCTION(BlueprintCallable, Category = "Motorbike|货物")
	bool AttachCargo(ADeliveryPackageItem* Package);

	/**
	 * 服务器解除快递与车辆的关系并返回快递，无货时返回空指针
	 * 后续放到手中还是地面由调用方决定
	 */
	UFUNCTION(BlueprintCallable, Category = "Motorbike|货物")
	ADeliveryPackageItem* ReleaseCargo();

	/** 取得车上的有效快递，已销毁的快递按无货处理 */
	UFUNCTION(BlueprintPure, Category = "Motorbike|货物")
	ADeliveryPackageItem* GetCargo() const;

	UFUNCTION(BlueprintPure, Category = "Motorbike|货物")

	bool HasCargo() const
	{
		return GetCargo() != nullptr;
	}

	// 后备箱装卸
	/**
	 * 手上拿着快递时能不能装车：车上没货、没人骑、也不在播装卸动画
	 *
	 * 这是"按 F 上车"那道闸的另一半——手上有货就只给装货、不给上车，
	 * 两边读同一组条件，不会出现"提示说能装、按下去没反应"
	 */
	UFUNCTION(BlueprintPure, Category = "Motorbike|后备箱")
	bool CanLoadTrunk(const ADeliveryCharacter* Character) const;

	/** 空手时能不能从后备箱取货 */
	UFUNCTION(BlueprintPure, Category = "Motorbike|后备箱")
	bool CanUnloadTrunk(const ADeliveryCharacter* Character) const;

	/** 正在播开盖/合盖动画；这期间既不能上车也不能再按一次装卸 */
	UFUNCTION(BlueprintPure, Category = "Motorbike|后备箱")
	bool IsTrunkBusy() const
	{
		return TrunkPhase != EDeliveryTrunkPhase::Idle;
	}

	/** 服务器：把角色手上的快递装进后备箱，并开始开盖动画 */
	UFUNCTION(BlueprintCallable, Category = "Motorbike|后备箱")
	bool BeginTrunkLoad(ADeliveryCharacter* Character);

	/** 服务器：开盖把后备箱里的快递交回角色手上 */
	UFUNCTION(BlueprintCallable, Category = "Motorbike|后备箱")
	bool BeginTrunkUnload(ADeliveryCharacter* Character);

	// 撞击和驾驶状态
	/**
	 * 服务器处理交通车撞击，达到指定次数后强制下车
	 * 返回是否接受本次撞击，供来车更新冷却
	 */
	bool NotifyTrafficImpact(const FVector& CarVelocity);

	UFUNCTION(BlueprintPure, Category = "Motorbike")

	ADeliveryCharacter* GetDriver() const
	{
		return Driver;
	}

	UFUNCTION(BlueprintPure, Category = "Motorbike")

	float GetCurrentSpeed() const
	{
		return CurrentSpeed;
	}

	// 骑手动画配置
	/** 主线程供骑手动画读取本帧输入和四个组件空间接触点 */
	void BuildRiderAnimationFrame(FDeliveryRiderFrame& OutFrame) const;

	/** 从模型坐姿自动标定手把/脚踏目标；也可关闭自动标定后手调四个组件 */
	UFUNCTION(BlueprintCallable, CallInEditor, Category = "Motorbike|骑手动画")
	void CalibrateRiderContacts();

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Motorbike|骑手动画")
	FDeliveryRiderSettings RiderAnimationSettings;

	UPROPERTY(EditAnywhere, Category = "Motorbike|骑手动画")
	bool bAutoCalibrateRiderContacts = true;

	UPROPERTY(EditAnywhere, Category = "Motorbike|骑手动画")
	TSubclassOf<UAnimInstance> RiderAnimationClass;

	/** 骑手组件未配置模型时使用的备用资源，运行时按需加载 */
	UPROPERTY(EditAnywhere, Category = "Motorbike|Mesh")
	TSoftObjectPtr<USkeletalMesh> RiderMeshAsset;

	/** 车体部件网格；FBX 里摩托车是 9 个独立静态网格，导入脚本会把它们填到这里 */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Motorbike|Mesh")
	TArray<TObjectPtr<UStaticMesh>> BodyMeshes;

  protected:
	// 车体组件与固定槽位
	/** 车体部件的固定槽位数；用定额的默认子对象而不是运行时 NewObject，避免构造脚本反复重建组件 */
	static constexpr int32 MaxBodyParts = 12;

	/** 轮子转轴的固定槽位数；摩托车两个轮，留 4 个余量给以后的三轮/挂斗 */
	static constexpr int32 MaxWheels = 4;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Components")
	TObjectPtr<UBoxComponent> CollisionBox;

	/**
	 * 只处理绕行驶方向的转弯侧倾
	 * 与模型朝向校正分层，避免侧倾轴被模型旋转改变
	 */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Components")
	TObjectPtr<USceneComponent> MeshRoot;

	/**
	 * 整车视觉缩放层，夹在侧倾层和模型校正层之间
	 *
	 * **为什么要单独一层**：MeshAlign 的位置和朝向是 setup_motorbike.py 量出来写进
	 * 蓝图的，缩放写在它身上会和那份配置混在一起；而 MeshRoot 每帧被写 Roll（压弯）。
	 * 这一层只由 C++ 管，蓝图里没有它的覆盖值，所以改 VisualScale 一定生效。
	 */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Components")
	TObjectPtr<USceneComponent> MeshScale;

	/**
	 * 校正模型位置和朝向，使车头朝前、车轮位于地面
	 * 参数可由模型配置工具计算
	 */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Components")
	TObjectPtr<USceneComponent> MeshAlign;

	/**
	 * 固定车上快递的货架挂点，随模型朝向和侧倾一起运动
	 * 位置通过货架偏移配置
	 */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Components")
	TObjectPtr<USceneComponent> CargoPivot;

	/**
	 * 快递在后备箱**里面**的位置，相对模型校正层（该层局部正纵轴朝车头）
	 * 零向量表示按箱体模型自动量，非零值使用手动配置
	 *
	 * 这替掉了原来那个"货架"偏移（`CargoOffset`，量的是车尾顶面 + 12cm，
	 * 快递就摆在箱盖上）。现在快递是装进箱子里的，装好之后整件隐藏——
	 * 它在一个关着的箱子里，看不见才是对的，而且快递比箱子内腔大时
	 * 不隐藏就会穿出箱盖。
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Motorbike|货物")
	FVector TrunkInsideOffset = FVector::ZeroVector;

	/** 快递在箱子里的朝向，相对 MeshAlign */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Motorbike|货物")
	FRotator CargoRotation = FRotator::ZeroRotator;

	// 整车尺寸
	/**
	 * 整车视觉缩放，1 表示按原始导入尺寸
	 *
	 * **默认 0.73 = 140 / 190**：导入时 `IMPORT_SCALE = 1.9` 是按"主角身高 192cm"
	 * 算的（那个数来自胶囊半高 96，是错的），而主角实测只有 **140cm**
	 * （胶囊半高 70、renwu 网格高 139.9）。所以这辆车和车上那个骑手是照一个
	 * 190cm 的人做的，整车 256cm 长、168cm 高，站在 140cm 的主角旁边明显过大，
	 * 而且一上车视野里那个骑手比刚才操控的人高出一头——"坐上去尺寸变大"就是这个。
	 *
	 * 这里不重导资产：重导要人跑脚本，还会冲掉蓝图上手调过的覆盖值；
	 * 运行时缩一层在打包版里一样有效（和 `AutoConfigureFromMeshes()` 同一个理由）。
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Motorbike|尺寸",
	          meta = (ClampMin = "0.1", ClampMax = "3.0"))
	float VisualScale = 0.73f;

	/**
	 * 碰撞盒半尺寸（缩放前）；实际用的是它乘 VisualScale
	 *
	 * 缩了视觉不缩碰撞的话，车会在离墙十几厘米的地方就"撞上"，而画面里还差一截。
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Motorbike|尺寸")
	FVector BaseCollisionExtent = FVector(130.0f, 68.0f, 70.0f);

	// 后备箱
	/**
	 * 后备箱盖的旋转挂点，铰链在箱盖后沿，开盖时前沿抬起
	 */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Components")
	TObjectPtr<USceneComponent> TrunkLidPivot;

	/** 后备箱的交互测量点和提示锚点，大致在箱体中心 */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Components")
	TObjectPtr<USceneComponent> TrunkAnchor;

	/** 后备箱的左键交互（装货/取货）；和车身那个按 F 的交互是两个独立组件 */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Components")
	TObjectPtr<UDeliveryInteractableComponent> TrunkInteractable;

	/** 箱盖所在的模型槽位；-1 表示按几何自动认（车尾最高的那块扁平件） */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Motorbike|后备箱")
	int32 TrunkLidPartIndex = INDEX_NONE;

	/** 箱体所在的模型槽位；-1 表示自动认（紧贴箱盖下方、和它平面重叠的那块） */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Motorbike|后备箱")
	int32 TrunkBoxPartIndex = INDEX_NONE;

	/** 箱盖铰链位置，MeshAlign 局部空间；零向量表示自动量 */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Motorbike|后备箱")
	FVector TrunkLidHinge = FVector::ZeroVector;

	/**
	 * 箱盖的铰链在前沿（靠骑手那一侧），盖子**向后翻开**
	 *
	 * 这一个开关同时决定"铰链落在箱盖哪条边"和"往哪边转"，所以不可能配出
	 * "铰链在后沿、却往后转"这种把盖子转进车身的组合——那种错只能靠眼睛发现。
	 * 关掉就变成铰链在后沿、向前翻开。
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Motorbike|后备箱")
	bool bTrunkLidHingeAtFront = true;

	/** 开盖角度（正数就是"开多少度"，往哪边开由上面那个开关决定） */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Motorbike|后备箱",
	          meta = (ClampMin = "0.0", ClampMax = "170.0", Units = "deg"))
	float TrunkLidOpenAngle = 85.0f;

	/** 快递在箱口上方的位置（MeshAlign 局部）；零向量表示自动量 */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Motorbike|后备箱")
	FVector TrunkOpeningOffset = FVector::ZeroVector;

	/** 开盖/合盖各用多久 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Motorbike|后备箱",
	          meta = (ClampMin = "0.05", Units = "s"))
	float TrunkLidDuration = 0.35f;

	/** 快递进出箱子用多久 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Motorbike|后备箱",
	          meta = (ClampMin = "0.05", Units = "s"))
	float TrunkMoveDuration = 0.45f;

	/**
	 * 站多近才显示"左键装货/取货"
	 *
	 * 默认和车身那个 F 交互一样是 300：手上有货时 F 提示被压掉，如果这个半径更小，
	 * 站在车头就会**什么提示都不出现**，玩家只知道"按 F 没反应"。
	 * 提示浮窗吊在后备箱上方，所以"该走到车尾"是靠提示的位置告诉玩家的。
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Motorbike|后备箱",
	          meta = (ClampMin = "0.0", Units = "cm"))
	float TrunkInteractRadius = 300.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Motorbike|后备箱")
	FText TrunkLoadPromptText;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Motorbike|后备箱")
	FText TrunkUnloadPromptText;

	/** 前轮、前叉和车把的转向层，随车身校正与侧倾一起运动 */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Components")
	TObjectPtr<USceneComponent> SteerPivot;

	/** 车把使用独立转向层，只转动前轮转角的一部分 */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Components")
	TObjectPtr<USceneComponent> BarPivot;

	/** 骑手模型的独立挂点，身体转动由骑手动画控制 */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Components")
	TObjectPtr<USceneComponent> RiderPivot;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Components")
	TArray<TObjectPtr<UStaticMeshComponent>> BodyParts;

	/**
	 * 轮子旋转的挂点，放在轮心并反向补偿模型偏移
	 * 前轮随转向层转动，后轮随车身模型转动
	 */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Components")
	TArray<TObjectPtr<USceneComponent>> WheelPivots;

	/** 打满角度跟转的槽位（前轮、前叉）；由 setup_motorbike.py 按几何认出来填，别手填 */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Motorbike|Mesh")
	TArray<int32> SteeringPartIndices;

	/** 只跟转一部分的槽位（车把） */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Motorbike|Mesh")
	TArray<int32> HandlebarPartIndices;

	/** 轮子对应的模型槽位，可按模型几何特征自动识别 */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Motorbike|Mesh")
	TArray<int32> WheelPartIndices;

	/** 每个轮子的轮心，和 WheelPartIndices 一一对应，MeshAlign 局部空间 */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Motorbike|Mesh")
	TArray<FVector> WheelCenters;

	/** 轮子半径（导入空间 cm）；决定同样车速下轮子转多快，脚本按包围盒量 */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Motorbike|Mesh",
	          meta = (ClampMin = "1.0", Units = "cm"))
	float WheelRadius = 30.0f;

	/** 转向轴位置，MeshAlign 局部空间；同样由脚本量出来填 */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Motorbike|Mesh")
	FVector SteerPivotLocation = FVector::ZeroVector;

	/** 骑手髋部的参考位置，用于设置模型挂点 */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Motorbike|Mesh")
	FVector RiderPivotLocation = FVector::ZeroVector;

	// 骑手模型与四肢接触点
	/**
	 * 独立骑手骨骼网格：AnimBP 的原生代理从参考坐姿生成身体惯性，再求手脚 IK
	 * 不参与车体碰撞，也不直接修改原角色布娃娃；材质仍按驾驶者映射
	 */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Components")
	TObjectPtr<USkeletalMeshComponent> RiderMesh;

	/** 双手目标跟随 BarPivot，双脚目标跟随 MeshAlign；身体骨骼晃动不移动这些点 */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Components")
	TObjectPtr<USceneComponent> LeftHandGrip;
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Components")
	TObjectPtr<USceneComponent> RightHandGrip;
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Components")
	TObjectPtr<USceneComponent> LeftFootPeg;
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Components")
	TObjectPtr<USceneComponent> RightFootPeg;

	/**
	 * 驾驶时按材质槽名称，将驾驶员材质应用到骑手模型
	 * 两者需要使用匹配的材质槽和贴图坐标
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Motorbike|表现")
	bool bUseDriverMaterials = true;

	/**
	 * 手动指定骑手每个槽位的材质，按槽位下标对应；填了的槽位优先于上面的自动映射，
	 * 留空（None）的槽位仍然走自动映射；美术想单独给骑手配一套时用这个，不用改代码
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Motorbike|表现")
	TArray<TObjectPtr<UMaterialInterface>> RiderMaterialOverrides;

	// 镜头和交互组件
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Components")
	TObjectPtr<USpringArmComponent> CameraBoom;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Components")
	TObjectPtr<UCameraComponent> FollowCamera;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Components")
	TObjectPtr<UDeliveryInteractableComponent> Interactable;

	// 车辆音频
	/** 行驶循环音；声音资产本身需要设置为循环，C++ 负责启停和随车速调音 */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Components")
	TObjectPtr<UAudioComponent> EngineAudio;

    /** 当前车辆的本地事件，各客户端分别接收网络通知 */
    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category="Components")
    TObjectPtr<UDeliveryEventComponent> Events;

    /** 事件音效和发动机播放，配置在此组件中设置 */
    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category="Components")
    TObjectPtr<UDeliveryVehicleAudioComponent> VehicleAudio;

	// ---- 行驶参数 ----

	UPROPERTY(EditAnywhere, Category = "Motorbike|行驶", meta = (ClampMin = "0.0", Units = "cm/s"))
	float MaxSpeed = 1500.0f;

	UPROPERTY(EditAnywhere, Category = "Motorbike|行驶", meta = (ClampMin = "0.0", Units = "cm/s"))
	float MaxReverseSpeed = 400.0f;

	UPROPERTY(EditAnywhere, Category = "Motorbike|行驶", meta = (ClampMin = "0.0"))
	float ThrottleAcceleration = 950.0f;

	/** 往回推油门时的刹车力；比加速大得多，按一下就能明显减速 */
	UPROPERTY(EditAnywhere, Category = "Motorbike|行驶", meta = (ClampMin = "0.0"))
	float BrakeDeceleration = 2200.0f;

	/** 松油门时的自然减速 */
	UPROPERTY(EditAnywhere, Category = "Motorbike|行驶", meta = (ClampMin = "0.0"))
	float CoastDeceleration = 400.0f;

	UPROPERTY(EditAnywhere, Category = "Motorbike|行驶", meta = (ClampMin = "0.0", Units = "deg/s"))
	float MaxTurnRate = 115.0f;

	/**
	 * 转向随车速淡入的参考速度：车速到这个值时转向到满
	 * 摩托车停着不能原地转圈，所以转向量乘的是 Speed/TurnSpeedReference（上限 1）
	 */
	UPROPERTY(EditAnywhere, Category = "Motorbike|行驶", meta = (ClampMin = "1.0", Units = "cm/s"))
	float TurnSpeedReference = 450.0f;

	// ---- 贴地 ----

	/** 车身根点离地高度；要大于碰撞盒半高，否则盒子会插进地面 */
	UPROPERTY(EditAnywhere, Category = "Motorbike|贴地", meta = (ClampMin = "0.0", Units = "cm"))
	float HoverHeight = 55.0f;

	UPROPERTY(EditAnywhere, Category = "Motorbike|贴地", meta = (ClampMin = "0.0", Units = "cm"))
	float GroundTraceUp = 150.0f;

	UPROPERTY(EditAnywhere, Category = "Motorbike|贴地", meta = (ClampMin = "0.0", Units = "cm"))
	float GroundTraceDown = 500.0f;

	/** 贴地高度的追随速度；太大在碎石路上会抖，太小上坡会陷进去 */
	UPROPERTY(EditAnywhere, Category = "Motorbike|贴地", meta = (ClampMin = "0.1"))
	float GroundSnapSpeed = 12.0f;

	UPROPERTY(EditAnywhere, Category = "Motorbike|贴地")
	float GravityZ = -2200.0f;

	/** 车身俯仰跟随坡度的插值速度 */
	UPROPERTY(EditAnywhere, Category = "Motorbike|贴地", meta = (ClampMin = "0.1"))
	float GroundAlignSpeed = 7.0f;

	UPROPERTY(EditAnywhere, Category = "Motorbike|贴地", meta = (ClampMin = "0.0", ClampMax = "80.0"))
	float MaxGroundAlignAngle = 45.0f;

	// ---- 爬坡越障 ----

	/**
	 * 允许跨越的台阶高度，单位为厘米
	 * 先抬高、前移，再落回地面，任一步失败都会撤回
	 * 数值过大会让车辆越过本应阻挡的矮墙
	 */
	UPROPERTY(EditAnywhere, Category = "Motorbike|爬坡越障", meta = (ClampMin = "0.0", Units = "cm"))
	float MaxStepHeight = 60.0f;

	/**
	 * 最陡能爬的坡度；比这更陡的面当墙，跨上去也不算站住
	 * 和 MaxGroundAlignAngle 保持一致：能爬上去的坡，车身姿态就该跟着贴上去
	 */
	UPROPERTY(EditAnywhere, Category = "Motorbike|爬坡越障", meta = (ClampMin = "0.0", ClampMax = "80.0"))
	float MaxClimbAngle = 45.0f;

	/**
	 * 向上贴合地面的最低速度，单位为厘米每秒
	 * 避免高速上坡时车身跟不上地面高度，导致前移检测撞到坡面
	 */
	UPROPERTY(EditAnywhere, Category = "Motorbike|爬坡越障", meta = (ClampMin = "0.0", Units = "cm/s"))
	float MaxClimbRate = 600.0f;

	/**
	 * 贴地射线往前看多远；上坡/上台阶时提前抬车，免得等撞上了才反应
	 * 实际前瞻距离取"这一帧位移的两倍"和这个值里的小者，所以停着不会凭空浮起来
	 */
	UPROPERTY(EditAnywhere, Category = "Motorbike|爬坡越障", meta = (ClampMin = "0.0", Units = "cm"))
	float GroundLookAhead = 120.0f;

	/** 真撞墙（跨不上去）时车速剩下多少；原来写死 0.3 */
	UPROPERTY(EditAnywhere, Category = "Motorbike|爬坡越障", meta = (ClampMin = "0.0", ClampMax = "1.0"))
	float WallHitSpeedScale = 0.5f;

	/**
	 * 跨台阶时必须完成的剩余位移比例
	 * 越小越容易通过轻微阻挡，越大越严格；零表示只要能够前移即可
	 */
	UPROPERTY(EditAnywhere, Category = "Motorbike|爬坡越障", meta = (ClampMin = "0.0", ClampMax = "1.0"))
	float StepUpMinProgress = 0.03f;

	/** 离地多高还算"贴着地"，超过就交给重力；要大于 MaxStepHeight，否则刚跨上台阶就被判成起飞 */
	UPROPERTY(EditAnywhere, Category = "Motorbike|贴地", meta = (ClampMin = "0.0", Units = "cm"))
	float GroundStickTolerance = 80.0f;

	// ---- 表现 ----

	/** 转弯时车身视觉上的最大侧倾角；只动 MeshRoot，不影响碰撞和行驶 */
	UPROPERTY(EditAnywhere, Category = "Motorbike|表现", meta = (ClampMin = "0.0", ClampMax = "60.0"))
	float MaxLeanAngle = 26.0f;

	UPROPERTY(EditAnywhere, Category = "Motorbike|表现", meta = (ClampMin = "0.1"))
	float LeanSpeed = 5.0f;

	/** 前轮的最大转向角，车把按单独的比例跟转 */
	UPROPERTY(EditAnywhere, Category = "Motorbike|表现", meta = (ClampMin = "0.0", ClampMax = "60.0"))
	float MaxVisualSteerAngle = 22.0f;

	UPROPERTY(EditAnywhere, Category = "Motorbike|表现", meta = (ClampMin = "0.1"))
	float SteerVisualSpeed = 9.0f;

	/**
	 * 车把转前轮的百分之多少；手部目标跟随实际车把，不再依赖整人转角近似贴合
	 * 增大后需验证骑手骨架能否够到，不能用无限拉长手臂来补偿不合理的目标
	 */
	UPROPERTY(EditAnywhere, Category = "Motorbike|表现", meta = (ClampMin = "0.0", ClampMax = "1.0"))
	float HandlebarSteerRatio = 0.5f;

	/**
	 * 是否允许鼠标转动骑车镜头
	 * 关闭时镜头固定跟随车尾，开启时可绕车自由观察
	 */
	UPROPERTY(EditAnywhere, Category = "Motorbike|表现")
	bool bFreeLookCamera = true;

	/**
	 * 自由视角下按镜头方向解释移动输入，再换算成车辆油门和转向
	 * 关闭时按车头方向驾驶，固定视角不受此设置影响
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Motorbike|镜头")
	bool bCameraRelativeMoveInFreeLook = true;

	/**
	 * 车头和"想去的方向"差多少度算满舵（度）
	 * 小了会在小角度就打满、走蛇形；大了转向迟钝、过弯要压很久
	 */
	UPROPERTY(EditAnywhere, Category = "Motorbike|镜头", meta = (ClampMin = "5.0", ClampMax = "180.0"))
	float CameraRelativeFullSteerAngle = 50.0f;

	/**
	 * 大角度转向时保留的最低油门，范围为零到一
	 * 应大于零，让车辆保持移动，否则车辆停下后无法继续转向
	 */
	UPROPERTY(EditAnywhere, Category = "Motorbike|镜头", meta = (ClampMin = "0.05", ClampMax = "1.0"))
	float CameraRelativeMinThrottle = 0.4f;

	/**
	 * 方向差超过此角度时先减速再掉头，单位为度
	 * 只影响方向换算，不影响直接倒车操作
	 */
	UPROPERTY(EditAnywhere, Category = "Motorbike|镜头", meta = (ClampMin = "90.0", ClampMax = "180.0"))
	float CameraRelativeBrakeAngle = 120.0f;

	/**
	 * 大角度掉头先转向侧面，再朝目标方向修正，单位为度
	 * 开始时锁定转向侧和是否先刹车，避免接近正后方或速度临界点时反复切换
	 */
	UPROPERTY(EditAnywhere, Category = "Motorbike|镜头", meta = (ClampMin = "30.0", ClampMax = "135.0"))
	float CameraRelativeStageSideAngle = 90.0f;

	/**
	 * 掉头前减速到此速度后开始转向，单位为厘米每秒
	 * 应明显低于开始刹车的速度，避免频繁切换刹车和转向
	 */
	UPROPERTY(EditAnywhere, Category = "Motorbike|镜头", meta = (ClampMin = "0.0"))
	float CameraRelativeBrakeExitSpeed = 25.0f;

	/**
	 * 车辆接受两次撞击的最短间隔，单位为秒
	 * 统一限制不同来车的连续撞击，避免一次接触就累计到强制下车次数
	 */
	UPROPERTY(EditAnywhere, Category = "Motorbike|撞击", meta = (ClampMin = "0.0"))
	float MinImpactInterval = 1.0f;

	/** 是否按车速显示轮子自转，可在车辆蓝图中开启 */
	UPROPERTY(EditAnywhere, Category = "Motorbike|表现")
	bool bSpinWheels = false;

	/**
	 * 轮子自转方向，正负号决定旋转朝向
	 * 模型前进时轮子反转，可调整该符号
	 */
	UPROPERTY(EditAnywhere, Category = "Motorbike|表现")
	float WheelSpinSign = -1.0f;

	/** 骑车时镜头的俯角；固定视角下是写死的俯角；自由视角下只当上车那一刻的起始俯角 */
	UPROPERTY(EditAnywhere, Category = "Motorbike|表现", meta = (ClampMin = "-80.0", ClampMax = "20.0"))
	float CameraPitch = -12.0f;

	// ---- 上下车 ----

	/** 下车时驾驶员放在车身右侧多远（负值就是左侧） */
	UPROPERTY(EditAnywhere, Category = "Motorbike|上下车", meta = (Units = "cm"))
	float ExitSideOffset = -140.0f;

	/**
	 * 骑行中被交通车撞几次会被掀下车；默认 2：第一下只当擦碰，第二下才下车
	 * 每次上车重新从 0 开始算，中途主动下车也清零
	 */
	UPROPERTY(EditAnywhere, Category = "Motorbike|上下车", meta = (ClampMin = "1"))
	int32 ImpactsToDismount = 2;

	// 撞路人
	/**
	 * 骑车撞上路人时给的冲量基准
	 *
	 * **单独给一个数，不和交通车或拳头共用**：路人的刚体质量和玩家布娃娃不是一个
	 * 量级，共用一个值要么推不动、要么一拳飞上天（交通车那边也是单独给的）。
	 * 给 0 就是关掉"骑车能撞人"。
	 */
	UPROPERTY(EditAnywhere, Category = "Motorbike|撞路人", meta = (ClampMin = "0.0"))
	float PedestrianImpactImpulse = 48000.0f;

	/** 撞飞路人时往上抬的比例；纯水平的话人会贴着地面滑出去，不像被撞飞 */
	UPROPERTY(EditAnywhere, Category = "Motorbike|撞路人",
	          meta = (ClampMin = "0.0", ClampMax = "1.0"))
	float PedestrianImpactUpwardFraction = 0.35f;

	/**
	 * 力度按车速缩放到满的参考速度（cm/s）
	 *
	 * 和交通车不一样，这里**可以**按车速缩放：摩托车的 CurrentSpeed 是自己算的真实值，
	 * 不是交通车那个"每帧位移 ÷ 帧时间"（那个在车被瞬移回出生点的那一帧会算出
	 * 假的 3053 cm/s）。但按 R 召唤会瞬移这辆车，所以仍然要挡住瞬移那一帧，
	 * 见 ProcessPedestrianImpacts 里的位移上限检查。
	 */
	UPROPERTY(EditAnywhere, Category = "Motorbike|撞路人", meta = (ClampMin = "1.0"))
	float PedestrianImpactSpeedReference = 700.0f;

	/** 慢速蹭到人时的最低力度比例；给 0 的话慢慢顶过去人是原地瘫软，看着像自己躺下 */
	UPROPERTY(EditAnywhere, Category = "Motorbike|撞路人",
	          meta = (ClampMin = "0.0", ClampMax = "1.0"))
	float PedestrianImpactMinScale = 0.3f;

	/** 低于这个车速（cm/s）不撞人，免得停着蹭一下也把人撞飞 */
	UPROPERTY(EditAnywhere, Category = "Motorbike|撞路人", meta = (ClampMin = "0.0"))
	float PedestrianImpactMinSpeed = 120.0f;

	// ---- 被撞反应 ----

	/** 把来车速度的多少比例变成自己的击退速度 */
	UPROPERTY(EditAnywhere, Category = "Motorbike|被撞", meta = (ClampMin = "0.0", ClampMax = "2.0"))
	float KnockbackFraction = 0.6f;

	UPROPERTY(EditAnywhere, Category = "Motorbike|被撞", meta = (ClampMin = "0.0", Units = "cm/s"))
	float MaxKnockbackSpeed = 700.0f;

	/** 被撞歪：车头被撞偏的角速度，按来车方向相对车身的左右分量定正负 */
	UPROPERTY(EditAnywhere, Category = "Motorbike|被撞", meta = (ClampMin = "0.0", Units = "deg/s"))
	float KnockYawPerHit = 140.0f;

	/** 被撞歪：车身（连带骑手）视觉上倾斜多少度；只改 MeshRoot，不影响碰撞 */
	UPROPERTY(EditAnywhere, Category = "Motorbike|被撞", meta = (ClampMin = "0.0", ClampMax = "60.0"))
	float KnockTiltPerHit = 32.0f;

	/** 击退/撞歪/抖动统一的指数衰减速率；越大恢复越快 */
	UPROPERTY(EditAnywhere, Category = "Motorbike|被撞", meta = (ClampMin = "0.1"))
	float KnockDecay = 2.6f;

	/** 镜头震动的最大角度 */
	UPROPERTY(EditAnywhere, Category = "Motorbike|被撞", meta = (ClampMin = "0.0", ClampMax = "15.0"))
	float ShakeAngle = 5.0f;

	UPROPERTY(EditAnywhere, Category = "Motorbike|被撞", meta = (ClampMin = "0.1"))
	float ShakeFrequency = 30.0f;

	/**
	 * 撞击下车时是否将驾驶员生命值清零
	 * 清零后由技能组件执行现有的晕倒和恢复流程
	 */
	UPROPERTY(EditAnywhere, Category = "Motorbike|被撞")
	bool bKnockDownDriverOnDismount = true;

	/** 被撞下车时把人抛出去的水平速度；调大人飞得更远、更不容易和车纠缠 */
	UPROPERTY(EditAnywhere, Category = "Motorbike|被撞", meta = (ClampMin = "0.0", Units = "cm/s"))
	float KnockDownLaunchSpeed = 1400.0f;

	UPROPERTY(EditAnywhere, Category = "Motorbike|被撞", meta = (ClampMin = "0.0", Units = "cm/s"))
	float KnockDownLaunchUp = 550.0f;

	/** 按来车速度缩放抛射力度，并保留最低力度，使轻微撞击也有反馈 */
	UPROPERTY(EditAnywhere, Category = "Motorbike|被撞", meta = (ClampMin = "1.0", Units = "cm/s"))
	float KnockDownSpeedReference = 1200.0f;

	UPROPERTY(EditAnywhere, Category = "Motorbike|被撞", meta = (ClampMin = "0.0", ClampMax = "1.0"))
	float KnockDownLaunchMinScale = 0.35f;

	UPROPERTY(EditAnywhere, Category = "Motorbike|上下车")
	FText DrivePromptText;

	UPROPERTY(EditAnywhere, Category = "Motorbike|上下车")
	FText ExitPromptText;

	/** 上车后显示下车提示的时长，单位为秒 */
	UPROPERTY(EditAnywhere, Category = "Motorbike|上下车", meta = (ClampMin = "0.0", Units = "s"))
	float ExitPromptDuration = 2.0f;

	UPROPERTY(EditAnywhere, Category = "Input")
	TObjectPtr<UInputAction> MoveAction;

	// 视角与交互输入
    /** 切换视角动作，键盘和手柄按键在 IMC_Default 中配置 */
    UPROPERTY(EditDefaultsOnly, Category="Input")
    TSoftObjectPtr<UInputAction> CameraToggleAction;

	/** 手柄右摇杆看向；只有 bFreeLookCamera 开着才起作用 */
	UPROPERTY(EditAnywhere, Category = "Input")
	TObjectPtr<UInputAction> LookAction;

	/** 鼠标看向；和角色身上用的是同两个 IA，手感一致 */
	UPROPERTY(EditAnywhere, Category = "Input")
	TObjectPtr<UInputAction> MouseLookAction;

	/**
	 * 鼠标自由视角灵敏度；固定视角和手柄右摇杆不受影响
	 *
	 * 和步行那套保持同一个数（见 UDeliveryInputComponent::MouseLookSensitivity），
	 * 0.4 = 原始输入的 40%。两边不一致的表现是上下车时手感突变。
	 */
	UPROPERTY(EditAnywhere, Category = "Input", meta = (ClampMin = "0.1", ClampMax = "2.0"))
	float MouseLookSensitivity = 0.4f;

	/** 通用交互输入，默认 F 键，在绑定输入时按需加载资源 */
	UPROPERTY(EditAnywhere, Category = "Input")
	TSoftObjectPtr<UInputAction> InteractAction;

	/** 骑车时将 E 键操作转发给驾驶员，复用角色的探测、长按和服务器检查 */
	UPROPERTY(EditAnywhere, Category = "Input")
	TSoftObjectPtr<UInputAction> PickupAction;

  private:
	void MoveInput(const FInputActionValue& Value);
	void LookInput(const FInputActionValue& Value);
	void MouseLookInput(const FInputActionValue& Value);

	/** 局内切一次视角模式，并在屏幕上提示现在是哪种 */
	void ToggleCameraMode();

	/** 按 bFreeLookCamera 把弹簧臂配成自由视角或固定车尾视角；上车时和 BeginPlay 各调一次 */
	void ApplyCameraMode();
	void InteractPressed(const FInputActionValue& Value);
	void PickupPressed(const FInputActionValue& Value);
	void PickupReleased(const FInputActionValue& Value);

	// 服务器驾驶输入
	// 网络请求与同步
	UFUNCTION(Server, Unreliable)
	void ServerSetDriveInput(float InThrottle, float InSteer);

	UFUNCTION(Server, Reliable)
	void ServerRequestExit();

	/** 服务器确认上下车、碰撞或召唤后，在各端发布车辆事件 */
	UFUNCTION(NetMulticast, Unreliable)
	void MulticastVehicleEvent(EDeliveryEvent Event);

	UFUNCTION()
	void HandleInteractRequested(APawn* Interactor);

	UFUNCTION()
	void OnRep_Driver();

	UFUNCTION()
	void OnRep_Cargo();

	/** 把 Cargo 焊到货架上（或在它变空时什么都不做）；两端都要跑 */
	void ApplyCargoPresentation();

	// 后备箱装卸
	/** 两端各自跑一遍同一条时间线；服务器只负责开头的校验和结尾的交接 */
	UFUNCTION(NetMulticast, Reliable)
	void MulticastTrunkAction(EDeliveryTrunkPhase Phase);

	/** 推进箱盖角度和快递位置；装卸结束时在服务器上做交接 */
	void UpdateTrunkAnimation(float DeltaSeconds);

	/** 把箱盖角度写进挂点；Alpha 0 = 关好，1 = 开到 TrunkLidOpenAngle */
	void ApplyTrunkLidAlpha(float Alpha);

	/**
	 * 箱盖抬起来的那条边朝哪（MeshAlign 局部方向，用于算旋转方向和写测试断言）
	 *
	 * 铰链在前沿时抬的是后沿（−Y），反之抬前沿（+Y）；局部 +Y 朝车头。
	 */
	FVector GetTrunkLidLiftDirection() const;

	/** 车身那个按 F 的交互要问的条件：有人骑、手上有货、正在播动画都不给上车 */
	bool CanDriveNow(const ADeliveryCharacter* Character) const;

	/** 左键交互：按当前状态决定这一下是装货还是取货 */
	bool CanUseTrunk(APawn* Interactor, bool bCheckTaskAvailability);
	bool UseTrunk(APawn* Interactor);

	/** 按当前状态刷新"左键装货/取货"的文字和开关 */
	void ApplyTrunkPresentation();

	// 模型与骑手显示
	/** 把 BodyMeshes 里的网格铺到固定槽位上，多余的槽位清空 */
	void ApplyBodyMeshes();

	/**
	 * 把 VisualScale 写进缩放层和碰撞盒，并把视觉整体往下挪，保持轮子还踩在地上
	 *
	 * 缩放是绕 MeshRoot 原点做的，而车辆原点在车身中间、离地 HoverHeight；
	 * 只缩不挪的话车底会跟着抬起来，表现是"车悬空了一截"。
	 */
	void ApplyVisualScale();

	/**
	 * 根据模型包围盒补全轮心、车把和转向轴等几何参数
	 * 只填写未配置的参数，保留已有手动设置
	 */
	void AutoConfigureFromMeshes();

	/** 按有没有驾驶员刷新骑手网格和可交互状态；服务器和客户端都会走到 */
	void ApplyDriverPresentation();

	/** 把驾驶员身上的材质映射到骑手网格的槽位上；没人骑就还原成资产自带的 */
	void ApplyRiderMaterials();

	// 速度与转向计算
	// 驾驶计算
	void UpdateSpeed(float DeltaSeconds);
	void UpdateSteering(float DeltaSeconds);

	/** 把 MoveInputRaw 按镜头朝向换算成 ThrottleInput / SteerInput；每帧调一次 */
	void ApplyCameraRelativeInput();

	/** 现在是不是按镜头方向开（开关打开 且 处于自由视角） */
	bool UsesCameraRelativeMove() const;

	// 地面检测与移动
	void UpdateGroundAndMove(float DeltaSeconds);

	/**
	 * 水平推进被挡住时试着跨上去；成功返回 真（车已经在台阶上了），
	 * 失败会把车放回调用前的位置，调用方按撞墙处理
	 */
	bool TryStepUp(const FVector& RemainingDelta, const FHitResult& BlockingHit);
	// 车身与骑手表现
	void UpdateLean(float DeltaSeconds);
	void UpdateSteerVisual(float DeltaSeconds);

	// 车辆外观更新
	/** 轮子按车速自转 */
	void UpdateWheelSpin(float DeltaSeconds);

	/** 根据车速、转向和撞击更新骑手姿态，并同步给其他玩家 */
	void UpdateRiderMotion(float DeltaSeconds);
	void ResetRiderMotion();
	void UpdateImpactReaction(float DeltaSeconds);
	// 操作提示
	/** 把 NoticeText 推给浮窗子系统（约定是每帧推一次，停推 0.25 秒自动消失） */
	void PushNotice();

	/** 让某句提示在屏幕上停留 Seconds 秒；上车提示和切视角提示共用这一套 */
	void ShowNotice(const FText& Text, float Seconds);

	/** 骑行期间常驻左下角的操作提示（切视角那条） */

	/** 把人打倒：血清零交给既有的晕倒流程，再补一记冲量把他抛离车身 */
	void KnockDownDriver(ADeliveryCharacter* Rider, const FVector& LaunchDirection, float LaunchScale);

	/**
	 * 指定本次下车侧，正一为右侧、负一为左侧、零使用默认侧
	 * 撞击下车时选择与车辆分离的一侧，避免车辆再次压住角色
	 */
	float PendingExitSideSign = 0.0f;

	/** 固定视角下将控制旋转对齐车头，使下车后的角色镜头保持连续 */
	void SyncControlRotation();

	/** 找一个能把驾驶员放下去的位置；找不到空位就放在车顶上方，总比卡进墙里好 */
	FVector FindExitLocation(float PreferredSideSign = 0.0f) const;

	// 同步驾驶员与快递
	UPROPERTY(ReplicatedUsing = OnRep_Driver)
	TObjectPtr<ADeliveryCharacter> Driver;

	/**
	 * 当前固定在车上的快递，同步给所有玩家
	 * 同步后在各客户端更新快递显示和附着状态
	 */
	UPROPERTY(ReplicatedUsing = OnRep_Cargo)
	TObjectPtr<ADeliveryPackageItem> Cargo;

	// 后备箱运行时状态
	/** 当前动画阶段；只在本机推进，开始由多播同步，所以不需要复制 */
	EDeliveryTrunkPhase TrunkPhase = EDeliveryTrunkPhase::Idle;

	/** 本阶段已经播了多久 */
	float TrunkElapsed = 0.0f;

	/** 装货动画结束时才真正隐藏快递；取货动画结束时才交回手上 */
	TWeakObjectPtr<ADeliveryCharacter> TrunkPendingReceiver;

	// 本次驾驶输入
	float ThrottleInput = 0.0f;
	float SteerInput = 0.0f;

	/**
	 * 保存原始二维移动输入
	 * 镜头和车头方向不断变化，因此每帧重新计算油门和转向
	 */
	FVector2D MoveInputRaw = FVector2D::ZeroVector;

	/**
	 * 本次掉头锁定的方向，正一向右、负一向左、零表示未锁定
	 * 转过侧面后解除，松键、上下车和召唤时也要清除
	 */
	float TurnAroundSign = 0.0f;

	/**
	 * 本次掉头是否还在减速阶段
	 * 开始时确定是否刹车，降到退出速度后才转向，避免在临界速度反复切换
	 */
	bool bBrakingForTurn = false;

	/** 上次发给服务器的油门/转向，用来避免每帧重复发同样的值 */
	float LastSentThrottle = 0.0f;
	float LastSentSteer = 0.0f;

	/** 上一次真正记数的撞击发生在什么时候（服务器时间）；只在服务器维护 */
	float LastImpactTime = -1000.0f;
	// 行驶与车身显示状态
	float CurrentSpeed = 0.0f;
	float VerticalVelocity = 0.0f;
	float CurrentLean = 0.0f;
	float CurrentVisualSteer = 0.0f;

	/** 轮子累计转角（度）；一直累加、不取模——FRotator 自己会归一化 */
	float WheelSpinAngle = 0.0f;

	/** 上一帧位置；远端客户端上 CurrentSpeed 恒为 0，只能靠位移反推车速来转轮子 */
	FVector LastWheelLocation = FVector::ZeroVector;
	bool bWheelLocationValid = false;
	bool bWasGrounded = true;

	// 撞击状态
	/** 本次骑行已经被交通车撞了几下；只在服务器上维护，上/下车时归零 */
	int32 TrafficImpactCount = 0;

	/**
	 * 服务器：沿这一帧走过的路径扫一次路人，撞到就把人打飞
	 *
	 * **单开一次扫描，不并进已有的那两条。** 水平推进那条 sweep 用的是车自己的
	 * 碰撞响应（车是 Pawn 配置），而路人刻意不挡 Pawn——挡了的表现是路人变成一堵墙
	 * 把车别停。贴地那条是竖直射线、只查 WorldStatic/WorldDynamic。
	 * 往这两条里加通道都会改变它们已经调通的命中集合（摩托车受击当初就是因为
	 * 同一个理由才单开一次 ECC_Pawn 查询）。
	 */
	void ProcessPedestrianImpacts(float DeltaSeconds);

	/** 上一帧扫描用的位置，用来沿路径扫而不是只扫当前位置 */
	FVector PreviousPedestrianSweepLocation = FVector::ZeroVector;
	bool bHasPedestrianSweepLocation = false;

	/** 被撞之后的残余状态，全部按 KnockDecay 指数衰减回 0 */
	FVector KnockVelocity = FVector::ZeroVector;
	float KnockYawRate = 0.0f;
	float KnockTilt = 0.0f;
	float ShakeAmount = 0.0f;
	float ShakePhase = 0.0f;

	/** 当前这句提示还剩多少秒；上车那一帧由 ExitPromptDuration 起算，只在本机上用 */
	float NoticeRemaining = 0.0f;
	FText NoticeText;
	bool bHadDriverLastFrame = false;

	// 骑手运动采样
	UPROPERTY(Replicated)
	FDeliveryRiderMotion ReplicatedRiderMotion;
	FDeliveryRiderMotion LocalRiderMotion;
	FVector2D RiderImpact = FVector2D::ZeroVector;
	float RiderSampleSpeed = 0, RiderSampleZ = 0, RiderSampleVerticalSpeed = 0, RiderNetElapsed = 0;
	bool bRiderSampleValid = false;
};
