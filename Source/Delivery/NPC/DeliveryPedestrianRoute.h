// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "DeliveryPedestrianRoute.generated.h"

class ADeliveryPedestrian;
class UAnimSequence;
class UMaterialInterface;
class USkeletalMesh;
class USplineComponent;

/** 一种路人的外观与出现概率，权重只在同一条路线内部做相对比较 */
USTRUCT(BlueprintType)
struct FDeliveryPedestrianKind
{
	GENERATED_BODY()

	/**
	 * 要用的骨骼网格
	 * 用软引用而不是硬引用：这些资产由导入脚本现建，硬引用会让"资产还没导入"
	 * 变成关卡加载报错，而软引用只是这一种暂时抽不出来
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Pedestrian")
	TSoftObjectPtr<USkeletalMesh> Mesh;

	/** 相对权重，0 表示这一种暂时不出 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Pedestrian", meta = (ClampMin = "0.0"))
	float Weight = 1.f;

	/**
	 * 这一种的身高系数，1 = 导入时的尺寸
	 * **必须在这里写**：六份源模型按**老的 FbxFactory**
	 * 路线导进来的包围盒高度实测全是 99.8 cm（归一化过的），所以同一个导入缩放
	 * 会把所有人都拉到同一身高——小男孩和中年男一样高。
	 * （被 Interchange 接走时量到的是 192 cm：那条路把 FbxImportUI 的缩放整份忽略、
	 * 按自己的单位换算，所以**报告里那一列"导入路"要看一眼**，两个数不是同一回事。）
	 * 相对高矮不在模型里，只能在这儿表达
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Pedestrian",
	          meta = (ClampMin = "0.1", ClampMax = "3.0"))
	float HeightScale = 1.f;
};

/**
 * 关卡里的路人生成点。普通路人默认在生成点周围短距离游走；
 * 需要指定路线时可关闭 bRoamInArea，继续使用样条和接驳。
 * 生成只在服务器执行，路人自己复制移动，和交通车同一套做法
 */
UCLASS()

class DELIVERY_API ADeliveryPedestrianRoute : public AActor
{
	GENERATED_BODY()
	friend class FDeliveryBikeHornTest;

  public:
	ADeliveryPedestrianRoute();

	virtual void Tick(float DeltaTime) override;
	/** 服务器收到车前过街行人的提示后，整条线统一加速 */
	void NotifyHorn(float SpeedMultiplier);

	// 路人外观
	/** 这条线上会出现哪些路人，按权重随机 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Pedestrian|外观")
	TArray<FDeliveryPedestrianKind> Kinds;

	/** 小孩队伍每轮第一人的外观；留空则沿用普通随机生成 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Pedestrian|外观")
	TArray<FDeliveryPedestrianKind> LeaderKinds;

	/** 与 Kinds 同序的淡化材质；仅启用渐隐的路线使用 */
	UPROPERTY(EditAnywhere, Category = "Pedestrian|外观")
	TArray<TSoftObjectPtr<UMaterialInterface>> FadeMaterials;

	/** 与 LeaderKinds 同序的渐隐材质 */
	UPROPERTY(EditAnywhere, Category = "Pedestrian|外观")
	TArray<TSoftObjectPtr<UMaterialInterface>> LeaderFadeMaterials;

	UPROPERTY(EditAnywhere, Category = "Pedestrian|出入场")
	bool bFadeInsteadOfScale = false;

	UMaterialInterface* GetFadeMaterialForMesh(const USkeletalMesh* Mesh) const;

	/**
	 * 全部路人共用的走路动画
	 * 六个 NPC 的骨骼命名一致、共用一个 Skeleton，所以一份动画够了
	 * 留空时路人仍会走，只是没有腿部动作（滑步），不会报错
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Pedestrian|外观")
	TSoftObjectPtr<UAnimSequence> WalkAnim;

	/**
	 * 走路动画本身对应的前进速度（cm/s）
	 * 实际速度与它的比值就是动画播放速率，所以随机出来的快慢不会变成滑步
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Pedestrian|外观",
	          meta = (ClampMin = "1.0", Units = "cm/s"))
	float WalkAnimReferenceSpeed = 150.f;

	/** 生成的路人类，留空时用内置的 ADeliveryPedestrian */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Pedestrian|外观")
	TSubclassOf<ADeliveryPedestrian> PedestrianClass;

