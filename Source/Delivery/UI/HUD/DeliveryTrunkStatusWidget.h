#pragma once
#include "CoreMinimal.h"
#include "Blueprint/UserWidget.h"
#include "DeliveryTrunkStatusWidget.generated.h"
class UTextBlock;
class UProgressBar;

/**
 * @brief 玩家躺在后备箱里时屏幕下方的状态条：一行文字和一个进度条
 *
 * HUD 的 RefreshTrunkStatus 每 0.1 秒调用 SetStatus；控件只显示，不推进恢复也不请求出箱
 */
UCLASS(Blueprintable)
class DELIVERY_API UDeliveryTrunkStatusWidget : public UUserWidget
{
    GENERATED_BODY()
public:
    /// @brief 显示状态文字和进度（0 到 1）
    void SetStatus(const FText& Text, float Progress);
protected:
    /// @brief 第一次时用代码建好面板、文字和进度条
    virtual TSharedRef<SWidget> RebuildWidget() override;
private:
    UPROPERTY(Transient) TObjectPtr<UTextBlock> Label;
    UPROPERTY(Transient) TObjectPtr<UProgressBar> Bar;
};
