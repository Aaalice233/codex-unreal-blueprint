using UnrealBuildTool;

public class PiUnrealBlueprintCommandlet : ModuleRules
{
    public PiUnrealBlueprintCommandlet(ReadOnlyTargetRules Target) : base(Target)
    {
        PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;

        PrivateDependencyModuleNames.AddRange(new[]
        {
            "Core",
            "CoreUObject",
            "Engine",
            "Json",
            "PiUnrealBlueprintCore"
        });
    }
}
