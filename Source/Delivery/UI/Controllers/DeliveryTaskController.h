#pragma once
#include "CoreMinimal.h"
#include "UObject/Object.h"
#include "Task/DeliveryTaskTypes.h"
#include "DeliveryTaskController.generated.h"

class APlayerController;
class UDeliveryWalletComponent;
class UDeliveryTaskDefinition;
class UDeliveryPhoneCallQueueComponent;

/** 选择来电、结算或当前任务的显示内容 */
UCLASS(BlueprintType)
class DELIVERY_API UDeliveryTaskController : public UObject
{
    GENERATED_BODY()
  public:
    void Initialize(APlayerController *InPlayer);
    void Shutdown();
    virtual UWorld *GetWorld() const override;
    void Update(float CompletionSeconds, float UrgentSeconds);
    UPROPERTY(BlueprintReadOnly, Category = "Task") FText Title;
    UPROPERTY(BlueprintReadOnly, Category = "Task") FText Detail;
    UPROPERTY(BlueprintReadOnly, Category = "Task") bool bUrgent = false;
    UPROPERTY(BlueprintReadOnly, Category = "Task") bool bVisible = false;

  private:
    void SetDisplay(const FText &InTitle, const FText &InDetail, bool bInUrgent);
    void BindToWallet();
    UFUNCTION()
    void HandleTaskPaid(UDeliveryTaskDefinition *Task, const FDeliveryRewardBreakdown &Reward);
    bool PushCallState(UDeliveryPhoneCallQueueComponent *Phone);
    bool PushCompletionBanner();
    bool PushTaskState();
    TWeakObjectPtr<APlayerController> Player;
    TWeakObjectPtr<UDeliveryWalletComponent> Wallet;
    FText CompletionText;
    double CompletionShownTime = -1000.0;
    float CompletionBannerSeconds = 4.f;
    float UrgentRemainingSeconds = 60.f;
};
