#pragma once

#include "Task/DeliverySettlementWidget.h"
#include "DeliverySettlementTestWidget.generated.h"

/** 自动化测试在内存里构造控件，不修改正式 WBP 的布局或配置 */
UCLASS(Transient, NotBlueprintable)
class UDeliverySettlementTestWidget : public UDeliverySettlementWidget
{
	GENERATED_BODY()
};
