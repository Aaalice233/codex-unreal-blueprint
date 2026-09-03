using Newtonsoft.Json;

sealed class InspectionResult
{
    public required string File { get; init; }
    public long Size { get; init; }
    public List<string> CompanionFiles { get; init; } = [];
    public string? ContentRoot { get; set; }
    public bool Parsed { get; set; }
    public string? EngineVersion { get; set; }
    public List<string> ParseErrors { get; init; } = [];
    public string Classification { get; set; } = "Unknown";
    public List<string> References { get; init; } = [];
    public List<string> NiagaraParameters { get; init; } = [];
    public Dictionary<string, string> NiagaraParameterTypes { get; init; } = [];
    public List<string> NiagaraEmitters { get; init; } = [];
    public List<ImportSummary> Imports { get; init; } = [];
    public List<ExportSummary> Exports { get; init; } = [];
    public List<ComponentSummary> BlueprintComponents { get; init; } = [];
    public List<BlueprintLayerSummary> BlueprintInheritance { get; init; } = [];
    public List<ComponentTreeNode> ComponentTree { get; init; } = [];
    public List<WidgetTreeNode> WidgetTree { get; init; } = [];
    public List<BlueprintGraphSummary> BlueprintGraphs { get; init; } = [];
    public List<DependencySummary> Dependencies { get; init; } = [];
    public List<SearchMatch> SearchMatches { get; init; } = [];

    [JsonIgnore]
    public string? RawJson { get; set; }

    public object ForOutput() => new
    {
        File,
        Size,
        CompanionFiles,
        ContentRoot,
        Parsed,
        EngineVersion,
        ParseErrors,
        Classification,
        References,
        NiagaraParameters,
        NiagaraParameterTypes,
        NiagaraEmitters,
        Imports,
        Exports,
        BlueprintComponents,
        BlueprintInheritance,
        ComponentTree,
        WidgetTree,
        BlueprintGraphs,
        Dependencies,
        SearchMatches
    };
}

sealed record ExportSummary(
    string ObjectName,
    string ClassName,
    IReadOnlyList<string> Properties
);

sealed record ImportSummary(
    string ObjectName,
    string ClassPackage,
    string ClassName,
    int OuterIndex
);

sealed record ComponentSummary(
    string ObjectName,
    string ClassName,
    IReadOnlyList<string> Properties,
    IReadOnlyDictionary<string, string> PropertyValues,
    IReadOnlyDictionary<string, string> ObjectReferences,
    IReadOnlyList<ParameterBlock> ParameterBlocks
);

sealed class WidgetTreeNode
{
    public string Name { get; init; } = "";
    public string ClassName { get; init; } = "";
    public string? ParentName { get; set; }
    public bool IsVariable { get; set; }
    public string? Visibility { get; set; }
    public string? Text { get; set; }
    public string? SlotInfo { get; set; }
}

sealed record BlueprintLayerSummary(
    string File,
    string PackagePath,
    string? ParentReference,
    string? EngineVersion
);

sealed class ComponentTreeNode
{
    public string Name { get; init; } = "";
    public string ObjectName { get; set; } = "";
    public string ClassName { get; set; } = "";
    public string? ParentName { get; set; }
    public string? AttachSocket { get; set; }
    public string OriginAsset { get; init; } = "";
    public string RelationStatus { get; set; } = "Resolved";
    public Dictionary<string, string> PropertyValues { get; } = [];
    public Dictionary<string, string> ObjectReferences { get; } = [];
    public List<ParameterBlock> ParameterBlocks { get; set; } = [];
    public List<string> OverrideAssets { get; } = [];
}

sealed record BlueprintGraphSummary(
    string Name,
    string ClassName,
    string Owner,
    int NodeCount
);

sealed record DependencySummary(
    string PackagePath,
    string Status,
    string? LocalPath
);

sealed record ParameterBlock(
    string PropertyName,
    int ByteCount,
    string Hex,
    IReadOnlyList<string> Float32Lanes
);

sealed record SearchMatch(string Source, string Term, string Context);
