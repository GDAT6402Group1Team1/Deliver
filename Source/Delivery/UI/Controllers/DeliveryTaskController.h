#pragma once
#include "CoreMinimal.h"
#include "UObject/Object.h"
#include "Task/DeliveryTaskTypes.h"
#include "DeliveryTaskController.generated.h"

class APlayerController;
class UDeliveryWalletComponent;
class UDeliveryTaskDefinition;
class UDeliveryPhoneCallQueueComponent;

/**
 * @brief 顶部任务提示条的界面处理器：按来电、刚完成的结算、当前任务的顺序挑一条显示
 *
 * 任务提示组件每 0.05 秒调用 Update，结果写在 Title、Detail、bUrgent、bVisible 里
 * 订阅钱包的 OnTaskPaid，收到奖励后显示一段时间的结算提示
 */
UCLASS(BlueprintType)
class DELIVERY_API UDeliveryTaskController : public UObject
{
    GENERATED_BODY()
  public:
    /// @brief 绑定本机玩家，先调用 Shutdown 取消旧的订阅
    void Initialize(APlayerController *InPlayer);

    /// @brief 取消钱包订阅，清空显示
    void Shutdown();

    /// @brief 玩家所在的世界
    virtual UWorld *GetWorld() const override;

    /// @brief 重新挑要显示的内容：先清空，有来电显示来电，其次显示没过期的结算提示，最后显示当前任务
    /// @param CompletionSeconds 结算提示显示多久
    /// @param UrgentSeconds 剩余时间少于它时标红
    void Update(float CompletionSeconds, float UrgentSeconds);

    /// 标题
    UPROPERTY(BlueprintReadOnly, Category = "Task") FText Title;

    /// 第二行说明（地点方向、剩余时间等）
    UPROPERTY(BlueprintReadOnly, Category = "Task") FText Detail;

    /// 要标红
    UPROPERTY(BlueprintReadOnly, Category = "Task") bool bUrgent = false;

    /// 有内容要显示
    UPROPERTY(BlueprintReadOnly, Category = "Task") bool bVisible = false;

  private:
    /// @brief 一次写好标题、说明和是否标红，bVisible 设为 true
    void SetDisplay(const FText &InTitle, const FText &InDetail, bool bInUrgent);

    /// @brief 订阅当前 PlayerState 上钱包的 OnTaskPaid；PlayerState 换了先取消旧的订阅
    void BindToWallet();

    /// @brief 收到奖励：记下结算提示文字和显示开始的时间
    UFUNCTION()
    void HandleTaskPaid(UDeliveryTaskDefinition *Task, const FDeliveryRewardBreakdown &Reward);

    /// @brief 响铃或通话中时显示电话提示（带手机键名），返回 true
    bool PushCallState(UDeliveryPhoneCallQueueComponent *Phone);

    /// @brief 结算提示还没到期时显示它，返回 true；过期后清掉
    bool PushCompletionBanner();

    /// @brief 按这个玩家追踪的任务显示：待取件时提示去取件、配送中提示去送货，加上方向和剩余时间
    bool PushTaskState();

    /// 本机玩家
    TWeakObjectPtr<APlayerController> Player;

    /// 订阅了 OnTaskPaid 的钱包
    TWeakObjectPtr<UDeliveryWalletComponent> Wallet;

    /// 结算提示的文字
    FText CompletionText;

    /// 结算提示开始显示的时间
    double CompletionShownTime = -1000.0;

    /// 结算提示显示多久，Update 时更新
    float CompletionBannerSeconds = 4.f;

    /// 剩余多少秒时标红，Update 时更新
    float UrgentRemainingSeconds = 60.f;
};
