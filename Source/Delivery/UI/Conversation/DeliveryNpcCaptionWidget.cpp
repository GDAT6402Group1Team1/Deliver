#include "UI/Conversation/DeliveryNpcCaptionWidget.h"
#include "Components/TextBlock.h"

// 控件树建好后补显示最近收到的字幕
void UDeliveryNpcCaptionWidget::NativeConstruct()
{
    Super::NativeConstruct();
    ApplyCaption();
}

// 文字没变时直接返回，避免每帧重设文本
void UDeliveryNpcCaptionWidget::SetCaption(const FText& Text)
{
    if (PendingCaption.EqualTo(Text))
        return;
    PendingCaption = Text;
    ApplyCaption();
}

// 写入文本控件，空字幕时整块隐藏，有字幕时显示且不拦截鼠标
void UDeliveryNpcCaptionWidget::ApplyCaption()
{
    if (!Caption)
        return;
    Caption->SetText(PendingCaption);
    SetVisibility(PendingCaption.IsEmpty() ? ESlateVisibility::Collapsed : ESlateVisibility::HitTestInvisible);
}
