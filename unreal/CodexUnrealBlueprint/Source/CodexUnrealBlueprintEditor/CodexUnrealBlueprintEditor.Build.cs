using UnrealBuildTool;

public class CodexUnrealBlueprintEditor : ModuleRules
{
    public CodexUnrealBlueprintEditor(ReadOnlyTargetRules Target) : base(Target)
    {
        PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;

        PrivateDependencyModuleNames.AddRange(new[]
        {
            "Core",
            "CoreUObject",
            "Engine",
            "EditorStyle",
            "LevelEditor",
            "NetCore",
            "CodexUnrealBlueprintCore",
            "RenderCore",
            "Slate",
            "SlateCore",
            "UnrealEd"
        });
    }
}
