#include "Ragdoll/DeliveryFootPlacement.h"
#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDeliveryFootPlacementTest,
	"Delivery.Ragdoll.FootSeparation", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FDeliveryFootPlacementTest::RunTest(const FString& Parameters)
{
	for (float Yaw : { -170.0f, -90.0f, 0.0f, 45.0f, 170.0f })
	{
		const FVector BodyRight = FRotationMatrix(FRotator(0.0f, Yaw, 0.0f)).GetUnitAxis(EAxis::Y);
		for (FVector Normal : { FVector::UpVector, FVector(0.5f, 0.0f, 0.866f), FVector(0.0f, 0.5f, 0.866f) })
		{
			Normal.Normalize();
			const FVector Side = DeliveryFootPlacement::SideAxis(Normal, Yaw);
			TestTrue(TEXT("Side axis stays on body's right"), FVector::DotProduct(Side, BodyRight) > 0.8f);
			TestTrue(TEXT("Side axis lies on slope"), FMath::Abs(FVector::DotProduct(Side, Normal)) < 0.001f);
		}
	}
	TestEqual(TEXT("Clear stance needs no correction"), DeliveryFootPlacement::SeparationAcceleration(32, -20, 16), 0.0f);
	TestTrue(TEXT("Crossed stationary foot is pushed outward"), DeliveryFootPlacement::SeparationAcceleration(-8, 0, 16) > 0.0f);
	TestTrue(TEXT("Inward sliding is braked before crossing"), DeliveryFootPlacement::SeparationAcceleration(12, -40, 16) > 0.0f);
	TestEqual(TEXT("Correction remains bounded"), DeliveryFootPlacement::SeparationAcceleration(-100, -100, 16), 1200.0f);
	TestEqual(TEXT("Already escaping foot is not accelerated further"), DeliveryFootPlacement::SeparationAcceleration(12, 100, 16), 0.0f);
	float SmoothAlpha = 0.0f;
	const FVector MidSwing = DeliveryFootPlacement::SwingPosition(FVector::ZeroVector,
		FVector(100, 0, 0), FVector::UpVector, 0.5f, 20.0f, SmoothAlpha);
	TestTrue(TEXT("半步时水平移动到中点并抬脚"),
		FMath::IsNearlyEqual(MidSwing.X, 50.0f) && FMath::IsNearlyEqual(MidSwing.Z, 20.0f));
	TestTrue(TEXT("半步时平滑比例为一半"), FMath::IsNearlyEqual(SmoothAlpha, 0.5f));
	const FVector OnSide = DeliveryFootPlacement::KeepOnSide(FVector(0, -4, 0),
		FVector::ZeroVector, FVector::RightVector, 1.0f, 16.0f);
	TestTrue(TEXT("摆动目标不跨过所属一侧"), FMath::IsNearlyEqual(OnSide.Y, 16.0f));
	const FVector Landing = DeliveryFootPlacement::LandingOnPlane(FVector::ZeroVector,
		FVector::ForwardVector, FVector::RightVector, FVector::ZeroVector,
		1.0f, 100.0f, 0.0f, 32.0f, 16.0f);
	TestTrue(TEXT("落点在前方且仍在本侧"),
		FMath::IsNearlyEqual(Landing.X, 100.0f) && FMath::IsNearlyEqual(Landing.Y, 32.0f));
	return true;
}
#endif
