#include "UI/Controllers/DeliveryTaskController.h"

#include "Task/Tracking/DeliveryGuidanceLibrary.h"
#include "Phone/DeliveryPhoneCallQueueComponent.h"
#include "Task/Data/DeliveryTaskDefinition.h"
#include "Task/DeliveryTaskManagerComponent.h"
#include "Task/Tracking/DeliveryTaskTrackerComponent.h"
#include "Economy/DeliveryWalletComponent.h"
#include "Engine/World.h"
#include "Player/DeliveryPlayerController.h"
#include "GameFramework/PlayerController.h"
#include "GameFramework/PlayerState.h"
#include "Interaction/DeliveryPromptSubsystem.h"
#include "TimerManager.h"

#define LOCTEXT_NAMESPACE "DeliveryTaskHud"
namespace
{
/** 钱包挂在 PlayerState 上，而 PlayerState 是复制过来的，可能比控制器晚到 */
constexpr float TaskHudBindRetrySeconds = 0.5f;

/** 把秒数排成 4:32；负数（超时）前面加 +，表示已经多用了这么久 */
FText FormatClock(float Seconds)
{
    const bool bOver = Seconds < 0.f;
    const int32 Total = FMath::CeilToInt(FMath::Abs(Seconds));

    return FText::FromString(
        FString::Printf(TEXT("%s%d:%02d"), bOver ? TEXT("+") : TEXT(""), Total / 60, Total % 60));
}

/**
 * 把相对镜头的夹角变成一个箭头
 *
 * 只用八个方向而不是连续旋转的图标：这是 Slate 文本，转不了
 * 而玩家在跑动中其实只需要"大概往哪拐"，八向足够
 */
const TCHAR *ArrowForYaw(float RelativeYaw)
{
    const float Yaw = FRotator::NormalizeAxis(RelativeYaw);

    if (Yaw > -22.5f && Yaw <= 22.5f)
        return TEXT("↑");
    if (Yaw > 22.5f && Yaw <= 67.5f)
        return TEXT("↗");
    if (Yaw > 67.5f && Yaw <= 112.5f)
        return TEXT("→");
    if (Yaw > 112.5f && Yaw <= 157.5f)
        return TEXT("↘");
    if (Yaw > -67.5f && Yaw <= -22.5f)
        return TEXT("↖");
    if (Yaw > -112.5f && Yaw <= -67.5f)
        return TEXT("←");
    if (Yaw > -157.5f && Yaw <= -112.5f)
        return TEXT("↙");

    return TEXT("↓");
}
} // namespace

// 生成结算提示，并记录提示开始显示的时间
void UDeliveryTaskController::HandleTaskPaid(UDeliveryTaskDefinition *Task,
                                             const FDeliveryRewardBreakdown &Reward)
{
    const FText GradeSuffix = Reward.TimeGradeName.IsEmpty()
                                  ? FText::GetEmpty()
                                  : FText::Format(LOCTEXT("GradeSuffix", "（{0}）"), Reward.TimeGradeName);

    CompletionText = FText::Format(LOCTEXT("Delivered", "送达！+{0}{1}"), FText::AsNumber(Reward.FinalReward),
                                   GradeSuffix);
    CompletionShownTime = GetWorld() ? GetWorld()->GetTimeSeconds() : 0.0;
}
// 有来电或通话时生成电话提示，并占用顶部显示
bool UDeliveryTaskController::PushCallState(UDeliveryPhoneCallQueueComponent *Phone)
{
    if (!Phone || !GetWorld())
    {
        return false;
    }

    FDeliveryPhoneCall Call;
    if (!Phone->GetCurrentCall(Call))
    {
        return false;
    }

    const FDeliveryPhoneCallContent Content = UDeliveryPhoneCallQueueComponent::GetCallContent(Call);

    switch (Phone->GetCallState())
    {
    case EDeliveryPhoneCallState::Ringing:
    {
        // 提示使用实际绑定的手机键名，改键后同步更新

        const ADeliveryPlayerController *Owner = Cast<ADeliveryPlayerController>(Player.Get());
        const FText Key = (Owner && Owner->TogglePhoneKey.IsValid()) ? Owner->TogglePhoneKey.GetDisplayName()
                                                                     : LOCTEXT("PhoneKeyFallback", "手机键");
        SetDisplay(FText::Format(LOCTEXT("Ringing", "来电：{0}"), Content.CallerName),
                   FText::Format(LOCTEXT("RingingHint", "按 {0} 接听　（{1} 秒后转未接）"), Key,
                                 FText::AsNumber(FMath::CeilToInt(Phone->GetStateRemainingSeconds()))),
                   /* 紧急提示= */ true);

        return true;
    }

    case EDeliveryPhoneCallState::InCall:
        SetDisplay(FText::Format(LOCTEXT("InCall", "通话中：{0}"), Content.CallerName), FText::GetEmpty(),
                   /* 紧急提示= */ false);

        return true;

    default:
        return false;
    }
}
// 结算提示尚未到期时继续显示，过期后清除
bool UDeliveryTaskController::PushCompletionBanner()
{
    UWorld *World = GetWorld();
    if (!World || CompletionText.IsEmpty())
    {
        return false;
    }

    if (World->GetTimeSeconds() - CompletionShownTime > CompletionBannerSeconds)
    {
        CompletionText = FText::GetEmpty();

        return false;
    }

    SetDisplay(CompletionText, FText::GetEmpty(), /* 紧急提示= */ false);

    return true;
}
// 根据个人追踪任务生成目的地和倒计时提示
bool UDeliveryTaskController::PushTaskState()
{
    UWorld *World = GetWorld();
    const UDeliveryTaskManagerComponent *Manager = UDeliveryTaskManagerComponent::Get(this);
    if (!World || !Manager)
    {
        return false;
    }

    // 从该玩家的跟踪组件取得当前任务，不直接显示全局任务列表的第一项
    const APlayerState *State = Cast<APlayerController>(Player.Get())->PlayerState;
    const UDeliveryTaskTrackerComponent *Tracker =
        State ? State->FindComponentByClass<UDeliveryTaskTrackerComponent>() : nullptr;
    const UDeliveryTaskDefinition *Task = Tracker ? Tracker->GetTrackedTask() : nullptr;
    if (!Task)
    {
        return false;
    }

    const EDeliveryTaskStatus Status = Manager->GetTaskStatus(Task);
    if (Status != EDeliveryTaskStatus::AwaitingPickup && Status != EDeliveryTaskStatus::InProgress)
    {
        return false;
    }

    // 等待取件时提示去取件，配送中则提示送达
    const bool bPickup = Status == EDeliveryTaskStatus::AwaitingPickup;
    const FText TaskTitle = bPickup ? FText::Format(LOCTEXT("GoPickup", "去取件：{0}"), Task->DisplayName)
                                    : FText::Format(LOCTEXT("GoDeliver", "送达：{0}"), Task->DisplayName);

    // 明细行按有什么拼什么：地点组件还没摆时就只显示计时，不让整条消失
    TArray<FString> Parts;

    const FDeliveryGuidance Guidance = UDeliveryGuidanceLibrary::GetTaskGuidance(this, Task);
    if (Guidance.bValid)
    {
        Parts.Add(FString::Printf(TEXT("%s %s"), ArrowForYaw(Guidance.RelativeYaw),
                                  *UDeliveryGuidanceLibrary::FormatDistance(Guidance.Distance).ToString()));
    }

    // 方向和倒计时分别生成，地点缺失时仍可显示剩余时间
    bool bTaskUrgent = false;
    const FDeliveryTaskTimeSnapshot Time = Manager->GetTimeSnapshot(Task);
    if (Time.bRunning)
    {
        Parts.Add(FString::Printf(TEXT("剩余 %s"), *FormatClock(Time.RemainingSeconds).ToString()));
        bTaskUrgent = Time.bOverdue || Time.RemainingSeconds < UrgentRemainingSeconds;
    }

    SetDisplay(TaskTitle, FText::FromString(FString::Join(Parts, TEXT("　·　"))), bTaskUrgent);

    return true;
}

