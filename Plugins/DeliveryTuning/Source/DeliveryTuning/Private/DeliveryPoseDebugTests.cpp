#if WITH_DEV_AUTOMATION_TESTS
#include "SDeliveryPoseDebug.h"
#include "Ragdoll/DeliveryActiveRagdollComponent.h"
#include "Misc/AutomationTest.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDeliveryPoseDebugTest, "Delivery.Debug.PanelLifecycle",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FDeliveryPoseDebugTest::RunTest(const FString&)
{
    UWorld* World = UWorld::CreateWorld(EWorldType::Game, false);
    if (!TestNotNull(TEXT("debug test world"), World)) return false;
    AActor* Actor = World->SpawnActor<AActor>();
    auto* Component = NewObject<UDeliveryActiveRagdollComponent>(Actor);
    TSharedPtr<SDeliveryPoseDebug> Panel = SNew(SDeliveryPoseDebug);
    Panel->Selected = MakeShared<SDeliveryPoseDebug::FTarget>();
    Panel->Selected->Component = Component;
    FDeliveryPoseDebug Pose;
    Panel->DrawPose(World, Pose);
    TestTrue(TEXT("debug owns a registered drawing component"), Panel->Lines.IsValid() && Panel->Lines->IsRegistered());
    const int32 Count = Panel->Lines->BatchedLines.Num();
    TestTrue(TEXT("pose creates visible markers"), Count > 0);
    Panel->DrawPose(World, Pose);
    TestEqual(TEXT("repeated drawing does not accumulate while paused"), Panel->Lines->BatchedLines.Num(), Count);
    // 角色远离原点后，绘制范围也必须移动，不能被视口按旧范围裁掉
    Pose.Hips = FVector(200000.f, 0.f, 0.f);
    Pose.HipTarget = Pose.Hips;
    Pose.Feet[0] = Pose.Feet[1] = Pose.Hips;
    Panel->DrawPose(World, Pose);
    TestTrue(TEXT("drawing bounds follow distant pose"), Panel->Lines->Bounds.GetBox().IsInside(Pose.Hips));
    Panel->Lines->TickComponent(1.f, LEVELTICK_All, nullptr);
    TestEqual(TEXT("markers survive between world updates"), Panel->Lines->BatchedLines.Num(), Count);
    Panel->bEnabled = true;
    Panel->StopDiagnostics();
    TestFalse(TEXT("explicit page switch stops sampling"), Panel->bEnabled);
    Panel->DrawPose(World, Pose);
    TWeakObjectPtr<ULineBatchComponent> Previous = Panel->Lines.Get();
    Panel->ClearDrawing();
    TestFalse(TEXT("turning off releases drawing ownership"), Panel->Lines.IsValid());
    TestTrue(TEXT("old drawing is unregistered"), !Previous.IsValid() || !Previous->IsRegistered());
    Panel->ClearDrawing();
    Panel->DrawPose(World, Pose);
    Panel->OnEndPIE(false);
    TestFalse(TEXT("PIE ending clears hidden panel drawings"), Panel->Lines.IsValid());
    TestFalse(TEXT("PIE ending drops selected actor"), Panel->Selected.IsValid());
    Panel.Reset();
    World->DestroyWorld(false);
    return true;
}
#endif
