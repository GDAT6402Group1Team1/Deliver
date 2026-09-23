#include "Interaction/DeliveryInteractionGeometry.h"
#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDeliveryPickupFacingTest,
	"Delivery.Interaction.FloorPackageFacing", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FDeliveryPickupFacingTest::RunTest(const FString& Parameters)
{
	const FVector RayStart = FVector::ZeroVector;
	const FVector RayForward = FVector::ForwardVector;
	TestTrue(TEXT("准星旁的小物体能计算出有限偏差"),
		FMath::IsNearlyEqual(DeliveryInteractionGeometry::AimMissDistance(
			RayStart, RayForward, FVector(300.0f, 40.0f, 0.0f)), 40.0f));
	TestTrue(TEXT("镜头后方目标不参与拾取"),
		DeliveryInteractionGeometry::AimMissDistance(
			RayStart, RayForward, FVector(-30.0f, 0.0f, 0.0f)) < 0.0f);
	const FVector FloorPackage(30.0f, 0.0f, -90.0f);
	TestTrue(TEXT("Old pelvis-centred camera cone rejected reachable floor package"),
		FVector::DotProduct(FRotator::ZeroRotator.Vector(), FloorPackage.GetSafeNormal()) < 0.35f);
	for (float Pitch : { -70.0f, -35.0f, 0.0f })
	{
		TestTrue(TEXT("Reachable forward package accepted independent of third-person pitch"),
			DeliveryInteractionGeometry::IsWithinReachFacing(FloorPackage, FRotator(Pitch, 0, 0)));
		TestFalse(TEXT("Package behind player still rejected"),
			DeliveryInteractionGeometry::IsWithinReachFacing(FVector(-60, 0, -90), FRotator(Pitch, 0, 0)));
	}
	TestTrue(TEXT("Package immediately under player has no meaningful yaw"),
		DeliveryInteractionGeometry::IsWithinReachFacing(FVector(0, 0, -90), FRotator::ZeroRotator));
	return true;
}
#endif
