#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS
#include "Speech/Recognition/DeliverySpeechSegmentation.h"

namespace
{
    struct FSpeechSegmentProbe
    {
        FDeliverySpeechSegmentation Segmenter;
        TArray<uint64> Detected;
        TArray<uint64> Finished;
        TMap<uint64, TArray<float>> Audio;
        int32 FrameIndex = 0;
        bool bAudioAfterFinish = false;

        FSpeechSegmentProbe() { Reset(); }
        void Reset()
        {
            FDeliverySpeechSegmentCallbacks Callbacks;
            Callbacks.OnDetected = [this](uint64 Id) { Detected.Add(Id); };
            Callbacks.OnFinished = [this](uint64 Id) { Finished.Add(Id); };
            Callbacks.OnAudio = [this](uint64 Id, int32, TArray<float>&& Samples)
            {
                bAudioAfterFinish |= Finished.Contains(Id);
                Audio.FindOrAdd(Id).Append(Samples);
            };
            Segmenter.Initialize(16000, MoveTemp(Callbacks));
        }
        bool Feed(int32 Frames, float Probability)
        {
            for (int32 Frame = 0; Frame < Frames; ++Frame)
            {
                float Samples[160];
                for (float& Sample : Samples) Sample = static_cast<float>(FrameIndex) / 2000.f;
                ++FrameIndex;
                if (!Segmenter.PushFrame(Samples, 160, Probability)) return false;
            }
            return true;
        }
    };
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDeliverySpeechAutomaticBoundariesTest, "Delivery.Speech.Continuous.Boundaries",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FDeliverySpeechAutomaticBoundariesTest::RunTest(const FString&)
{
    FSpeechSegmentProbe Probe;
    TestTrue(TEXT("静音持续监听"), Probe.Feed(20, .01f));
    TestTrue(TEXT("短噪声候选没有触发轮次"), Probe.Feed(5, .9f));
    TestTrue(TEXT("候选后重新安静"), Probe.Feed(10, .01f));
    TestTrue(TEXT("噪声不会申请交谈或提交音频"), Probe.Detected.IsEmpty() && Probe.Audio.IsEmpty());
    TestTrue(TEXT("稳定人声触发一次"), Probe.Feed(10, .9f));
    TestEqual(TEXT("只有一次起声"), Probe.Detected.Num(), 1);
    if (Probe.Detected.IsEmpty()) return false;
    if (!TestTrue(TEXT("游戏线程为检测段绑定轮次"), Probe.Segmenter.Accept(Probe.Detected[0], 101))) return false;
    const auto& Initial = Probe.Audio.FindChecked(101);
    TestEqual(TEXT("保留250ms预卷"), Initial.Num(), 4000);
    TestEqual(TEXT("预卷从正确样本开始"), Initial[0], 20.f / 2000.f);
    TestEqual(TEXT("起声帧没有丢失或重复"), Initial.Last(), 44.f / 2000.f);
    TestTrue(TEXT("400ms短停顿保持本句"), Probe.Feed(40, .01f));
    TestTrue(TEXT("继续说话清除尾静音计数"), Probe.Feed(10, .9f));
    TestTrue(TEXT("790ms尚未结束"), Probe.Feed(79, .01f));
    TestTrue(TEXT("短暂停顿没有产生final"), Probe.Finished.IsEmpty());
    TestTrue(TEXT("800ms静音自动结束"), Probe.Feed(1, .01f));
    TestEqual(TEXT("结束一次"), Probe.Finished.Num(), 1);
    TestEqual(TEXT("只发送预卷和本轮完整样本"), Probe.Audio.FindChecked(101).Num(), 155 * 160);
    TestFalse(TEXT("结束后没有本轮迟到音频"), Probe.bAudioAfterFinish);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDeliverySpeechAutomaticPendingTest, "Delivery.Speech.Continuous.PendingFinal",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FDeliverySpeechAutomaticPendingTest::RunTest(const FString&)
{
    FSpeechSegmentProbe Probe;
    Probe.Feed(10, .9f);
    if (!TestTrue(TEXT("开始第一句"), Probe.Segmenter.Accept(1, 201))) return false;
    Probe.Feed(80, .01f);
    TestEqual(TEXT("第一句进入final"), Probe.Finished.Num(), 1);
    const int32 FirstCount = Probe.Audio.FindChecked(201).Num();
    // 游戏线程尚未领取第一句final；第二句在此期间完整说完
    TestTrue(TEXT("下一句起声完整缓存"), Probe.Feed(30, .9f));
    TestTrue(TEXT("下一句结尾完整缓存"), Probe.Feed(80, .01f));
    TestEqual(TEXT("新句未提前进入旧识别轮次"), Probe.Audio.Num(), 1);
    TestEqual(TEXT("旧句保持完成且没有被覆盖"), Probe.Audio.FindChecked(201).Num(), FirstCount);
    if (!TestTrue(TEXT("领取旧final后才接收第二句"), Probe.Segmenter.Accept(2, 202))) return false;
    TestEqual(TEXT("第二句起声到结尾完整保留"), Probe.Audio.FindChecked(202).Num(), 110 * 160);
    TestEqual(TEXT("第二句首帧紧接第一句，不重复预卷"), Probe.Audio.FindChecked(202)[0], 90.f / 2000.f);
    TestEqual(TEXT("两句各完成一次"), Probe.Finished.Num(), 2);
    TestFalse(TEXT("已完成检测段不能重复绑定"), Probe.Segmenter.Accept(2, 203));
    TestEqual(TEXT("缓存交出后归零"), Probe.Segmenter.GetPendingSamples(), 0);
    TestFalse(TEXT("没有finish之后的旧音频"), Probe.bAudioAfterFinish);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDeliverySpeechAutomaticLongSegmentTest, "Delivery.Speech.Continuous.LongSegment",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FDeliverySpeechAutomaticLongSegmentTest::RunTest(const FString&)
{
    FSpeechSegmentProbe Probe;
    Probe.Feed(15, .01f);
    Probe.Feed(10, .9f);
    if (!TestTrue(TEXT("绑定包含预卷的长句"), Probe.Segmenter.Accept(1, 301))) return false;
    TestTrue(TEXT("持续说话达到14秒软上限"), Probe.Feed(1375, .9f));
    TestEqual(TEXT("包括预卷恰好14秒，不碰15秒硬限"), Probe.Audio.FindChecked(301).Num(), 14 * 16000);
    TestEqual(TEXT("自动完成长句"), Probe.Finished.Num(), 1);
    TestTrue(TEXT("下一完整帧立即接续，不等待再说100ms"), Probe.Feed(1, .9f));
    TestEqual(TEXT("接续段已检测"), Probe.Detected.Num(), 2);
    if (!TestTrue(TEXT("绑定下一段"), Probe.Segmenter.Accept(2, 302))) return false;
    TestEqual(TEXT("边界前最后样本"), Probe.Audio.FindChecked(301).Last(), 1399.f / 2000.f);
    TestEqual(TEXT("边界后首样本连续且无重复"), Probe.Audio.FindChecked(302)[0], 1400.f / 2000.f);
    Probe.Feed(80, .01f);
    TestEqual(TEXT("接续段可正常结束"), Probe.Finished.Num(), 2);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDeliverySpeechAutomaticCancelAndOverloadTest, "Delivery.Speech.Continuous.CancelAndOverload",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FDeliverySpeechAutomaticCancelAndOverloadTest::RunTest(const FString&)
{
    FSpeechSegmentProbe Probe;
    Probe.Feed(10, .9f);
    if (!TestTrue(TEXT("绑定将取消的一句"), Probe.Segmenter.Accept(1, 401))) return false;
    Probe.Segmenter.Cancel(401);
    Probe.Feed(200, .9f);
    TestEqual(TEXT("同一句被拒绝后不会不断重新申请"), Probe.Detected.Num(), 1);
    TestTrue(TEXT("取消不产生正常final"), Probe.Finished.IsEmpty());
    Probe.Feed(80, .01f);
    Probe.Feed(10, .9f);
    if (!TestTrue(TEXT("停顿后恢复新一句"), Probe.Segmenter.Accept(2, 402))) return false;
    Probe.Segmenter.Cancel(401);
    Probe.Feed(80, .01f);
    TestEqual(TEXT("旧取消不会影响新句完成"), Probe.Finished.Num(), 1);
    TestTrue(TEXT("新句身份正确"), Probe.Finished.Contains(402));

    FSpeechSegmentProbe Backlog;
    TestTrue(TEXT("允许有界2秒待绑定音频"), Backlog.Feed(200, .9f));
    TestFalse(TEXT("超过2秒明确失败，不丢弃旧样本继续"), Backlog.Feed(1, .9f));
    TestEqual(TEXT("缓存没有突破上限"), Backlog.Segmenter.GetPendingSamples(), 32000);
    Backlog.Segmenter.Reset();
    TestEqual(TEXT("退出清理全部待处理音频"), Backlog.Segmenter.GetPendingSamples(), 0);
    return true;
}
#endif
