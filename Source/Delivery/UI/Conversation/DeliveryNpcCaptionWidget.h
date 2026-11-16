#pragma once
#include "CoreMinimal.h"
#include "Blueprint/UserWidget.h"
#include "DeliveryNpcCaptionWidget.generated.h"
class UTextBlock;

/**
 * @brief NPC 头顶字幕 WBP_NpcCaption 的父类
 *
 * NPC 的表现组件在它头顶创建这个控件，每帧按播放进度把已经说出口的部分交给 SetCaption，
 * 所以字幕会跟着声音一个字一个字出来
 */
UCLASS()
class DELIVERY_API UDeliveryNpcCaptionWidget : public UUserWidget
{
    GENERATED_BODY()
public:
    // ── 显示 ──

    /// @brief 显示这段字；控件还没建好时先存着，建好后补上
    void SetCaption(const FText& Text);
protected:
    // ── 生命周期 ──
    virtual void NativeConstruct() override;
private:
    // ── 显示 ──

    /// 蓝图里名叫 Caption 的文字控件，必须有
    UPROPERTY(meta=(BindWidget)) TObjectPtr<UTextBlock> Caption;

    /// 最近收到的字；控件可能还没建好字幕就到了，建好后再显示它
    FText PendingCaption;

    /// 把存着的字放进 Caption 控件
    void ApplyCaption();
};
