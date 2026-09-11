#include "DeliveryBoxingPose.h"
#if WITH_DEV_AUTOMATION_TESTS
#include "DeliveryHandPose.h"
#include "Misc/AutomationTest.h"
#include "Engine/World.h"
#include "Engine/Engine.h"
#include "Engine/SkeletalMesh.h"
#include "Components/SkeletalMeshComponent.h"
#include "PhysicsEngine/PhysicsAsset.h"
#include "PhysicsEngine/PhysicsConstraintTemplate.h"
#include "PhysicsEngine/BodyInstance.h"
#include "PhysicsControlComponent.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDeliveryBoxingGeometryTest, "Delivery.Boxing.StraightReach",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FDeliveryBoxingGeometryTest::RunTest(const FString&)
{
	for (float Yaw : { 0.f, 90.f, 173.f })
	for (float Side : { -1.f, 1.f })
	for (float Alpha : { 0.f, 0.5f, 1.f })
	{
		const FVector Forward = FRotator(0,Yaw,0).Vector();
		const FVector Right = FVector::CrossProduct(FVector::UpVector, Forward);
		const FVector Shoulder = Right * Side * 20;
		const FVector Hand = Shoulder + Forward * FMath::Lerp(25.2f,56.4f,Alpha)
			- FVector::UpVector * FMath::Lerp(7.2f,1.8f,Alpha);
		const FVector Elbow = FDeliveryBoxingPose::SolveElbow(Shoulder, Hand,
			-FVector::UpVector + Right * Side * 0.22f, 30,30);
		TestTrue(TEXT("upper length preserved"), FMath::IsNearlyEqual(float((Elbow-Shoulder).Size()),30.f,0.01f));
		TestTrue(TEXT("forearm length preserved"), FMath::IsNearlyEqual(float((Hand-Elbow).Size()),30.f,0.01f));
		TestTrue(TEXT("elbow stays below shoulder"), Elbow.Z < Shoulder.Z);
		TestTrue(TEXT("hand stays in forward lane"), FMath::Abs(FVector::DotProduct(Hand-Shoulder,Right)) < 0.01f);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDeliveryBoxingAssetTest, "Delivery.Boxing.BossPhysicsAndControls",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FDeliveryBoxingAssetTest::RunTest(const FString&)
{
	USkeletalMesh* Asset = LoadObject<USkeletalMesh>(nullptr,TEXT("/Game/Characters/TestDeliveryMan/The_Boss.The_Boss"));
	if (!TestNotNull(TEXT("Boss mesh"), Asset)) return false;
	const auto Settings = UWorld::InitializationValues().AllowAudioPlayback(false).CreatePhysicsScene(true)
		.ShouldSimulatePhysics(true).CreateNavigation(false).CreateAISystem(false);
	UWorld* World = UWorld::CreateWorld(EWorldType::Game, false, NAME_None, nullptr, true, ERHIFeatureLevel::Num, &Settings);
	GEngine->CreateNewWorldContext(EWorldType::Game).SetCurrentWorld(World);
	AActor* Actor = World->SpawnActor<AActor>();
	USkeletalMeshComponent* Mesh = NewObject<USkeletalMeshComponent>(Actor);
	Actor->SetRootComponent(Mesh);
	Mesh->SetSkeletalMeshAsset(Asset);
	Mesh->SetWorldRotation(FRotator(0,-90,0));
	// Collision has to be on before registering, or the component never creates its bodies.
	Mesh->SetCollisionEnabled(ECollisionEnabled::QueryAndPhysics);
	Mesh->SetEnableGravity(false);
	Mesh->RegisterComponent();
	Mesh->RefreshBoneTransforms();
	UPhysicsAsset* Original = Mesh->GetPhysicsAsset();
	const int32 OriginalCount = Original->SkeletalBodySetups.Num();
	FDeliveryBoxingPose::CompletePhysicsAsset(Mesh);
	UPhysicsAsset* Runtime = Mesh->GetPhysicsAsset();
	TestEqual(TEXT("source asset untouched"), Original->SkeletalBodySetups.Num(), OriginalCount);
	TestTrue(TEXT("left elbow body"), Runtime->FindBodyIndex(TEXT("LeftForeArm")) != INDEX_NONE);
	TestTrue(TEXT("right elbow body"), Runtime->FindBodyIndex(TEXT("RightForeArm")) != INDEX_NONE);
	TestTrue(TEXT("left wrist attached to forearm"), Runtime->FindConstraintIndex(TEXT("LeftHand"),TEXT("LeftForeArm")) != INDEX_NONE);
	TestTrue(TEXT("right wrist attached to forearm"), Runtime->FindConstraintIndex(TEXT("RightHand"),TEXT("RightForeArm")) != INDEX_NONE);
	// The stock shoulder cone is 45 degrees, which is less than the 90 the arm needs to
	// point forward at all. Without this the punch can only ever move the forearm.
	for (const FString Side : { FString(TEXT("Left")), FString(TEXT("Right")) })
	{
		const int32 Index = Runtime->FindConstraintIndex(FName(*(Side + TEXT("Arm"))), TEXT("Spine2"));
		if (!TestTrue(TEXT("shoulder joint found"), Index != INDEX_NONE)) continue;
		const FConstraintInstance& Joint = Runtime->ConstraintSetup[Index]->DefaultInstance;
		TestTrue(TEXT("shoulder can swing past straight ahead"), Joint.GetAngularSwing1Limit() > 90.f);
		TestTrue(TEXT("shoulder can twist"), Joint.GetAngularTwistLimit() > 45.f);
	}
	const int32 RuntimeCount = Runtime->SkeletalBodySetups.Num();
	FDeliveryBoxingPose::CompletePhysicsAsset(Mesh);
	TestEqual(TEXT("repair is idempotent"), Mesh->GetPhysicsAsset()->SkeletalBodySetups.Num(), RuntimeCount);
	UPhysicsControlComponent* Controls = NewObject<UPhysicsControlComponent>(Actor);
	Controls->RegisterComponent();
	FDeliveryBoxingPose Pose;
	Pose.Create(Mesh,Controls);
	TestTrue(TEXT("left arm controls created"), Pose.IsReady(0));
	TestTrue(TEXT("right arm controls created"), Pose.IsReady(1));
	// Only the control targets are checked. This bare world does not step physics
	// (a falling body drops 0.18 cm in a second), so simulated poses prove nothing.
	const auto Step = [&](int32 Count, int32 Side, bool Released)
	{
		for (int32 Frame=0;Frame<Count;++Frame) Pose.Update(Mesh,Controls,0,1.f/60,Side,Released);
	};
	// A target orientation implies a bone direction: take the reference direction into bone space
	// and back out through the target. Chaining both bones gives the fist relative to the shoulder.
	const auto TargetBones = [&](const FDeliveryBoxingPose::FArm& Arm)
	{
		FPhysicsControlTarget Upper, Lower;
		const bool bFound = Controls->GetControlTarget(Arm.UpperControl, Upper)
			&& Controls->GetControlTarget(Arm.LowerControl, Lower);
		TestTrue(TEXT("arm control targets exist"), bFound);
		const auto Direction = [](const FPhysicsControlTarget& Target, const FQuat& Reference, const FVector& Bone)
		{
			return Target.TargetOrientation.Quaternion().RotateVector(Reference.UnrotateVector(Bone.GetSafeNormal()));
		};
		return TPair<FVector, FVector>(
			Direction(Upper, Arm.UpperReference, Arm.UpperDirection),
			Direction(Lower, Arm.LowerReference, Arm.LowerDirection));
	};
	const auto TargetFist = [&](const FDeliveryBoxingPose::FArm& Arm)
	{
		const TPair<FVector, FVector> Bones = TargetBones(Arm);
		return Bones.Key * Arm.UpperLength + Bones.Value * Arm.LowerLength;
	};
	Step(60,-1,false);
	for (int32 Side=0;Side<2;++Side)
	{
		const FDeliveryBoxingPose::FArm& Arm = Pose.Arms[Side];
		const float Length = Arm.UpperLength + Arm.LowerLength;
		const FVector Rest = TargetFist(Arm);
		Step(30,Side,false);
		const FVector Windup = TargetFist(Arm);
		const FVector OtherWindup = TargetFist(Pose.Arms[1-Side]);
		// Sample the whole strike, not just its end. The fist can land in the right spot
		// while getting there sideways past the ribs (a slap) or down from overhead (a chop).
		const float Outward = Side == 0 ? -1.f : 1.f;
		float WorstLateral = 0, WorstBackstep = 0;
		float HighestFist = Windup.Z, LowestFist = Windup.Z;
		float PreviousForward = Windup.X;
		for (int32 Sample=0;Sample<20;++Sample)
		{
			Step(1,Side,true);
			const FVector Fist = TargetFist(Arm);
			WorstLateral = FMath::Max(WorstLateral, Fist.Y * Outward);
			WorstBackstep = FMath::Max(WorstBackstep, PreviousForward - Fist.X);
			HighestFist = FMath::Max(HighestFist, Fist.Z);
			LowestFist = FMath::Min(LowestFist, Fist.Z);
			PreviousForward = Fist.X;
		}
		Step(10,Side,true);
		const FVector Strike = TargetFist(Arm);
		const TPair<FVector, FVector> StrikeBones = TargetBones(Arm);
		Step(60,-1,false);
		const FVector Relaxed = TargetFist(Arm);
		AddInfo(FString::Printf(
			TEXT("Side %d rest=%s windup=%s strike=%s lateral=%.1f backstep=%.2f rise=%.1f upperDot=%.2f"),
			Side,*Rest.ToString(),*Windup.ToString(),*Strike.ToString(),WorstLateral,WorstBackstep,
			HighestFist-LowestFist,FVector::DotProduct(StrikeBones.Key,StrikeBones.Value)));
		TestTrue(TEXT("the fist never swings out past the ribs"), WorstLateral < 0.25f * Length);
		TestTrue(TEXT("the fist only travels forward once released"), WorstBackstep < 0.01f * Length);
		TestTrue(TEXT("the fist drives level instead of chopping down"), HighestFist - LowestFist < 0.2f * Length);
		// The whole point: the upper arm has to end up pointing down the punch, not just the forearm.
		TestTrue(TEXT("the upper arm lines up with the forearm"),
			FVector::DotProduct(StrikeBones.Key, StrikeBones.Value) > 0.9f);
		TestTrue(TEXT("the upper arm points down the punch"), StrikeBones.Key.X > 0.85f);
		TestTrue(TEXT("idle arm hangs well below the shoulder"), Rest.Z < -0.6f * Length);
		TestTrue(TEXT("idle arm stays beside the body"), Rest.X < 0.3f * Length);
		// A guard, not a wind-up behind the back: the fist has to sit on the line the punch
		// will travel, otherwise it can only get to the target by swinging around the shoulder.
		TestTrue(TEXT("windup holds the fist in front of the chest"), Windup.X > 0.25f * Length);
		TestTrue(TEXT("windup keeps the elbow bent"), Windup.Size() < 0.6f * Length);
		TestTrue(TEXT("windup lifts the fist to shoulder height"), Windup.Z > 0);
		TestTrue(TEXT("the other arm keeps hanging"), OtherWindup.Z < -0.6f * Length);
		TestTrue(TEXT("strike sends the whole arm forward"), Strike.X > 0.8f * Length);
		TestTrue(TEXT("strike keeps the fist at shoulder height"), FMath::Abs(Strike.Z) < 0.2f * Length);
		TestTrue(TEXT("the fist is driven half an arm forward"), Strike.X - Windup.X > 0.5f * Length);
		TestTrue(TEXT("fist drops back to the A pose"), Relaxed.Z < Windup.Z - 0.5f * Length);
	}
	Controls->DestroyControlsInSet(TEXT("All"));
	World->DestroyWorld(false);
	GEngine->DestroyWorldContext(World);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDeliveryHandFistTest, "Delivery.Boxing.HandMakesAFist",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FDeliveryHandFistTest::RunTest(const FString&)
{
	USkeletalMesh* Asset = LoadObject<USkeletalMesh>(nullptr,TEXT("/Game/Characters/TestDeliveryMan/The_Boss.The_Boss"));
	if (!TestNotNull(TEXT("Boss mesh"), Asset)) return false;
	const FReferenceSkeleton& Skeleton = Asset->GetRefSkeleton();

	for (const TCHAR* Side : { TEXT("Left"), TEXT("Right") })
	for (const TCHAR* Finger : { TEXT("Thumb"), TEXT("Index"), TEXT("Middle"), TEXT("Ring"), TEXT("Pinky") })
	for (int32 Segment = 1; Segment <= 3; ++Segment)
	{
		const FString Name = FString::Printf(TEXT("%sHand%s%d"), Side, Finger, Segment);
		TestTrue(*FString::Printf(TEXT("skeleton has %s"), *Name), Skeleton.FindBoneIndex(FName(*Name)) != INDEX_NONE);
	}

	const FDeliveryArmPoseSettings Settings;
	TArray<DeliveryHandPose::FCurledBone> Curled;
	DeliveryHandPose::BuildFist(Skeleton, Settings.FingerCurlAngle, Settings.ThumbCurlAngle, Curled);
	// Ten fingers, three segments each. Anything less means a finger was too straight in the
	// reference pose to tell which way the palm is, and got left alone.
	TestEqual(TEXT("every finger segment is curled"), Curled.Num(), 30);

	TArray<FTransform> Local = Skeleton.GetRefBonePose();
	const auto ComponentSpace = [&](const TArray<FTransform>& Pose)
	{
		TArray<FTransform> Out;
		Out.SetNum(Pose.Num());
		for (int32 Bone = 0; Bone < Pose.Num(); ++Bone)
		{
			const int32 Parent = Skeleton.GetParentIndex(Bone);
			Out[Bone] = Parent == INDEX_NONE ? Pose[Bone] : Pose[Bone] * Out[Parent];
		}
		return Out;
	};
	const TArray<FTransform> Before = ComponentSpace(Local);
	for (const DeliveryHandPose::FCurledBone& Bone : Curled)
	{
		Local[Bone.BoneIndex].SetRotation((Bone.Delta * Local[Bone.BoneIndex].GetRotation()).GetNormalized());
	}
	const TArray<FTransform> After = ComponentSpace(Local);

	for (const TCHAR* Side : { TEXT("Left"), TEXT("Right") })
	for (const TCHAR* Finger : { TEXT("Index"), TEXT("Middle"), TEXT("Ring"), TEXT("Pinky") })
	{
		const int32 Wrist = Skeleton.FindBoneIndex(FName(*FString::Printf(TEXT("%sHand"), Side)));
		const int32 Tip = Skeleton.FindBoneIndex(FName(*FString::Printf(TEXT("%sHand%s3"), Side, Finger)));
		const int32 Thumb = Skeleton.FindBoneIndex(FName(*FString::Printf(TEXT("%sHandThumb3"), Side)));
		if (!TestTrue(TEXT("finger bones found"), Wrist != INDEX_NONE && Tip != INDEX_NONE && Thumb != INDEX_NONE)) continue;

		// The thumb sits on the palm side, so closing the hand carries the fingertips towards it.
		// Curling the other way folds the fingers onto the back of the hand and away from it.
		// This asset has one hand with dead straight fingers, so the reference pose itself cannot
		// be used to say which side the palm is on.
		TestTrue(TEXT("the fingertip curls towards the thumb"),
			FVector::Dist(After[Tip].GetLocation(), Before[Thumb].GetLocation())
				< FVector::Dist(Before[Tip].GetLocation(), Before[Thumb].GetLocation()));
		TestTrue(TEXT("the fingertip folds in against the wrist"),
			FVector::Dist(After[Tip].GetLocation(), After[Wrist].GetLocation())
				< FVector::Dist(Before[Tip].GetLocation(), Before[Wrist].GetLocation()) * 0.8f);
	}
	return true;
}
#endif
