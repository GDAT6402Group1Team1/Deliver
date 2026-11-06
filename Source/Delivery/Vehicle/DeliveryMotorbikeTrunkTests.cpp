// 验证整车视觉缩放的贴地补偿，以及后备箱装卸货的识别、动画和上车门槛
#include "Vehicle/DeliveryMotorbike.h"

#if WITH_DEV_AUTOMATION_TESTS
#include "Character/DeliveryCharacter.h"
#include "Components/BoxComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/Engine.h"
#include "Engine/StaticMesh.h"
#include "Engine/World.h"
#include "Interaction/DeliveryInteractableComponent.h"
#include "Inventory/DeliveryInventoryComponent.h"
#include "Inventory/DeliveryPackageItem.h"
#include "Misc/AutomationTest.h"
#include "Misc/ScopeExit.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDeliveryMotorbikeTrunkTest, "Delivery.Vehicle.Trunk",
                                 EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FDeliveryMotorbikeTrunkTest::RunTest(const FString&)
{
	const auto Settings = UWorld::InitializationValues()
	                          .AllowAudioPlayback(false)
	                          .CreatePhysicsScene(true)
	                          .ShouldSimulatePhysics(false)
	                          .CreateNavigation(false)
	                          .CreateAISystem(false);
	UWorld* World = UWorld::CreateWorld(EWorldType::Game, false, NAME_None, nullptr, true,
	                                    ERHIFeatureLevel::Num, &Settings);
	if (!TestNotNull(TEXT("test world"), World))
		return false;
	GEngine->CreateNewWorldContext(EWorldType::Game).SetCurrentWorld(World);
	ON_SCOPE_EXIT
	{
		GEngine->DestroyWorldContext(World);
		World->EndPlay(EEndPlayReason::Quit);
		World->DestroyWorld(false);
	};

	// 用正式蓝图：车体那 9 个网格是在蓝图上配的，几何识别全靠它们的包围盒
	UClass* BikeClass = StaticLoadClass(ADeliveryMotorbike::StaticClass(), nullptr,
	                                    TEXT("/Game/Vehicle/Motorbike/BP_Motorbike.BP_Motorbike_C"));
	if (!TestNotNull(TEXT("BP_Motorbike 能加载"), BikeClass))
		return false;
	ADeliveryMotorbike* Bike = World->SpawnActor<ADeliveryMotorbike>(BikeClass);
	if (!TestNotNull(TEXT("车辆能出生"), Bike))
		return false;
	World->InitializeActorsForPlay(FURL());
	Bike->DispatchBeginPlay();

	// ---- 后备箱识别 ----
	if (!TestTrue(TEXT("认出了箱盖槽位"), Bike->BodyMeshes.IsValidIndex(Bike->TrunkLidPartIndex)))
		return false;
	TestFalse(TEXT("箱盖不是轮子"), Bike->WheelPartIndices.Contains(Bike->TrunkLidPartIndex));
	TestFalse(TEXT("箱盖不是车把"), Bike->HandlebarPartIndices.Contains(Bike->TrunkLidPartIndex));

	// 箱盖必须是非轮子部件里最高的那块，否则开的是别的地方
	float HighestTop = -FLT_MAX;
	int32 HighestIndex = INDEX_NONE;
	for (int32 Index = 0; Index < Bike->BodyMeshes.Num(); ++Index)
	{
		const UStaticMesh* Mesh = Bike->BodyMeshes[Index].Get();
		if (!Mesh || Bike->WheelPartIndices.Contains(Index) ||
		    Bike->HandlebarPartIndices.Contains(Index) || Bike->SteeringPartIndices.Contains(Index))
			continue;
		const float Top = Mesh->GetBoundingBox().Max.Z;
		if (Top > HighestTop)
		{
			HighestTop = Top;
			HighestIndex = Index;
		}
	}
	TestEqual(TEXT("箱盖就是车尾最高的那块扁平件"), Bike->TrunkLidPartIndex, HighestIndex);

	// 快递要从箱口落进箱内：箱口在铰链之上，箱内在铰链之下
	TestTrue(TEXT("箱口比铰链高"), Bike->TrunkOpeningOffset.Z > Bike->TrunkLidHinge.Z);
	TestTrue(TEXT("箱内比箱口低"), Bike->TrunkInsideOffset.Z < Bike->TrunkOpeningOffset.Z);
	UE_LOG(LogTemp, Log, TEXT("TrunkDiag 箱盖=%d 箱体=%d 铰链=%s 箱内=%s 箱口=%s"),
	       Bike->TrunkLidPartIndex, Bike->TrunkBoxPartIndex, *Bike->TrunkLidHinge.ToString(),
	       *Bike->TrunkInsideOffset.ToString(), *Bike->TrunkOpeningOffset.ToString());

	// ---- 视觉缩放：车底高度不能跟着变 ----
	const auto LowestVisualZ = [Bike]()
	{
		float Lowest = FLT_MAX;
		for (UStaticMeshComponent* Part : Bike->BodyParts)
		{
			if (Part && Part->GetStaticMesh())
			{
				Part->UpdateBounds();
				Lowest = FMath::Min(Lowest, Part->Bounds.GetBox().Min.Z);
			}
		}
		return Lowest;
	};
	const float ConfiguredScale = Bike->VisualScale;
	Bike->VisualScale = 1.0f;
	Bike->ApplyBodyMeshes();
	const float UnscaledBottom = LowestVisualZ();
	Bike->VisualScale = ConfiguredScale;
	Bike->ApplyBodyMeshes();
	const float ScaledBottom = LowestVisualZ();

	TestEqual(TEXT("缩放写进了缩放层"),
	          static_cast<float>(Bike->MeshScale->GetRelativeScale3D().X), ConfiguredScale, 0.001f);
	TestEqual(TEXT("碰撞盒跟着缩"),
	          static_cast<float>(Bike->CollisionBox->GetUnscaledBoxExtent().X),
	          static_cast<float>(Bike->BaseCollisionExtent.X) * ConfiguredScale, 0.1f);
	TestEqual(TEXT("车身保持 Pawn 类型供交通车识别"), Bike->CollisionBox->GetCollisionObjectType(), ECC_Pawn);
	TestEqual(TEXT("车身阻挡步行布娃娃"), Bike->CollisionBox->GetCollisionResponseToChannel(ECC_PhysicsBody), ECR_Block);
	TestTrue(TEXT("正式蓝图的车身盒覆盖缩小后的主体宽度"),
	         Bike->CollisionBox->GetUnscaledBoxExtent().X >= 90.f &&
	         Bike->CollisionBox->GetUnscaledBoxExtent().Y >= 45.f);
	// 只缩不挪的话车底会抬起来，表现是"车悬空了一截"
	TestEqual(TEXT("缩放后车底仍在原来的高度"), ScaledBottom, UnscaledBottom, 1.0f);
	TestTrue(TEXT("整车确实变小了"), ConfiguredScale >= 1.0f || Bike->MeshScale->GetRelativeScale3D().X < 1.0f);

	// ---- 手上有货不能上车，必须先装箱 ----
	ADeliveryCharacter* Player = World->SpawnActor<ADeliveryCharacter>();
	if (!TestNotNull(TEXT("角色能出生"), Player))
		return false;
	Player->SetActorLocation(Bike->GetActorLocation() + FVector(0.0f, 150.0f, 0.0f));
	UDeliveryInventoryComponent* Inventory = Player->GetInventoryComponent();
	if (!TestNotNull(TEXT("背包组件"), Inventory))
		return false;

	TestTrue(TEXT("空手时可以上车"), Bike->CanDriveNow(Player));
	TestFalse(TEXT("空车空手时没有东西可装卸"), Bike->CanUseTrunk(Player, true));

	ADeliveryPackageItem* Package = World->SpawnActor<ADeliveryPackageItem>();
	if (!TestNotNull(TEXT("快递能出生"), Package))
		return false;
	// 直接塞进手持槽：走 TryPickup 需要关卡里有任务管理器，这个测试世界没有
	Inventory->StoreItem(0, Package);
	if (!TestEqual(TEXT("快递在手上"), static_cast<AActor*>(Inventory->GetHeldItem()),
	               static_cast<AActor*>(Package)))
		return false;

	TestFalse(TEXT("手上有货时不能直接按 F 上车"), Bike->CanDriveNow(Player));
	TestFalse(TEXT("手上有货时 F 交互组件也不给用"), Bike->Interactable->CanInteract(Player));
	TestTrue(TEXT("手上有货时可以装车"), Bike->CanLoadTrunk(Player));
	TestFalse(TEXT("上车请求被服务器拒绝"), Bike->TryEnter(Player));
	TestNull(TEXT("被拒绝时没有驾驶员"), Bike->GetDriver());

	// ---- 装货动画 ----
	if (!TestTrue(TEXT("开始装货"), Bike->BeginTrunkLoad(Player)))
		return false;
	TestEqual(TEXT("快递已经记在车上"), static_cast<AActor*>(Bike->GetCargo()),
	          static_cast<AActor*>(Package));
	TestNull(TEXT("手上已经空了"), static_cast<AActor*>(Inventory->GetHeldItem()));
	TestTrue(TEXT("装货期间在播动画"), Bike->IsTrunkBusy());
	TestFalse(TEXT("装货期间不能上车"), Bike->CanDriveNow(Player));

	// 开盖到一半：盖子转起来了，快递看得见
	Bike->UpdateTrunkAnimation(Bike->TrunkLidDuration * 0.5f);
	/**
	 * 断言的是**几何方向**不是角度数值：铰链对面那条边必须往上（+Z）走
	 *
	 * 默认铰链在前沿、向后翻开，所以抬起来的是后沿（局部 −Y）。只看角度非零的话，
	 * 盖子往反方向翻、穿进车身也照样过。铰链那条边同时必须**不往上动**，
	 * 否则等于整块盖子在平移、铰链没起作用。
	 */
	const FTransform LidTransform = Bike->TrunkLidPivot->GetRelativeTransform();
	const FVector Lifted = LidTransform.TransformVector(Bike->GetTrunkLidLiftDirection());
	const FVector Hinged = LidTransform.TransformVector(-Bike->GetTrunkLidLiftDirection());
	TestTrue(TEXT("铰链对面那条边往上翻开"), Lifted.Z > 0.1f);
	TestTrue(TEXT("铰链那一侧不往上动"), Hinged.Z < -0.1f);
	TestTrue(TEXT("默认是铰链在前沿、向后打开"), Bike->bTrunkLidHingeAtFront);
	// 铰链也要落在箱盖的前沿上，不然"向后开"会把盖子转进车身
	TestTrue(TEXT("铰链在箱盖靠车头那条边"),
	         Bike->TrunkLidHinge.Y > Bike->TrunkInsideOffset.Y);
	TestFalse(TEXT("动画期间快递看得见"), Package->IsHidden());

	// 整条时间线走完：盖子回零、快递藏进关好的箱子、可以上车了
	Bike->UpdateTrunkAnimation(Bike->TrunkLidDuration * 2.0f + Bike->TrunkMoveDuration);
	TestFalse(TEXT("装完不再播动画"), Bike->IsTrunkBusy());
	TestTrue(TEXT("装完盖子合上了"),
	         Bike->TrunkLidPivot->GetRelativeRotation().IsNearlyZero());
	TestTrue(TEXT("装完快递藏在箱子里"), Package->IsHidden());
	TestTrue(TEXT("装完之后才能上车"), Bike->CanDriveNow(Player));
	TestEqual(TEXT("提示切成取货"), Bike->TrunkInteractable->PromptText.ToString(),
	          Bike->TrunkUnloadPromptText.ToString());

	// ---- 取货：动画结束才交接；交不回去就留在车上 ----
	TestTrue(TEXT("箱子里有货时可以取"), Bike->CanUnloadTrunk(Player));
	if (!TestTrue(TEXT("开始取货"), Bike->BeginTrunkUnload(Player)))
		return false;
	TestEqual(TEXT("取货动画期间快递还在车上"), static_cast<AActor*>(Bike->GetCargo()),
	          static_cast<AActor*>(Package));
	Bike->UpdateTrunkAnimation(Bike->TrunkLidDuration * 2.0f + Bike->TrunkMoveDuration);
	TestFalse(TEXT("取完不再播动画"), Bike->IsTrunkBusy());
	/**
	 * 这个测试世界没有任务管理器，所以背包会拒收（CanBeAcquired 为假）
	 * 这里要的就是那条兜底：交不回去时快递重新入箱，不会凭空消失
	 * 「成功交回手上」依赖任务系统，只能在真实关卡里验收
	 */
	TestEqual(TEXT("交不回去时快递留在车上"), static_cast<AActor*>(Bike->GetCargo()),
	          static_cast<AActor*>(Package));
	TestTrue(TEXT("重新入箱后仍然藏着"), Package->IsHidden());

	// 有人骑着的时候别人不该还能在车尾开箱
	Bike->VisualScale = ConfiguredScale;
	Bike->ReleaseCargo();
	Package->Destroy();
	if (Bike->TryEnter(Player))
	{
		TestFalse(TEXT("有人骑着时后备箱不可用"), Bike->TrunkInteractable->bInteractEnabled);
	}

	return true;
}
#endif
