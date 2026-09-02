using UnrealBuildTool;

public class PiUnrealBlueprintTests : ModuleRules
{
    public PiUnrealBlueprintTests(ReadOnlyTargetRules Target) : base(Target)
    {
        PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;

        PrivateDependencyModuleNames.AddRange(new[]
        {
            "Core",
            "Json",
            "PiUnrealBlueprintCore",
            "PiUnrealBlueprintTransport"
        });
    }
}
