using UnrealBuildTool;

public class CodexUnrealBlueprintTransport : ModuleRules
{
    public CodexUnrealBlueprintTransport(ReadOnlyTargetRules Target) : base(Target)
    {
        PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;

        PublicDependencyModuleNames.AddRange(new[]
        {
            "Core",
            "CodexUnrealBlueprintCore"
        });

        PrivateDependencyModuleNames.AddRange(new[]
        {
            "Json",
            "Networking",
            "Sockets"
        });

        if (Target.Platform == UnrealTargetPlatform.Win64)
        {
            PublicSystemLibraries.Add("Advapi32.lib");
        }
    }
}
