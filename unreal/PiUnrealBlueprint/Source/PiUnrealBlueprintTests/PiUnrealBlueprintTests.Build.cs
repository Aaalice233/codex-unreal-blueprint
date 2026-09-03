using UnrealBuildTool;

public class PiUnrealBlueprintTests : ModuleRules
{
    public PiUnrealBlueprintTests(ReadOnlyTargetRules Target) : base(Target)
    {
        PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;

        PrivateDependencyModuleNames.AddRange(new[]
        {
            "Core",
            "CoreUObject",
            "Engine",
            "Json",
            "AssetRegistry",
            "AssetTools",
            "AnimGraph",
            "BlueprintGraph",
            "Kismet",
            "MovieScene",
            "SlateCore",
            "UMG",
            "UMGEditor",
            "UnrealEd",
            "PiUnrealBlueprintCore",
            "PiUnrealBlueprintCommandlet",
            "PiUnrealBlueprintTransport"
        });
    }
}
