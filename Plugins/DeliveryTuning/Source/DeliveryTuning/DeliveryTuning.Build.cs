using UnrealBuildTool;

public class DeliveryTuning : ModuleRules
{
    public DeliveryTuning(ReadOnlyTargetRules Target) : base(Target)
    {
        PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;
        PrivateDependencyModuleNames.AddRange(new[] {
            "Core", "CoreUObject", "Engine", "Slate", "SlateCore", "UnrealEd",
            "PropertyEditor", "ToolMenus", "Delivery", "Json"
        });
    }
}
