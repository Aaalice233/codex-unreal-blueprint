using UnrealBuildTool;

public class PiUnrealBlueprintCore : ModuleRules
{
    public PiUnrealBlueprintCore(ReadOnlyTargetRules Target) : base(Target)
    {
        PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;

        PublicDependencyModuleNames.AddRange(new[]
        {
            "Core",
            "Json"
        });

        PrivateDependencyModuleNames.AddRange(new[]
        {
            "CoreUObject",
            "Engine",
            "Projects"
        });
    }
}
