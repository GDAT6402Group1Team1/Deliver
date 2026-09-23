// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Subsystems/WorldSubsystem.h"
#include "DeliveryPromptSubsystem.generated.h"

class SWidget;

/** 世界里的"按键提示"浮窗，跟着目标 Actor 的世界坐标贴在屏幕上。
 *
 * 为什么是 C++ Slate 而不是 UMG 资产：提示只有一行字加一个底框，做成 WBP 反而多一份
 * 要手工维护、又不能从脚本可靠生成的资产。这里直接建一个视口 Widget，中文靠 Slate 自带的
 * 字体回退（引擎自带 DroidSansFallback）渲染，不需要额外导字体。
 *
 * 调用约定是"每帧推一次"：PushPrompt 只刷新内容和时间戳，停止推送 PromptTimeout 秒后浮窗
 * 自己消失。这样调用方（探测组件、载具）不需要成对写 Show/Hide，也就不会因为某条退出分支
 * 漏掉 Hide 而把提示永久留在屏幕上。
 */
UCLASS()
class DELIVERY_API UDeliveryPromptSubsystem : public UWorldSubsystem
{
	GENERATED_BODY()

public:

	virtual bool ShouldCreateSubsystem(UObject* Outer) const override;
	virtual void Deinitialize() override;

	static UDeliveryPromptSubsystem* Get(const UObject* WorldContextObject);

	/** 每帧调用一次。WorldAnchor 是浮窗要贴住的世界坐标。 */
	void PushPrompt(const FText& Text, const FVector& WorldAnchor, float HoldProgress = -1.0f);

private:

	void EnsureWidget();
	bool ComputeScreenPosition(FVector2D& OutPosition) const;
	bool IsPromptFresh() const;

	TSharedPtr<SWidget> PromptWidget;
	FText PromptText;
	FVector PromptAnchor = FVector::ZeroVector;
	float PromptHoldProgress = -1.0f;
	double LastPushTime = -1000.0;

	/** 超过这么久没人推送就隐藏。取两三帧的量级，够盖住偶尔掉帧，又不会拖出残留。 */
	static constexpr double PromptTimeout = 0.25;
};
