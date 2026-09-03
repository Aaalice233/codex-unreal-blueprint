using UnrealBuildTool;

public class CodexUnrealBlueprintTests : ModuleRules
{
    public CodexUnrealBlueprintTests(ReadOnlyTargetRules Target) : base(Target)
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
            "CodexUnrealBlueprintCore",
            "CodexUnrealBlueprintTransport"
        });
    }
}
