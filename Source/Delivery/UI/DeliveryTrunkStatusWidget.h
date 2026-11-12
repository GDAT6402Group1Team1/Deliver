#pragma once
#include "CoreMinimal.h"
#include "Blueprint/UserWidget.h"
#include "DeliveryTrunkStatusWidget.generated.h"
class UTextBlock;
class UProgressBar;

// 只显示 HUD 提供的乘员状态，不自行推进恢复或请求出箱
UCLASS(Blueprintable)
class DELIVERY_API UDeliveryTrunkStatusWidget : public UUserWidget
{
    GENERATED_BODY()
public:
    void SetStatus(const FText& Text, float Progress);
protected:
    virtual TSharedRef<SWidget> RebuildWidget() override;
private:
    UPROPERTY(Transient) TObjectPtr<UTextBlock> Label;
    UPROPERTY(Transient) TObjectPtr<UProgressBar> Bar;
};
