#include "Vehicle/DeliveryRiderPose.h"
#if WITH_DEV_AUTOMATION_TESTS
#include "Misc/AutomationTest.h"
#include "Vehicle/DeliveryMotorbike.h"
#include "Vehicle/DeliveryRiderAnimInstance.h"
#include "Components/SkeletalMeshComponent.h"
#include "Engine/SkeletalMesh.h"
#include "Engine/World.h"
#include "Engine/Engine.h"
#include "DeliveryCharacter.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDeliveryRiderSpringTest, "Delivery.Vehicle.Rider.Spring",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FDeliveryRiderSpringTest::RunTest(const FString&)
{
	for (float Dt : {1.f/120, 1.f/60, 1.f/30, .2f})
	{
		DeliveryRiderPose::FSpring Spring;
		for (int32 I=0; I<300; ++I)
		{
			Spring.Step(100, Dt, 2.6f, .65f, 22);
			TestTrue(TEXT("bounded despite large input/hitches"), FMath::IsFinite(Spring.Value) && FMath::Abs(Spring.Value)<=22);
		}
		for (int32 I=0; I<600; ++I) Spring.Step(0, Dt, 2.6f, .65f, 22);
		TestTrue(TEXT("settles after stopping"), FMath::Abs(Spring.Value)<.001f);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDeliveryRiderContactsTest, "Delivery.Vehicle.Rider.AssetContacts",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FDeliveryRiderContactsTest::RunTest(const FString&)
{
	UClass* Class = LoadClass<ADeliveryMotorbike>(nullptr, TEXT("/Game/Vehicle/Motorbike/BP_Motorbike.BP_Motorbike_C"));
	if (!TestNotNull(TEXT("bike asset"), Class)) return false;
	const auto* Bike = Class->GetDefaultObject<ADeliveryMotorbike>();
	USkeletalMesh* Mesh = Bike->RiderMesh->GetSkeletalMeshAsset();
	if (!TestNotNull(TEXT("mesh survives component migration"), Mesh)) return false;
	TestTrue(TEXT("native animation parent"), Bike->RiderAnimationClass && Bike->RiderAnimationClass->IsChildOf(UDeliveryRiderAnimInstance::StaticClass()));
	DeliveryRiderPose::FRig Rig;
	if (!TestTrue(TEXT("required bones"), Rig.Initialize(Mesh->GetRefSkeleton()))) return false;
	float MaxGap = 0;
	for (float Steer : {-1.f, 0.f, 1.f})
	for (float Pitch : {-22.f, 0.f, 22.f})
	for (float Roll : {-20.f, 0.f, 20.f})
	{
		FTransform Contacts[4];
		const FQuat Turn(FVector::UpVector, FMath::DegreesToRadians(Steer * Bike->MaxVisualSteerAngle * Bike->HandlebarSteerRatio));
		for (int32 I=0; I<4; ++I)
		{
			Contacts[I] = Rig.Reference[Rig.Limbs[I].End];
			if (I<2)
			{
				Contacts[I].SetLocation(Bike->SteerPivotLocation + Turn.RotateVector(Contacts[I].GetLocation()-Bike->SteerPivotLocation));
				Contacts[I].SetRotation(Turn*Contacts[I].GetRotation());
			}
		}
		TArray<FTransform> Pose;
		Rig.Solve({Pitch, Roll, Steer*8, 2}, FVector::RightVector, -FVector::ForwardVector, Contacts, Pose);
		for (int32 I=0; I<4; ++I)
		{
			const auto& L = Rig.Limbs[I];
			const float Gap = FVector::Distance(Pose[L.End].GetLocation(), Contacts[I].GetLocation());
			MaxGap = FMath::Max(MaxGap, Gap);
			TestTrue(FString::Printf(TEXT("contact %d steer %.0f pitch %.0f roll %.0f gap %.2f"),I,Steer,Pitch,Roll,Gap), Gap<.1f);
			TestTrue(TEXT("no upper limb stretching"), FMath::IsNearlyEqual(
				FVector::Distance(Pose[L.Root].GetLocation(),Pose[L.Mid].GetLocation()),
				FVector::Distance(Rig.Reference[L.Root].GetLocation(),Rig.Reference[L.Mid].GetLocation()), .01));
		}
	}
	AddInfo(FString::Printf(TEXT("Maximum contact gap: %.4f cm"), MaxGap));
	return true;
}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDeliveryRiderRuntimeTest, "Delivery.Vehicle.Rider.AnimationRuntime",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FDeliveryRiderRuntimeTest::RunTest(const FString&)
{
	UClass* Class = LoadClass<ADeliveryMotorbike>(nullptr, TEXT("/Game/Vehicle/Motorbike/BP_Motorbike.BP_Motorbike_C"));
	if (!Class) return false;
	const auto Settings = UWorld::InitializationValues().AllowAudioPlayback(false).CreatePhysicsScene(true)
		.ShouldSimulatePhysics(false).CreateNavigation(false).CreateAISystem(false);
	UWorld* World = UWorld::CreateWorld(EWorldType::Game, false, NAME_None, nullptr, true, ERHIFeatureLevel::Num, &Settings);
	GEngine->CreateNewWorldContext(EWorldType::Game).SetCurrentWorld(World);
	auto* Bike = World->SpawnActor<ADeliveryMotorbike>(Class);
	Bike->SetActorRotation(FRotator(12, 73, 0));
	Bike->Driver = World->SpawnActor<ADeliveryCharacter>();
	Bike->RiderMesh->SetAnimInstanceClass(Bike->RiderAnimationClass);
	Bike->RiderMesh->SetVisibility(true);
	Bike->CalibrateRiderContacts();
	DeliveryRiderPose::FRig Rig;
	Rig.Initialize(Bike->RiderMesh->GetSkeletalMeshAsset()->GetRefSkeleton());
	const auto Evaluate = [&]()
	{
		Bike->RiderMesh->TickAnimation(1.f/60, false);
		Bike->RiderMesh->RefreshBoneTransforms();
	};
	Evaluate();
	const FTransform NeutralNeck = Bike->RiderMesh->GetComponentSpaceTransforms()[Rig.Neck];
	const FVector NeutralHead = NeutralNeck.GetLocation();
	float MaxGap = 0;
	float MaxHeadStep = 0;
	float MaxFootGap = 0, MaxHeadAngleStep = 0;
	FQuat PreviousHeadRotation = NeutralNeck.GetRotation();
	FVector PreviousHead = NeutralHead;
	for (int32 Frame=0; Frame<240; ++Frame)
	{
		Bike->SteerInput = FMath::Sin(Frame*.03f);
		Bike->LocalRiderMotion.Speed = .8f;
		Bike->LocalRiderMotion.Steer = Bike->SteerInput;
		Bike->LocalRiderMotion.Acceleration = Frame<120 ? 1 : -1;
		Bike->UpdateSteerVisual(1.f/60);
		Evaluate();
		const FVector Head = Bike->RiderMesh->GetComponentSpaceTransforms()[Rig.Neck].GetLocation();
		MaxHeadStep = FMath::Max(MaxHeadStep, float(FVector::Distance(PreviousHead,Head)));
		PreviousHead = Head;
		const FQuat HeadRotation = Bike->RiderMesh->GetComponentSpaceTransforms()[Rig.Neck].GetRotation();
		MaxHeadAngleStep = FMath::Max(MaxHeadAngleStep, float(FMath::RadiansToDegrees(PreviousHeadRotation.AngularDistance(HeadRotation))));
		PreviousHeadRotation = HeadRotation;
		FDeliveryRiderFrame Targets;
		Bike->BuildRiderAnimationFrame(Targets);
		for (int32 I=2; I<4; ++I)
			MaxFootGap = FMath::Max(MaxFootGap, float(FVector::Distance(
				Bike->RiderMesh->GetComponentSpaceTransforms()[Rig.Limbs[I].End].GetLocation(),Targets.Contacts[I].GetLocation())));
		for (int32 I=0; I<4; ++I)
			MaxGap = FMath::Max(MaxGap, float(FVector::Distance(
				Bike->RiderMesh->GetComponentSpaceTransforms()[Rig.Limbs[I].End].GetLocation(),Targets.Contacts[I].GetLocation())));
	}
	TestTrue(FString::Printf(TEXT("real AnimBP relaxed contact gap %.4f cm"),MaxGap), MaxGap<5.2f);
	const float HeadMotion = FVector::Distance(NeutralHead,Bike->RiderMesh->GetComponentSpaceTransforms()[Rig.Neck].GetLocation());
	TestTrue(FString::Printf(TEXT("real animation moves upper body %.3f cm"),HeadMotion), HeadMotion>1.f);
	TestTrue(FString::Printf(TEXT("continuous head movement %.3f cm/frame"),MaxHeadStep), MaxHeadStep<4.f);
	TestTrue(TEXT("feet remain planted despite hand slack"),MaxFootGap<.2f);
	TestTrue(FString::Printf(TEXT("continuous head rotation %.3f degrees/frame"),MaxHeadAngleStep),MaxHeadAngleStep<12.f);
	// 平路匀速直行也必须明显摆动，不能只验证转弯/刹车时身体有变化。
	Bike->SteerInput = 0;
	Bike->LocalRiderMotion = {};
	Bike->LocalRiderMotion.Speed = .6f;
	FQuat WindStart = FQuat::Identity;
	float WindAngle = 0, WindGap = 0;
	float MinBackTilt = 1.f, MaxSideTilt = 0.f;
	for (int32 Frame=0; Frame<360; ++Frame)
	{
		Bike->UpdateSteerVisual(1.f/60);
		Evaluate();
		const auto& Pose = Bike->RiderMesh->GetComponentSpaceTransforms();
		if (Frame==120) WindStart = Pose[Rig.Neck].GetRotation();
		if (Frame>120) WindAngle = FMath::Max(WindAngle, float(FMath::RadiansToDegrees(WindStart.AngularDistance(Pose[Rig.Neck].GetRotation()))));
		if (Frame>120)
		{
			// 骨架前向为 +Y：后仰后看向上方（Z>0），直行不应产生横向看偏（X）。
			const FQuat Delta = Pose[Rig.Neck].GetRotation() * Rig.Reference[Rig.Neck].GetRotation().Inverse();
			const FVector Facing = Delta.RotateVector(FVector::RightVector);
			MinBackTilt = FMath::Min(MinBackTilt, float(Facing.Z));
			MaxSideTilt = FMath::Max(MaxSideTilt, float(FMath::Abs(Facing.X)));
		}
		FDeliveryRiderFrame Targets;
		Bike->BuildRiderAnimationFrame(Targets);
		for (int32 I=0; I<4; ++I) WindGap = FMath::Max(WindGap, float(FVector::Distance(Pose[Rig.Limbs[I].End].GetLocation(),Targets.Contacts[I].GetLocation())));
	}
	TestTrue(FString::Printf(TEXT("straight cruising keeps head tilted back %.3f"),MinBackTilt),MinBackTilt>.2f);
	TestTrue(FString::Printf(TEXT("straight cruising does not shake sideways %.3f"),MaxSideTilt),MaxSideTilt<.05f);
	TestTrue(FString::Printf(TEXT("straight cruising has visible fore-aft rebound %.2f degrees"),WindAngle),WindAngle>8.f);
	TestTrue(TEXT("strong wind stays within allowed 5cm contact slack"),WindGap<5.2f);
	Bike->LocalRiderMotion = {};
	for (int32 Frame=0; Frame<300; ++Frame) Evaluate();
	// 有驾驶员时的中立姿势包含握把可达性补偿，不等于隐藏/下车后的裸参考姿势。
	TestTrue(TEXT("wind settles when parked"),Bike->RiderMesh->GetComponentSpaceTransforms()[Rig.Neck].Equals(NeutralNeck,.02));
	AddInfo(FString::Printf(TEXT("Cruising wind excursion %.2f degrees, contact gap %.4f cm"),WindAngle,WindGap));
	// 模拟代理只能读取复制状态；本地车辆控制量不应污染远端动画。
	Bike->SetRole(ROLE_SimulatedProxy);
	Bike->ReplicatedRiderMotion.Speed = -.4f;
	Bike->ReplicatedRiderMotion.Steer = -.7f;
	FDeliveryRiderFrame RemoteFrame;
	Bike->BuildRiderAnimationFrame(RemoteFrame);
	TestEqual(TEXT("remote speed reads replicated input"),RemoteFrame.Motion.Speed,-.4f);
	TestEqual(TEXT("remote steer reads replicated input"),RemoteFrame.Motion.Steer,-.7f);
	Bike->SetRole(ROLE_Authority);
	Bike->Driver = nullptr;
	Bike->ResetRiderMotion();
	Evaluate();
	TestTrue(TEXT("dismount clears pose"), Bike->RiderMesh->GetComponentSpaceTransforms()[Rig.Neck].Equals(Rig.Reference[Rig.Neck],.01));
	AddInfo(FString::Printf(TEXT("AnimBP maximum contact gap %.4f cm, head displacement %.3f cm, max step %.3f cm"),MaxGap,HeadMotion,MaxHeadStep));
	World->DestroyWorld(false);
	GEngine->DestroyWorldContext(World);
	return true;
}
#endif
