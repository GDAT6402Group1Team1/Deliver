#if WITH_DEV_AUTOMATION_TESTS
#include "Misc/AutomationTest.h"
#include "Ragdoll/Movement/DeliveryRagdollRecovery.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDeliveryFootRecoveryTest, "Delivery.Ragdoll.FootRecovery",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FDeliveryFootRecoveryTest::RunTest(const FString&)
{
    FDeliveryRagdollRecovery Recovery;
    auto Observe = [&](float Separation, float Step)
    {
        Recovery.UpdateFeet(Step);
        return Recovery.ObserveFeet(Separation, Step, 3.f, .18f, 16.f, .15f, 2.f);
    };

    // 一次短暂交叉不能触发，回到正常顺序后重新累计
    TestFalse(TEXT("brief crossing is ignored"), Observe(-10.f, .1f));
    TestFalse(TEXT("normal order clears observation"), Observe(20.f, .1f));
    TestFalse(TEXT("separate brief crossings do not accumulate"), Observe(-10.f, .1f));
    TestTrue(TEXT("persistent crossing reports once"), Observe(-10.f, .1f));
    TestTrue(TEXT("crossing pauses walking"), Recovery.ShouldPauseWalking());
    TestFalse(TEXT("duplicate crossing cannot restart recovery"), Recovery.ReportIssue(EDeliveryRecoveryIssue::FeetCrossed));
    Recovery.UpdateFeet(.06f);
    TestTrue(TEXT("duplicate event preserves release progress"), FMath::IsNearlyEqual(Recovery.GetReleaseRemaining(), .06f));

    // 分开一帧不结束，需连续保持安全间距
    Observe(20.f, .1f);
    TestTrue(TEXT("one clear sample is not stable"), Recovery.IsCorrectingFeet());
    Observe(0.f, .1f);
    Observe(20.f, .1f);
    TestTrue(TEXT("stable time must be consecutive"), Recovery.IsCorrectingFeet());
    Observe(20.f, .1f);
    TestFalse(TEXT("stable feet finish correction"), Recovery.ShouldPauseWalking());
    TestFalse(TEXT("retry delay suppresses immediate reentry"), Observe(-10.f, .2f));
    Observe(-10.f, .4f);
    TestTrue(TEXT("persistent fault may retry after cooldown"), Recovery.IsCorrectingFeet());
    Observe(-10.f, 2.1f);
    TestFalse(TEXT("unreachable landing cannot lock movement forever"), Recovery.ShouldPauseWalking());

    // 卡脚释放与交叉检测独立，禁用交叉观察不能取消已有的卡脚保护
    Recovery.ResetFeet();
    Recovery.ReportIssue(EDeliveryRecoveryIssue::FootBlocked, .45f);
    Recovery.CancelFootCorrection();
    TestTrue(TEXT("action interruption preserves blocked step release"), Recovery.IsReleasingFeet());
    Recovery.UpdateFeet(.2f);
    TestFalse(TEXT("blocked duplicate cannot extend release"), Recovery.ReportIssue(EDeliveryRecoveryIssue::FootBlocked));
    Recovery.UpdateFeet(.25f);
    TestFalse(TEXT("blocked release expires"), Recovery.ShouldPauseWalking());
    Recovery.Reset();
    TestFalse(TEXT("reset clears all foot recovery state"), Recovery.ShouldPauseWalking());
    // 战斗只观察刚出过拳的手，恢复不暂停走路，也不因重复报告而续期
    TestFalse(TEXT("idle arm cannot request recovery"), Recovery.ReportIssue(EDeliveryRecoveryIssue::ArmBlocked, .65f, 0));
    Recovery.WatchArm(0);
    TestFalse(TEXT("brief arm error is ignored"), Recovery.ObserveArm(0, true, .2f, .45f, .65f));
    TestFalse(TEXT("normal return clears arm error timer"), Recovery.ObserveArm(0, false, .1f, .45f, .65f));
    TestTrue(TEXT("persistent arm error reports recovery"), Recovery.ObserveArm(0, true, .5f, .45f, .65f));
    TestTrue(TEXT("arm recovery is active"), Recovery.IsRecoveringArms());
    TestFalse(TEXT("arm correction does not pause walking"), Recovery.ShouldPauseWalking());
    TestFalse(TEXT("duplicate arm error cannot renew recovery"), Recovery.ReportIssue(EDeliveryRecoveryIssue::ArmBlocked, .65f, 0));
    Recovery.AdvanceArms(.65f);
    TestFalse(TEXT("arm recovery is bounded"), Recovery.IsRecoveringArms());
    TestFalse(TEXT("one punch cannot retry forever"), Recovery.ObserveArm(0, true, 1.f, .45f, .65f));
    Recovery.WatchArm(1);
    Recovery.ResetArms();
    TestFalse(TEXT("interruption clears arm observation"), Recovery.IsWatchingArm(1));
    return true;
}
#endif
