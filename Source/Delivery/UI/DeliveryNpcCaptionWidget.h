#pragma once
#include "CoreMinimal.h"
#include "Blueprint/UserWidget.h"
#include "DeliveryNpcCaptionWidget.generated.h"
class UTextBlock;

// 居民头顶字幕使用 UMG 字体回退，避免 TextRender 缺少中文字形
UCLASS()
class DELIVERY_API UDeliveryNpcCaptionWidget : public UUserWidget
{
    GENERATED_BODY()
public:
    void SetCaption(const FText& Text);
protected:
    virtual TSharedRef<SWidget> RebuildWidget() override;
private:
    UPROPERTY() TObjectPtr<UTextBlock> Caption;
};
