// Copyright Epic Games, Inc. All Rights Reserved.

using UnrealBuildTool;

public class Delivery : ModuleRules
{
	public Delivery(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;
		PublicIncludePaths.Add(ModuleDirectory);

		PublicDependencyModuleNames.AddRange(new string[] {
			"Core",
			"CoreUObject",
			"Engine",
			"InputCore",
			"EnhancedInput",
			"PhysicsControl",
            "GameplayAbilities",
            "GameplayTags",
            "GameplayTasks",
            // 交互浮窗直接用 Slate 画（见 Interaction/DeliveryPromptSubsystem），
            // UMG 只用来做世界坐标 → 屏幕坐标的换算（那一步自带 DPI 缩放处理）。
            "Slate",
            "SlateCore",
            "UMG"
        });

		PrivateDependencyModuleNames.AddRange(new string[] { });
	}
}
