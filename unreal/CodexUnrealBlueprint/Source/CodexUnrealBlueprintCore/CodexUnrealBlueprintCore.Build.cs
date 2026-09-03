using UnrealBuildTool;

public class CodexUnrealBlueprintCore : ModuleRules
{
    public CodexUnrealBlueprintCore(ReadOnlyTargetRules Target) : base(Target)
    {
        PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;

        PublicDependencyModuleNames.AddRange(new[]
        {
            "Core",
            "CoreUObject",
            "Engine",
            "Json"
        });

        PrivateDependencyModuleNames.AddRange(new[]
        {
            "AnimGraph",
            "AnimGraphRuntime",
            "AssetRegistry",
            "AssetTools",
            "BlueprintGraph",
            "KismetCompiler",
            "MovieScene",
            "MovieSceneTracks",
            "Projects",
            "SlateCore",
            "SourceControl",
            "UMG",
            "UMGEditor",
            "UnrealEd"
        });
    }
}
