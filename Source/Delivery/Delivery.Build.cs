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
            "GameplayTasks"
        });

		PrivateDependencyModuleNames.AddRange(new string[] { });
	}
}