// 连接当前玩家，先解除旧玩家的订阅
void UDeliveryTaskController::Initialize(APlayerController *InPlayer)
{
    Shutdown();
    Player = InPlayer;
    BindToWallet();
}

// 通过所属玩家取得当前世界
UWorld *UDeliveryTaskController::GetWorld() const
{
    return Player.IsValid() ? Player->GetWorld() : nullptr;
}

// 解除钱包订阅并清空本次界面状态
void UDeliveryTaskController::Shutdown()
{
    if (Wallet.IsValid())
        Wallet->OnTaskPaid.RemoveDynamic(this, &UDeliveryTaskController::HandleTaskPaid);
    Wallet.Reset();
    Player.Reset();
    CompletionText = FText::GetEmpty();
    bVisible = false;
}

// 在玩家状态就绪或变化后连接正确的钱包
void UDeliveryTaskController::BindToWallet()
{
    UDeliveryWalletComponent *Current =
        Player.IsValid() ? UDeliveryWalletComponent::FindWallet(Player.Get()) : nullptr;
    if (Wallet.Get() == Current)
        return;
    // 玩家状态更换后先解除旧订阅，避免结算提示重复出现
    if (Wallet.IsValid())
        Wallet->OnTaskPaid.RemoveDynamic(this, &UDeliveryTaskController::HandleTaskPaid);
    Wallet = Current;
    if (Current)
        Current->OnTaskPaid.AddUniqueDynamic(this, &UDeliveryTaskController::HandleTaskPaid);
}

// 一次更新完整的任务显示内容
void UDeliveryTaskController::SetDisplay(const FText &InTitle, const FText &InDetail, bool bInUrgent)
{
    Title = InTitle;
    Detail = InDetail;
    bUrgent = bInUrgent;
    bVisible = true;
}

// 按照来电、结算、任务的顺序选择本次显示内容
void UDeliveryTaskController::Update(float CompletionSeconds, float UrgentSeconds)
{
    // 每次重新判断是否有内容，防止上一条任务提示残留
    bVisible = false;
    if (!Player.IsValid() || !Player->IsLocalController())
        return;
    // 玩家状态可能晚于界面创建，持续检查并绑定当前钱包
    BindToWallet();
    CompletionBannerSeconds = CompletionSeconds;
    UrgentRemainingSeconds = UrgentSeconds;
    // 来电优先，其次显示刚完成的结算，最后显示当前任务
    if (PushCallState(UDeliveryPhoneCallQueueComponent::Get(this)))
        return;
    if (PushCompletionBanner())
        return;
    PushTaskState();
}
#undef LOCTEXT_NAMESPACE