	// 可选的斑马线过街闸门；只限制新生成，不改变已在路上的 NPC
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Pedestrian|过街")
	TObjectPtr<AActor> CrosswalkActor;

	/** 对应车道段的 StopAtInter 为 true 时，人行方向为绿 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Pedestrian|过街")
	TObjectPtr<AActor> SignalSegment;

	/** 红灯后已出发的小孩加速通过，绿灯恢复正常速度 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Pedestrian|过街", meta = (ClampMin = "1.0"))
	float RedLightWalkSpeedMultiplier = 1.5f;

	/** 仅覆盖本路线生成路人的前方探测距离；负值沿用路人默认值 */
	UPROPERTY(EditAnywhere, Category = "Pedestrian|过街", meta = (Units = "cm"))
	float YieldProbeDistanceOverride = -1.f;

	/** 在样条走廊内使用分离、对齐和聚合；活动圆游走仍沿用原逻辑 */
	UPROPERTY(EditAnywhere, Category = "Pedestrian|Boids")
	bool bUseBoids = false;

	UPROPERTY(EditAnywhere, Category = "Pedestrian|Boids", meta = (ClampMin = "50", Units = "cm"))
	float BoidsNeighborRadius = 200.f;

	UPROPERTY(EditAnywhere, Category = "Pedestrian|Boids", meta = (ClampMin = "20", Units = "cm"))
	float BoidsSeparationDistance = 65.f;

	/** 相对各自路线中心最多横移多远，不能借群体聚合走到车道外 */
	UPROPERTY(EditAnywhere, Category = "Pedestrian|Boids", meta = (ClampMin = "0", Units = "cm"))
	float BoidsCorridorHalfWidth = 65.f;

	UPROPERTY(EditAnywhere, Category = "Pedestrian|Boids", meta = (ClampMin = "0", ClampMax = "3"))
	float BoidsSeparationWeight = 1.8f;

	UPROPERTY(EditAnywhere, Category = "Pedestrian|Boids", meta = (ClampMin = "0", ClampMax = "1"))
	float BoidsAlignmentWeight = .35f;

	UPROPERTY(EditAnywhere, Category = "Pedestrian|Boids", meta = (ClampMin = "0", ClampMax = "1"))
	float BoidsCohesionWeight = .15f;

	/** 限制转向加速度，单位 cm/s²，避免低帧率或邻居变化时横向跳动 */
	UPROPERTY(EditAnywhere, Category = "Pedestrian|Boids", meta = (ClampMin = "1"))
	float BoidsMaxAcceleration = 260.f;

	// 生成节奏
	/** 这条线上同时最多几个人 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Pedestrian|生成", meta = (ClampMin = "0"))
	int32 MaxAlive = 6;

	/** 相邻两次生成的最短间隔（秒） */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Pedestrian|生成",
	          meta = (ClampMin = "0.0", Units = "s"))
	float SpawnIntervalMin = 1.5f;

