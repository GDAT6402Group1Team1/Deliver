#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "Speech/DeliverySpeechTurn.h"
#include <limits>

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDeliverySpeechTurnCompletionTest, "Delivery.Speech.Turn.Completion",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FDeliverySpeechTurnCompletionTest::RunTest(const FString&)
{
    FDeliverySpeechTurnState Turn;
    FDeliverySpeechTurnToken Token, Duplicate;
    FString Text;
    TestFalse(TEXT("未开启时不能录音"), Turn.BeginCapture(0, Token));
    const uint64 Generation = Turn.BeginPreparation();
    TestFalse(TEXT("准备期间不能录音"), Turn.BeginCapture(0, Token));
    TestTrue(TEXT("本次设备准备完成"), Turn.MarkReady(Generation));
    TestTrue(TEXT("开始第一轮"), Turn.BeginCapture(10, Token));
    TestFalse(TEXT("重复按下不能替换当前轮次"), Turn.BeginCapture(10.1, Duplicate));
    TestTrue(TEXT("第一次轮次保持有效"), Turn.IsCurrent(Token));
    TestFalse(TEXT("松键前的文字不能提交"), Turn.TryAcceptFinal(Token, TEXT("打他"), 11, Text));
    TestTrue(TEXT("录音仍在继续"), Turn.GetState() == EDeliverySpeechInputState::Capturing);
    TestTrue(TEXT("松键开始收尾"), Turn.FinishCapture(Token, 12));
    const double Deadline = Turn.GetDeadline();
    TestFalse(TEXT("重复松键不重复收尾"), Turn.FinishCapture(Token, 13));
    TestEqual(TEXT("重复松键不延长期限"), Turn.GetDeadline(), Deadline);
    TestTrue(TEXT("完整改口在松键后一次提交"), Turn.TryAcceptFinal(Token, TEXT("  打他……算了，别打他  "), 12.2, Text));
    TestEqual(TEXT("只去掉首尾空白"), Text, FString(TEXT("打他……算了，别打他")));
    TestFalse(TEXT("成功后令牌立即作废"), Turn.IsCurrent(Token));
    TestFalse(TEXT("重复最终结果不重复发送"), Turn.TryAcceptFinal(Token, TEXT("打他"), 12.3, Text));
    TestTrue(TEXT("拒绝结果不留下可误发文字"), Text.IsEmpty());

    for (const TCHAR* ShortText : { TEXT("好"), TEXT("停"), TEXT("不") })
    {
        TestTrue(TEXT("开始短答"), Turn.BeginCapture(20, Token));
        TestTrue(TEXT("极短按住允许收尾"), Turn.FinishCapture(Token, 20.05));
        TestTrue(TEXT("单字回答有效"), Turn.TryAcceptFinal(Token, ShortText, 20.1, Text));
        TestEqual(TEXT("单字按原样返回"), Text, FString(ShortText));
    }
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDeliverySpeechTurnCancellationTest, "Delivery.Speech.Turn.Cancellation",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FDeliverySpeechTurnCancellationTest::RunTest(const FString&)
{
    FDeliverySpeechTurnState Turn;
    const uint64 FirstGeneration = Turn.BeginPreparation();
    Turn.MarkReady(FirstGeneration);
    FDeliverySpeechTurnToken OldToken, NewToken;
    FString Text;
    Turn.BeginCapture(0, OldToken);
    Turn.FinishCapture(OldToken, 1);
    TestTrue(TEXT("取消收尾中的录音"), Turn.CancelCapture(OldToken));
    TestFalse(TEXT("重复取消不改变状态"), Turn.CancelCapture(OldToken));
    TestTrue(TEXT("取消后可开始下一轮"), Turn.BeginCapture(2, NewToken));
    TestTrue(TEXT("同会话也生成不同轮次编号"), OldToken.Generation == NewToken.Generation && OldToken.Id != NewToken.Id);
    TestFalse(TEXT("旧取消不结束新轮次"), Turn.CancelCapture(OldToken));
    TestFalse(TEXT("旧识别错误不结束新轮次"), Turn.FailTurn(OldToken, EDeliverySpeechTurnFailure::RecognitionFailure));
    TestFalse(TEXT("旧最终文字不提交"), Turn.TryAcceptFinal(OldToken, TEXT("旧话"), 2.1, Text));
    TestTrue(TEXT("新轮次仍有效"), Turn.IsCurrent(NewToken));
    TestTrue(TEXT("旧回调没有覆盖新轮失败原因"), Turn.GetLastFailure() == EDeliverySpeechTurnFailure::None);

    const uint64 SecondGeneration = Turn.BeginPreparation();
    TestTrue(TEXT("更换设备产生新会话代次"), SecondGeneration != FirstGeneration);
    TestFalse(TEXT("旧设备准备完成不能结束新准备"), Turn.MarkReady(FirstGeneration));
    TestFalse(TEXT("旧设备失败不能关闭新设备"), Turn.Fail(FirstGeneration, EDeliverySpeechTurnFailure::AudioFailure));
    TestFalse(TEXT("换设备后旧录音失效"), Turn.IsCurrent(NewToken));
    TestTrue(TEXT("新设备可以完成准备"), Turn.MarkReady(SecondGeneration));
    Turn.BeginCapture(5, NewToken);
    TestTrue(TEXT("当前设备错误进入错误状态"), Turn.FailTurn(NewToken, EDeliverySpeechTurnFailure::AudioFailure));
    TestTrue(TEXT("错误不会自动恢复录音"), Turn.GetState() == EDeliverySpeechInputState::Error);
    TestFalse(TEXT("错误后必须重新准备"), Turn.BeginCapture(6, NewToken));

    const uint64 ThirdGeneration = Turn.BeginPreparation();
    Turn.MarkReady(ThirdGeneration);
    Turn.BeginCapture(7, NewToken);
    Turn.Disable();
    Turn.Disable();
    TestTrue(TEXT("重复关闭保持关闭"), Turn.GetState() == EDeliverySpeechInputState::Off);
    TestFalse(TEXT("关闭后设备回调失效"), Turn.MarkReady(ThirdGeneration));
    TestFalse(TEXT("关闭后识别错误失效"), Turn.Fail(ThirdGeneration, EDeliverySpeechTurnFailure::RecognitionFailure));
    TestFalse(TEXT("关闭后文字不能提交"), Turn.TryAcceptFinal(NewToken, TEXT("关麦后的旧话"), 8, Text));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDeliverySpeechTurnBoundaryTest, "Delivery.Speech.Turn.Boundaries",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FDeliverySpeechTurnBoundaryTest::RunTest(const FString&)
{
    FDeliverySpeechTurnState Turn;
    Turn.MarkReady(Turn.BeginPreparation());
    FDeliverySpeechTurnToken Token;
    FString Text;
    Turn.BeginCapture(100, Token);
    TestTrue(TEXT("上限前仍可录音"), Turn.Tick(114.999) == EDeliverySpeechTurnFailure::None);
    TestFalse(TEXT("恰好到录音上限不把半句话当松键提交"), Turn.FinishCapture(Token, 115));
    TestTrue(TEXT("记录录音超时"), Turn.GetLastFailure() == EDeliverySpeechTurnFailure::CaptureTimeout);
    TestFalse(TEXT("录音超时后的文字不能提交"), Turn.TryAcceptFinal(Token, TEXT("不要"), 115.1, Text));

    Turn.BeginCapture(200, Token);
    TestTrue(TEXT("录音上限之前松键有效"), Turn.FinishCapture(Token, 214.999));
    const double FinalDeadline = Turn.GetDeadline();
    TestTrue(TEXT("收尾期限之前还在等待"), Turn.Tick(FinalDeadline - .001) == EDeliverySpeechTurnFailure::None);
    TestFalse(TEXT("恰好到收尾期限拒绝最终结果"), Turn.TryAcceptFinal(Token, TEXT("停"), FinalDeadline, Text));
    TestTrue(TEXT("记录识别超时"), Turn.GetLastFailure() == EDeliverySpeechTurnFailure::FinalTimeout);
    TestTrue(TEXT("超时后可以重新开始"), Turn.GetState() == EDeliverySpeechInputState::Ready);

    Turn.BeginCapture(300, Token);
    Turn.FinishCapture(Token, 301);
    TestFalse(TEXT("空白结果不发送"), Turn.TryAcceptFinal(Token, TEXT(" \t\r\n "), 302, Text));
    TestTrue(TEXT("记录未识别到文字"), Turn.GetLastFailure() == EDeliverySpeechTurnFailure::EmptyText);
    Turn.BeginCapture(303, Token);
    Turn.FinishCapture(Token, 304);
    TestTrue(TEXT("256字可以提交"), Turn.TryAcceptFinal(Token, FString::ChrN(256, TCHAR('a')), 305, Text));
    TestEqual(TEXT("完整保留256字"), Text.Len(), 256);
    Turn.BeginCapture(306, Token);
    Turn.FinishCapture(Token, 307);
    TestFalse(TEXT("超长结果不截断发送"), Turn.TryAcceptFinal(Token, FString::ChrN(257, TCHAR('a')), 308, Text));
    TestTrue(TEXT("记录过长文字"), Turn.GetLastFailure() == EDeliverySpeechTurnFailure::TextTooLong);
    TestTrue(TEXT("超长输出不留半句"), Text.IsEmpty());

    const double NaN = std::numeric_limits<double>::quiet_NaN();
    TestFalse(TEXT("无效开始时间不创建轮次"), Turn.BeginCapture(NaN, Token));
    Turn.BeginCapture(400, Token);
    Turn.Tick(401);
    TestFalse(TEXT("时间倒退不延长录音"), Turn.FinishCapture(Token, 400.5));
    TestTrue(TEXT("时间倒退终止本轮"), Turn.GetLastFailure() == EDeliverySpeechTurnFailure::InvalidClock);
    Turn.BeginCapture(500, Token);
    TestTrue(TEXT("无效时钟也不能无限录音"), Turn.Tick(NaN) == EDeliverySpeechTurnFailure::InvalidClock);
    TestFalse(TEXT("时钟失效后令牌作废"), Turn.IsCurrent(Token));
    return true;
}

#endif
