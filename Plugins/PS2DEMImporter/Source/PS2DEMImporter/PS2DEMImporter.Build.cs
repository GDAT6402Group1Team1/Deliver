using UnrealBuildTool;

public class PS2DEMImporter : ModuleRules
{
    public PS2DEMImporter(ReadOnlyTargetRules Target) : base(Target)
    {
        PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;

        PublicDependencyModuleNames.AddRange(new[]
        {
            "Core",
            "CoreUObject",
            "Engine",
            "Landscape"
        });

        PrivateDependencyModuleNames.AddRange(new[]
        {
            "ContentBrowser",
            "Slate",
            "SlateCore",
            "UnrealEd"
        });
    }
}