	/** 相邻两次生成的最长间隔（秒） */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Pedestrian|生成",
	          meta = (ClampMin = "0.0", Units = "s"))
	float SpawnIntervalMax = 4.f;

	/** 开局首名路人额外等待的秒数；可让平行路线错开生成节拍 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Pedestrian|生成",
	          meta = (ClampMin = "0.0", Units = "s"))
	float InitialSpawnDelay = 0.f;

	/** 每轮人行绿灯新出发人数；0 表示沿用原来的持续生成 */
	UPROPERTY(EditAnywhere, Category = "Pedestrian|生成", meta = (ClampMin = "0"))
	int32 GreenPhaseSpawnMin = 0;

	UPROPERTY(EditAnywhere, Category = "Pedestrian|生成", meta = (ClampMin = "0"))
	int32 GreenPhaseSpawnMax = 0;

	/** 双道路线共用第一条路线的每轮人数 */
	UPROPERTY(EditAnywhere, Category = "Pedestrian|生成")
	TObjectPtr<ADeliveryPedestrianRoute> SharedGreenPhaseBudgetOwner;

	/**
	 * 开局就把线段填到 MaxAlive，位置随机铺开
	 * 不填的话玩家一进图看到的是空街，再过十几秒人才陆续从端点冒出来
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Pedestrian|生成")
	bool bPrewarmOnBeginPlay = true;

	/** 允许路人从终点往起点走，即双向人流 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Pedestrian|生成")
	bool bBothDirections = true;

	/** 普通路人在本生成点周围游走，不沿长样条或接驳到别的生成点。 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Pedestrian|游走")
	bool bRoamInArea = true;

	/** 活动半径；路人每一步的目标都在这个圆内。 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Pedestrian|游走",
	          meta = (ClampMin = "50.0", Units = "cm", EditCondition = "bRoamInArea"))
	float RoamRadius = 300.f;

	/** 同一生成点的路人出生时至少相隔的距离。 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Pedestrian|游走",
	          meta = (ClampMin = "0.0", Units = "cm", EditCondition = "bRoamInArea"))
	float RoamSpawnSpacing = 90.f;

	/** 普通路人走多远后随机停一下，和长路线的停顿参数分开。 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Pedestrian|游走",
	          meta = (ClampMin = "0.0", Units = "cm", EditCondition = "bRoamInArea"))
	float RoamPauseDistanceMin = 120.f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Pedestrian|游走",
	          meta = (ClampMin = "0.0", Units = "cm", EditCondition = "bRoamInArea"))
	float RoamPauseDistanceMax = 320.f;

	// 行走
	/** 随机速度下限（cm/s） */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Pedestrian|行走",
	          meta = (ClampMin = "1.0", Units = "cm/s"))
	float WalkSpeedMin = 110.f;

	/** 随机速度上限（cm/s） */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Pedestrian|行走",
	          meta = (ClampMin = "1.0", Units = "cm/s"))
	float WalkSpeedMax = 190.f;

	/** 路人横向随机偏移的上限（cm），让人流不要排成一条直线 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Pedestrian|行走",
	          meta = (ClampMin = "0.0", Units = "cm"))
	float LateralJitter = 45.f;

	/** 关闭后沿线持续行走，适合单列队伍 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Pedestrian|行走")
	bool bPauseWhileWalking = true;

	// 路线接驳（走到头换下一条继续走）
	/**
	 * 允许路人走到端点后换一条相连的路线继续走，而不是缩掉退场
	 * 关掉就回到"一条线走到头就消失"
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Pedestrian|接驳")
	bool bAllowHandoff = true;

	/**
	 * 两条路线的端点离多近算相连（cm）
	 * 判据只看**端点到端点**，不看中段：从中间横穿过去的两条路不算连通，
	 * 否则路人会在路中央凭空拐弯。和交通车那套"终点有没有同向下家"同一个思路
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Pedestrian|接驳",
	          meta = (ClampMin = "1.0", Units = "cm"))
	float ConnectDistance = 400.f;

	/**
	 * 一个路人最多接驳几次，之后在下一个端点退场
	 * **不能不限**：不限的话开局那批人会一直活着在路网里转，
	 * 路线的人数上限永远是满的、再也不生成新人，街上永远是同样几个面孔
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Pedestrian|接驳",
	          meta = (ClampMin = "0"))
	int32 MaxHandoffs = 6;

	// 出入场
	/**
	 * 距线段端点多远完成放大 / 开始缩小（cm）
	 * 用距离而不是时间：快走的人和慢走的人在同一个位置完成出场，
	 * 按时间算的话快的人会在离端点很远的地方就已经是原尺寸了
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Pedestrian|出入场",
	          meta = (ClampMin = "1.0", Units = "cm"))
	float AppearDistance = 220.f;

	/** 缩到多小就算消失，用来决定什么时候销毁 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Pedestrian|出入场",
	          meta = (ClampMin = "0.0", ClampMax = "0.5"))
	float MinVisibleScale = 0.02f;

	// 样条查询
	USplineComponent* GetSpline() const
	{
		return Spline;
	}

	/** 线段总长（cm） */
	float GetRouteLength() const;

	/** 样条上 Distance 处的世界位置 */
	FVector GetLocationAtDistance(float Distance) const;

	/** 样条上 Distance 处的世界切线方向（已归一化） */
	FVector GetDirectionAtDistance(float Distance) const;

	float GetAppearDistance() const
	{
		return AppearDistance;
	}

	float GetMinVisibleScale() const
	{
		return MinVisibleScale;
	}

	/** 只在活动圆内选点；起身和转向也使用同一范围。 */
	FVector PickRoamPoint(const FVector& From, float MinTravelDistance = 0.f) const;

	/** 路人退场时回报，便于立刻补一个新的而不用等下一次扫描 */
	void NotifyPedestrianFinished(ADeliveryPedestrian* Pedestrian);

	/**
	 * 找一条从本路线某一端接下去的路线
	 * @param bFromFinishEnd true = 从样条终点那头出去，false = 从起点那头
	 * @param OutRoute       接下去走的路线
	 * @param OutEnterAtStart 从那条路线的起点进入还是终点进入
	 * @return 找到就 true；目标路线已经满员时也返回 false（人数上限仍然要守）
	 */
	bool FindContinuation(bool bFromFinishEnd, ADeliveryPedestrianRoute*& OutRoute,
	                      bool& OutEnterAtStart);

	/** 接收一个从别的路线走过来的路人，只是把它记进在场名单 */
	void AcceptHandoff(ADeliveryPedestrian* Pedestrian);

	/** 这一端有没有下家；路人据此决定要不要在接近端点时缩小 */
	bool HasContinuation(bool bFromFinishEnd);

	/**
	 * 样条上离这个世界坐标最近的那一点对应的距离（cm）
	 *
	 * 被打飞的路人起身时用它回到线上：人可能滑出去很远，
	 * 不重新对齐的话起身结束那一帧会横向瞬移一下。
	 */
	float GetDistanceAtLocation(const FVector& WorldLocation) const;

	/** 本路线两个端点的世界位置，调试命令用 */
	void GetEndpoints(FVector& OutStart, FVector& OutFinish) const;

	/** 当前活着的路人，调试命令用 */
	const TArray<TWeakObjectPtr<ADeliveryPedestrian>>& GetAlivePedestrians() const
	{
		return Alive;
	}

	/**
	 * 立刻生成一个路人，成功返回它
	 * bAtEntrance 为真时从端点进场，否则沿线段随机落点（开局铺人用）
	 */
	ADeliveryPedestrian* SpawnOnePedestrian(bool bAtEntrance);

  protected:
	virtual void BeginPlay() override;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Components")
	TObjectPtr<USplineComponent> Spline;

  private:
	/** 丢掉已经销毁的弱引用，返回剩下的个数 */
	void UpdateCrosswalkSpeed();
	// 路线持有短租约，无车辆引用，多车只取最高倍率
	float HornBoostUntil = -1.f;
	float HornSpeedMultiplier = 1.f;
	int32 CompactAlive();

	/**
	 * 按权重挑一种路人，输出它的网格和身高系数
	 * 挑"哪一种"和取"它的尺寸"必须是同一次决定——分两次查会在
	 * 权重随机那一步各挑一次，表现是小孩配着大人的身高
	 */
	USkeletalMesh* PickKind(const TArray<FDeliveryPedestrianKind>& Options,
	                     float& OutHeightScale) const;

	bool IsPedestrianGreen() const;
	bool CanSpawnAtCrosswalk() const;
	void UpdateLeaderPhase(bool bGreen);
	void UpdateGreenPhaseBudget(bool bGreen);
	ADeliveryPedestrianRoute* GetGreenPhaseBudgetOwner();
	void ResetSpawnTimer();

	TArray<TWeakObjectPtr<ADeliveryPedestrian>> Alive;

	/** 距下一次生成还有多少秒 */
	float SpawnCountdown = 0.f;
	bool bWasPedestrianGreen = false;
	bool bWasPedestrianGreenForLeader = false;
	bool bNextSpawnIsLeader = true;
	int32 GreenPhaseSpawnTarget = 0;
	int32 GreenPhaseSpawned = 0;

	/** 一条相连的路线：从哪条路、从它的哪一头进去 */
	struct FConnection
	{
		TWeakObjectPtr<ADeliveryPedestrianRoute> Route;
		bool bEnterAtStart = true;
	};

	/**
	 * 懒加载的连通表，索引 0 = 起点那头、1 = 终点那头
	 * **不在 BeginPlay 里建**：那时候别的路线可能还没 BeginPlay，
	 * 虽然 actor 已经存在、迭代得到，但样条点在构造脚本之后才定型；
	 * 第一次真的要用时再算，算完缓存
	 */
	TArray<FConnection> Connections[2];
	bool bConnectionsBuilt = false;

#if WITH_DEV_AUTOMATION_TESTS
	friend class FDeliveryPedestrianWalkTest;
#endif

	void BuildConnections();
};
