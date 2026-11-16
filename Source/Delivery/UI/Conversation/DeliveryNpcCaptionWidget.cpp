#include "UI/Conversation/DeliveryNpcCaptionWidget.h"
#include "Components/TextBlock.h"

// ── 生命周期 ──

/// @brief 控件树建好后调用 ApplyCaption，显示建好之前收到的字幕
void UDeliveryNpcCaptionWidget::NativeConstruct()
{
    Super::NativeConstruct();
    ApplyCaption();
}

// ── 显示 ──
// 表现组件 TickComponent 每帧调用 SetCaption，SetCaption 再调用 ApplyCaption 写进 Caption 控件

/// @brief 记下这段字并调用 ApplyCaption；和上一次的字一样时直接返回，不每帧重设文本
void UDeliveryNpcCaptionWidget::SetCaption(const FText& Text)
{
    if (PendingCaption.EqualTo(Text))
        return;
    PendingCaption = Text;
    ApplyCaption();
}

/// @brief 把 PendingCaption 写进 Caption 控件；字为空时隐藏整个控件，有字时显示且不挡鼠标点击
void UDeliveryNpcCaptionWidget::ApplyCaption()
{
    if (!Caption)
        return;
    Caption->SetText(PendingCaption);
    SetVisibility(PendingCaption.IsEmpty() ? ESlateVisibility::Collapsed : ESlateVisibility::HitTestInvisible);
}
