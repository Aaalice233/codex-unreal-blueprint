using UnrealBuildTool;

public class PiUnrealBlueprintTransport : ModuleRules
{
    public PiUnrealBlueprintTransport(ReadOnlyTargetRules Target) : base(Target)
    {
        PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;

        PublicDependencyModuleNames.AddRange(new[]
        {
            "Core",
            "PiUnrealBlueprintCore"
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
