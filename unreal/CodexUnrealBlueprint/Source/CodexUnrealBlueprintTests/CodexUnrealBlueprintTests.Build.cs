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
            "InputCore",
            "Json",
            "AssetRegistry",
            "AssetTools",
            "AnimGraph",
            "BlueprintGraph",
            "Kismet",
            "MovieScene",
            "Niagara",
            "Slate",
            "SlateCore",
            "UMG",
            "UMGEditor",
            "UnrealEd",
            "CodexUnrealBlueprintCore",
            "CodexUnrealBlueprintTransport"
        });
    }
}
