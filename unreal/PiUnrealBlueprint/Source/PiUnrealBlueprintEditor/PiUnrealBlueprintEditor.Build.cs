using UnrealBuildTool;

public class PiUnrealBlueprintEditor : ModuleRules
{
    public PiUnrealBlueprintEditor(ReadOnlyTargetRules Target) : base(Target)
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
            "PiUnrealBlueprintCore",
            "RenderCore",
            "Slate",
            "SlateCore",
            "UnrealEd"
        });
    }
}
