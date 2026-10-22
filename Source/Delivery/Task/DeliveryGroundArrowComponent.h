#pragma once

#include "CoreMinimal.h"
#include "Blueprint/UserWidget.h"
#include "Components/ActorComponent.h"
#include "DeliveryGroundArrowComponent.generated.h"

class UWidgetComponent;

/** Code-drawn yellow double-chevron; no external image asset required. */
UCLASS(NotBlueprintable)
class DELIVERY_API UDeliveryGroundArrowWidget : public UUserWidget
{
	GENERATED_BODY()

protected:
	virtual TSharedRef<SWidget> RebuildWidget() override;
	virtual void NativeConstruct() override;
	virtual int32 NativePaint(const FPaintArgs& Args, const FGeometry& AllottedGeometry,
		const FSlateRect& MyCullingRect, FSlateWindowElementList& OutDrawElements,
		int32 LayerId, const FWidgetStyle& InWidgetStyle, bool bParentEnabled) const override;
};

/** Local controller-owned guidance, survives possessing a motorbike. */
UCLASS(ClassGroup=(Delivery), meta=(BlueprintSpawnableComponent))
class DELIVERY_API UDeliveryGroundArrowComponent : public UActorComponent
{
	GENERATED_BODY()

public:
	UDeliveryGroundArrowComponent();
	virtual void TickComponent(float DeltaTime, ELevelTick TickType,
		FActorComponentTickFunction* ThisTickFunction) override;
	virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Delivery|Ground Arrow")
	bool bShowGroundArrow = true;

	/** Distance from the player to the arrow's centre, in cm. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Delivery|Ground Arrow", meta=(ClampMin="0", Units="cm"))
	float WalkingRadius = 95.f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Delivery|Ground Arrow", meta=(ClampMin="0", Units="cm"))
	float RidingRadius = 180.f;

	/** 目标在镜头正前方时，箭头额外向目标方向移动的距离；侧面和后方逐渐归零。 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Delivery|Ground Arrow", meta=(ClampMin="0", Units="cm"))
	float ForwardOffset = 50.f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Delivery|Ground Arrow", meta=(ClampMin="10", Units="cm"))
	float ArrowSize = 65.f;

	/** 抬高以避开草、路面与坡面遮挡；仍只按 XY 方向旋转。 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Delivery|Ground Arrow", meta=(ClampMin="1", Units="cm"))
	float GroundClearance = 60.f;

private:
	UPROPERTY(Transient)
	TObjectPtr<UWidgetComponent> Arrow;
};
