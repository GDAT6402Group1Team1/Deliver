#pragma once
#include "CoreMinimal.h"
#include "Blueprint/UserWidget.h"
#include "DeliveryNpcCaptionWidget.generated.h"
class UTextBlock;

// 显示正式 WBP 中的居民字幕，播放进度由表现组件提供
UCLASS()
class DELIVERY_API UDeliveryNpcCaptionWidget : public UUserWidget
{
    GENERATED_BODY()
public:
    void SetCaption(const FText& Text);
protected:
    virtual void NativeConstruct() override;
private:
    UPROPERTY(meta=(BindWidget)) TObjectPtr<UTextBlock> Caption;
    // 字幕可能先于 Slate 构建到达，重建后仍显示最近收到的内容
    FText PendingCaption;
    void ApplyCaption();
};
