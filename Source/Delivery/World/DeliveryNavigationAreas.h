#pragma once

#include "NavAreas/NavArea.h"
#include "NavFilters/NavigationQueryFilter.h"
#include "DeliveryNavigationAreas.generated.h"

// 供策划标记只供人物步行的导航连接，使用普通人物导航代价
UCLASS()
class DELIVERY_API UDeliveryPedestrianLinkArea : public UNavArea
{
    GENERATED_BODY()
};

// 专属车复用人物导航网格，查询时排除标记为仅步行的连接
UCLASS()
class DELIVERY_API UDeliveryCompanionVehicleNavFilter : public UNavigationQueryFilter
{
    GENERATED_BODY()
public:
    UDeliveryCompanionVehicleNavFilter();
};
