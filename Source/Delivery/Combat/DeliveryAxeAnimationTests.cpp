#include "DeliveryAnimationArmPose.h"
#if WITH_DEV_AUTOMATION_TESTS
#include "DeliveryBoxingPose.h"
#include "Animation/AnimSequence.h"
#include "Components/SkeletalMeshComponent.h"
#include "Engine/Engine.h"
#include "Engine/SkeletalMesh.h"
#include "Engine/World.h"
#include "Misc/AutomationTest.h"
#include "PhysicsControlComponent.h"
#include "DeliveryCharacter.h"
#include "Inventory/DeliveryInventoryComponent.h"
#include "Inventory/DeliveryTestAxe.h"
#include "Ragdoll/DeliveryActiveRagdollComponent.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDeliveryAxeAnimationTest, "Delivery.Axe.AnimationTargets",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FDeliveryAxeAnimationTest::RunTest(const FString&)
{
	UAnimSequence* Clip = LoadObject<UAnimSequence>(nullptr,
		TEXT("/Game/Characters/Animations/Axe/A_AxeDownward.A_AxeDownward"));
	USkeletalMesh* Asset = LoadObject<USkeletalMesh>(nullptr,
		TEXT("/Game/Characters/TestDeliveryMan/The_Boss.The_Boss"));
	if (!TestNotNull(TEXT("imported axe clip"), Clip) || !TestNotNull(TEXT("player mesh"), Asset)) return false;
	TestFalse(TEXT("source root motion disabled"), Clip->bEnableRootMotion);
	TestTrue(TEXT("clip has duration"), Clip->GetPlayLength() > 1.f);
	TestEqual(TEXT("blend starts at zero"), DeliveryAnimationArmPose::BlendWeight(0, 2, .12f, .2f), 0.f);
	TestEqual(TEXT("blend ends at zero"), DeliveryAnimationArmPose::BlendWeight(2, 2, .12f, .2f), 0.f);
	TestEqual(TEXT("middle has full weight"), DeliveryAnimationArmPose::BlendWeight(1, 2, .12f, .2f), 1.f);
	FDeliveryAnimationArmPose Invalid;
	TestFalse(TEXT("missing clip rejected"), DeliveryAnimationArmPose::Sample(nullptr, 0, Invalid));
	const auto Settings = UWorld::InitializationValues().AllowAudioPlayback(false).CreatePhysicsScene(true)
		.ShouldSimulatePhysics(true).CreateNavigation(false).CreateAISystem(false);
	UWorld* World = UWorld::CreateWorld(EWorldType::Game, false, NAME_None, nullptr, true, ERHIFeatureLevel::Num, &Settings);
	GEngine->CreateNewWorldContext(EWorldType::Game).SetCurrentWorld(World);
	AActor* Actor = World->SpawnActor<AActor>();
	USkeletalMeshComponent* Mesh = NewObject<USkeletalMeshComponent>(Actor);
	Actor->SetRootComponent(Mesh);
	Mesh->SetSkeletalMeshAsset(Asset);
	Mesh->SetWorldRotation(FRotator(0,-90,0));
	Mesh->SetCollisionEnabled(ECollisionEnabled::QueryAndPhysics);
	Mesh->SetEnableGravity(false);
	Mesh->RegisterComponent();
	Mesh->RefreshBoneTransforms();
	FDeliveryBoxingPose::CompletePhysicsAsset(Mesh);
	UPhysicsControlComponent* Controls = NewObject<UPhysicsControlComponent>(Actor);
	Controls->RegisterComponent();
	FDeliveryBoxingPose Pose;
	Pose.Create(Mesh, Controls);
	Pose.SettleTime = 1.f;
	Mesh->SetSimulatePhysics(true);
	float MinHeight = 2, MaxHeight = -2;
	for (int32 Frame = 0; Frame <= 76; ++Frame)
	{
		FDeliveryAnimationArmPose Sample;
		const float Time = Clip->GetPlayLength() * Frame / 76.f;
		if (!TestTrue(TEXT("sample source skeleton"), DeliveryAnimationArmPose::Sample(Clip, Time, Sample))) break;
		Sample.Weight = 1;
		const float Height = (Sample.Upper[1].Z + Sample.Lower[1].Z) * .5f;
		MinHeight = FMath::Min(MinHeight, Height);
		MaxHeight = FMath::Max(MaxHeight, Height);
		for (float Yaw : { 0.f, 90.f })
		{
			Pose.Update(Mesh, Controls, Yaw, 1.f/30, -1, false, FVector::ZeroVector, nullptr, 0, &Sample);
			for (int32 Side = 0; Side < 2; ++Side)
			{
				const auto& Arm = Pose.Arms[Side];
				FPhysicsControlTarget Upper, Lower;
				Controls->GetControlTarget(Arm.UpperControl, Upper);
				Controls->GetControlTarget(Arm.LowerControl, Lower);
				const FQuat UpperRotation = Upper.TargetOrientation.Quaternion();
				const FQuat LowerRotation = UpperRotation * Lower.TargetOrientation.Quaternion();
				const FVector UpperDirection = UpperRotation.RotateVector(Arm.UpperReference.UnrotateVector(Arm.UpperDirection.GetSafeNormal()));
				const FVector LowerDirection = LowerRotation.RotateVector(Arm.LowerReference.UnrotateVector(Arm.LowerDirection.GetSafeNormal()));
				const FQuat Facing = FRotator(0,Yaw,0).Quaternion();
				TestTrue(TEXT("upper target matches retargeted animation"), UpperDirection.Equals(Facing.RotateVector(Sample.Upper[Side]), .001));
				TestTrue(TEXT("lower target matches retargeted animation"), LowerDirection.Equals(Facing.RotateVector(Sample.Lower[Side]), .001));
			}
		}
	}
	TestTrue(TEXT("clip raises and lowers weapon arm"), MaxHeight - MinHeight > .5f);
	TestTrue(TEXT("physics stays enabled"), Mesh->IsSimulatingPhysics(TEXT("Hips")));
	AddInfo(FString::Printf(TEXT("Axe duration %.3fs; normalized right-hand height %.3f..%.3f"), Clip->GetPlayLength(), MinHeight, MaxHeight));
	GEngine->DestroyWorldContext(World);
	World->DestroyWorld(false);
	return true;
}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDeliveryAxeInputTest, "Delivery.Axe.HeldItemFlow",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FDeliveryAxeInputTest::RunTest(const FString&)
{
	UClass* CharacterClass = LoadClass<ADeliveryCharacter>(nullptr,
		TEXT("/Game/Blueprint/Character/DeliverMan/BP_DeliveryMan.BP_DeliveryMan_C"));
	for (const TCHAR* AxePath : {
		TEXT("/Game/Blueprint/Item/BP_Axe.BP_Axe_C"),
		TEXT("/Game/Blueprint/Item/Test/BP_TestAxe.BP_TestAxe_C") })
	{
		AddInfo(AxePath);
		UClass* AxeClass = LoadClass<ADeliveryHandheldItem>(nullptr, AxePath);
		if (!TestNotNull(TEXT("player blueprint"), CharacterClass) || !TestNotNull(TEXT("axe blueprint"), AxeClass)) return false;
		const auto Settings = UWorld::InitializationValues().AllowAudioPlayback(false).CreatePhysicsScene(true)
			.ShouldSimulatePhysics(true).CreateNavigation(false).CreateAISystem(false);
		UWorld* World = UWorld::CreateWorld(EWorldType::Game, false, NAME_None, nullptr, true, ERHIFeatureLevel::Num, &Settings);
		GEngine->CreateNewWorldContext(EWorldType::Game).SetCurrentWorld(World);
		ADeliveryCharacter* Character = World->SpawnActor<ADeliveryCharacter>(CharacterClass);
		ADeliveryHandheldItem* Axe = World->SpawnActor<ADeliveryHandheldItem>(AxeClass);
		if (TestNotNull(TEXT("spawn player"), Character) && TestNotNull(TEXT("spawn axe"), Axe))
		{
			auto* Ragdoll = Character->GetActiveRagdoll();
			auto* Inventory = Character->GetInventoryComponent();
			TestNotNull(TEXT("blueprint inherits source animation"), Ragdoll->AxeSwingAnimation.Get());
			Ragdoll->StartRagdoll();
			TestTrue(TEXT("production player uses faster 1.8 playback"), FMath::IsNearlyEqual(Ragdoll->AxeSwingPlayRate, 1.8f));
			ADeliveryHandheldItem* OrdinaryItem = World->SpawnActor<ADeliveryHandheldItem>();
			TestTrue(TEXT("pickup ordinary item"), Inventory->TryPickup(OrdinaryItem));
			Inventory->UseHeldItem();
			TestFalse(TEXT("ordinary item cannot start axe animation"), Ragdoll->SwingAxe.IsValid());
			TestFalse(TEXT("ordinary item rejected by server guard"), Ragdoll->CanStartAxeSwing());
			Inventory->DropHeldItem();
			TestFalse(TEXT("empty hand cannot swing axe"), Ragdoll->CanStartAxeSwing());
			TestTrue(TEXT("pickup axe through inventory"), Inventory->TryPickup(Axe));
			TestTrue(TEXT("equipped axe is ready"), Ragdoll->CanStartAxeSwing());
			Inventory->UseHeldItem();
			TestTrue(TEXT("Use starts authoritative swing"), Ragdoll->SwingAxe.Get() == Axe);
			TestFalse(TEXT("repeated Use cannot restart clip"), Ragdoll->CanStartAxeSwing());
			// Sample midway without relying on physics progression in this isolated automation world.
			Ragdoll->AxeSwingStartTime = 0;
			World->Tick(LEVELTICK_All, .25f);
			TestTrue(TEXT("animation clock advances at 1.8x"), FMath::IsNearlyEqual(Ragdoll->GetAxeSwingTime(), .45f, .001f));
			FDeliveryAnimationArmPose Sample;
			TestTrue(TEXT("held source animation feeds motor pose"), Ragdoll->SampleAxeSwing(Sample));
			Inventory->DropHeldItem();
			TestEqual(TEXT("dropping cancels visual swing immediately"), Ragdoll->GetAxeSwingWeight(), 0.f);
			Ragdoll->TickComponent(.01f, LEVELTICK_All, &Ragdoll->PrimaryComponentTick);
			TestFalse(TEXT("cancelled swing token cleared"), Ragdoll->SwingAxe.IsValid());
			Inventory->TryPickup(Axe);
			TestEqual(TEXT("reequipping cannot resume old clip"), Ragdoll->GetAxeSwingWeight(), 0.f);
			Ragdoll->SetLimp(true);
			TestFalse(TEXT("limp character cannot swing"), Ragdoll->CanStartAxeSwing());
			TestTrue(TEXT("physics remains active after cancellation"), Character->GetMesh()->IsSimulatingPhysics(TEXT("Hips")));
		}
		GEngine->DestroyWorldContext(World);
		World->DestroyWorld(false);
	}
	return true;
}
#endif
