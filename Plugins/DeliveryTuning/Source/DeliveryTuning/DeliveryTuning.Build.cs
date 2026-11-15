using UnrealBuildTool;

public class DeliveryTuning : ModuleRules
{
    public DeliveryTuning(ReadOnlyTargetRules Target) : base(Target)
    {
        PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;
        // 各编辑器工具保留独立编译单元，提交后不能因 Unity 合并而重定义文件内辅助函数
        bUseUnity = false;
        PrivateDependencyModuleNames.AddRange(new[] {
            "Core", "CoreUObject", "Engine", "Slate", "SlateCore", "UnrealEd",
            "PropertyEditor", "ToolMenus", "Delivery", "GameplayTags", "Json", "InputCore", "MovieScene", "MovieSceneTracks",
            "NavigationSystem", "AssetTools", "ToolsetRegistry", "EnhancedInput", "AIModule", "SmartObjectsModule", "AIGraph", "BehaviorTreeEditor", "BlueprintGraph", "KismetCompiler", "AssetRegistry", "UMG", "UMGEditor", "AnimGraph", "AnimGraphRuntime", "AnimationBlueprintLibrary"
        });
    }
}
