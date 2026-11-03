// 转换地形道路和河流样条，并维护生成结果的查询与清理接口
#pragma once

#include "CoreMinimal.h"
#include "Kismet/BlueprintFunctionLibrary.h"
#include "PS2DEMLandscapeSplineLibrary.generated.h"

UCLASS()

class PS2DEMIMPORTER_API UPS2DEMLandscapeSplineLibrary : public UBlueprintFunctionLibrary
{
	GENERATED_BODY()

  public:
	// 道路与河流转换
	/** 将选中的导入道路转换为可保存的地形样条 */
	UFUNCTION(BlueprintCallable, Category = "PS2DEM|Landscape")
	static bool ConvertSelectedRoutesToLandscapeSplines(bool bShowConfirmation = true);

	/** 转换选中的河流，同时保留已经生成的道路样条 */
	UFUNCTION(BlueprintCallable, Category = "PS2DEM|Landscape")
	static bool ConvertSelectedRiverRoutesToLandscapeSplines(bool bShowConfirmation = true);

	/** 根据地形样条重建普通主路和支路对象 */
	UFUNCTION(BlueprintCallable, Category = "PS2DEM|Landscape")
	static bool ExportRoadActorSplinesFromLandscape(bool bShowConfirmation = true);

	// 结果查询与维护
	/** 返回用于诊断和自动检查的简要统计信息 */
	UFUNCTION(BlueprintPure, Category = "PS2DEM|Landscape")
	static FString GetGeneratedLandscapeSplineSummary();

	/** 将内容浏览器中选中的静态模型分配给指定路线类型 */
	UFUNCTION(BlueprintCallable, Category = "PS2DEM|Landscape")
	static void AssignSelectedMeshToGeneratedLandscapeSplines(const FString& RouteType);

	/** 仅移除本插件生成的地形样条点和线段 */
	UFUNCTION(BlueprintCallable, Category = "PS2DEM|Landscape")
	static void RemoveGeneratedLandscapeSplines();
};
