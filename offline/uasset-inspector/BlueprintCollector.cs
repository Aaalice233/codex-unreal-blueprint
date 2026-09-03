using Newtonsoft.Json.Linq;

/// <summary>蓝图继承链与 SCS 组件树的解析（依赖 PackageLoader 的结构化 JSON）。</summary>
static partial class BlueprintCollector
{
    public static void Collect(
        string input,
        ParsedPackage currentPackage,
        string? contentRoot,
        InspectionResult result
    )
    {
        if (currentPackage.Root is null)
        {
            return;
        }

        var layers = new List<BlueprintLayerData>();
        var seenFiles = new HashSet<string>(
            StringComparer.OrdinalIgnoreCase
        );
        var package = currentPackage;

        for (var depth = 0; depth < 32 && package.Root is not null; depth++)
        {
            var fullFile = Path.GetFullPath(package.File);
            if (!seenFiles.Add(fullFile))
            {
                break;
            }

            var parentReference = FindParentReference(package.Root);
            var packagePath = ToPackagePath(fullFile, contentRoot) ?? fullFile;
            layers.Add(new BlueprintLayerData(
                package,
                packagePath,
                parentReference
            ));
            result.BlueprintInheritance.Add(new BlueprintLayerSummary(
                fullFile,
                packagePath,
                parentReference,
                package.EngineVersion
            ));

            var parentFile = ReferenceToLocalAsset(
                parentReference,
                contentRoot
            );
            if (parentFile is null ||
                !File.Exists(parentFile))
            {
                break;
            }

            package = PackageLoader.ParsePackage(parentFile);
        }

        var componentMap = new Dictionary<string, ComponentTreeNode>(
            StringComparer.Ordinal
        );
        foreach (var layer in layers.AsEnumerable().Reverse())
        {
            MergeLayerComponents(layer, componentMap);
        }

        foreach (var component in componentMap.Values)
        {
            if (!string.IsNullOrWhiteSpace(component.ParentName) &&
                !componentMap.ContainsKey(component.ParentName))
            {
                component.RelationStatus =
                    $"Unresolved parent: {component.ParentName}";
            }
            result.ComponentTree.Add(component);
        }
    }

    private static string? FindParentReference(JObject root)
    {
        var imports = root["Imports"] as JArray ?? [];
        var exports = root["Exports"] as JArray ?? [];
        foreach (var export in exports.OfType<JObject>())
        {
            var parentProperty = StructuredDataCollector.FindTopLevelProperty(
                export,
                "ParentClass"
            );
            if (parentProperty is null)
            {
                continue;
            }

            var reference = StructuredDataCollector.TryResolveObjectProperty(
                parentProperty,
                imports,
                exports
            );
            if (!string.IsNullOrWhiteSpace(reference))
            {
                return reference;
            }
        }
        return null;
    }

    public static string? ToPackagePath(
        string file,
        string? contentRoot
    )
    {
        if (string.IsNullOrWhiteSpace(contentRoot))
        {
            return null;
        }

        var relative = Path.GetRelativePath(contentRoot, file);
        if (relative.StartsWith("..", StringComparison.Ordinal))
        {
            return null;
        }
        var withoutExtension = Path.ChangeExtension(relative, null);
        return "/Game/" + withoutExtension.Replace('\\', '/');
    }

    private static string? ReferenceToLocalAsset(
        string? reference,
        string? contentRoot
    )
    {
        if (string.IsNullOrWhiteSpace(reference) ||
            string.IsNullOrWhiteSpace(contentRoot) ||
            !reference.StartsWith("/Game/", StringComparison.Ordinal))
        {
            return null;
        }

        var packagePath = NormalizePackagePath(reference);
        var relative = packagePath["/Game/".Length..]
            .Replace('/', Path.DirectorySeparatorChar);
        return Path.Combine(contentRoot, relative + ".uasset");
    }

    public static string NormalizePackagePath(string reference)
    {
        var dotIndex = reference.IndexOf('.', StringComparison.Ordinal);
        var colonIndex = reference.IndexOf(':', StringComparison.Ordinal);
        var end = new[] { dotIndex, colonIndex }
            .Where(index => index >= 0)
            .DefaultIfEmpty(reference.Length)
            .Min();
        return reference[..end];
    }

    private static void MergeLayerComponents(
        BlueprintLayerData layer,
        Dictionary<string, ComponentTreeNode> componentMap
    )
    {
        var root = layer.Package.Root;
        if (root is null)
        {
            return;
        }

        var imports = root["Imports"] as JArray ?? [];
        var exports = root["Exports"] as JArray ?? [];
        var layerName = Path.GetFileNameWithoutExtension(layer.Package.File);
        var templateExports = exports
            .OfType<JObject>()
            .Where(export => (export.Value<string>("ObjectName") ?? "")
                .EndsWith(
                    "_GEN_VARIABLE",
                    StringComparison.OrdinalIgnoreCase
                ))
            .ToDictionary(
                export => NormalizeComponentName(
                    export.Value<string>("ObjectName") ?? ""
                ),
                export => export,
                StringComparer.Ordinal
            );

        var scsNodes = ParseScsNodes(exports);
        var usedTemplates = new HashSet<string>(StringComparer.Ordinal);
        foreach (var node in scsNodes.Values)
        {
            foreach (var childIndex in node.ChildNodeIndices)
            {
                if (scsNodes.TryGetValue(childIndex, out var child) &&
                    string.IsNullOrWhiteSpace(child.ParentName))
                {
                    child.ParentName = node.VariableName;
                }
            }
        }

        foreach (var node in scsNodes.Values)
        {
            if (node.ComponentTemplateIndex <= 0 ||
                node.ComponentTemplateIndex > exports.Count ||
                exports[node.ComponentTemplateIndex - 1] is not JObject template)
            {
                continue;
            }

            var objectName = template.Value<string>("ObjectName") ?? "";
            var name = !string.IsNullOrWhiteSpace(node.VariableName)
                ? node.VariableName
                : NormalizeComponentName(objectName);
            usedTemplates.Add(NormalizeComponentName(objectName));
            var summary = StructuredDataCollector.BuildComponentSummary(
                template,
                imports,
                exports
            );

            if (!componentMap.TryGetValue(name, out var component))
            {
                component = new ComponentTreeNode
                {
                    Name = name,
                    ObjectName = objectName,
                    ClassName = summary.ClassName,
                    ParentName = NullIfEmpty(node.ParentName),
                    AttachSocket = NullIfEmpty(node.AttachSocket),
                    OriginAsset = layerName
                };
                componentMap[name] = component;
            }
            else
            {
                component.ParentName =
                    NullIfEmpty(node.ParentName) ?? component.ParentName;
                component.AttachSocket =
                    NullIfEmpty(node.AttachSocket) ?? component.AttachSocket;
            }

            ApplyComponentData(component, summary, layerName);
        }

        foreach (var pair in templateExports)
        {
            if (usedTemplates.Contains(pair.Key) ||
                string.Equals(
                    pair.Key,
                    "DefaultSceneRoot",
                    StringComparison.Ordinal
                ))
            {
                continue;
            }

            var summary = StructuredDataCollector.BuildComponentSummary(
                pair.Value,
                imports,
                exports
            );
            if (!componentMap.TryGetValue(pair.Key, out var component))
            {
                component = new ComponentTreeNode
                {
                    Name = pair.Key,
                    ObjectName = summary.ObjectName,
                    ClassName = summary.ClassName,
                    OriginAsset = layerName,
                    RelationStatus =
                        "No SCS relationship in parsed hierarchy"
                };
                componentMap[pair.Key] = component;
            }
            ApplyComponentData(component, summary, layerName);
        }
    }

    private static Dictionary<int, ScsNodeData> ParseScsNodes(JArray exports)
    {
        var allNodeExports = new Dictionary<int, JObject>();
        for (var index = 0; index < exports.Count; index++)
        {
            if (exports[index] is JObject export &&
                (export.Value<string>("ObjectName") ?? "")
                    .StartsWith("SCS_Node", StringComparison.Ordinal))
            {
                allNodeExports[index + 1] = export;
            }
        }

        var selectedIndices = new HashSet<int>();
        var scsExport = exports
            .OfType<JObject>()
            .FirstOrDefault(export =>
                (export.Value<string>("ObjectName") ?? "")
                    .StartsWith(
                        "SimpleConstructionScript",
                        StringComparison.Ordinal
                    ));
        if (scsExport is not null)
        {
            foreach (var propertyName in new[] { "AllNodes", "RootNodes" })
            {
                var property = StructuredDataCollector.FindTopLevelProperty(
                    scsExport,
                    propertyName
                );
                foreach (var nodeIndex in StructuredDataCollector
                             .ExtractPackageIndices(
                                 property?["Value"]
                             ))
                {
                    if (nodeIndex > 0)
                    {
                        selectedIndices.Add(nodeIndex);
                    }
                }
                if (selectedIndices.Count > 0 &&
                    propertyName == "AllNodes")
                {
                    break;
                }
            }
        }

        if (selectedIndices.Count == 0)
        {
            return new Dictionary<int, ScsNodeData>();
        }

        var pending = new Queue<int>(selectedIndices);
        var parsed = new Dictionary<int, ScsNodeData>();
        while (pending.Count > 0)
        {
            var nodeIndex = pending.Dequeue();
            if (parsed.ContainsKey(nodeIndex) ||
                !allNodeExports.TryGetValue(nodeIndex, out var nodeExport))
            {
                continue;
            }

            var node = new ScsNodeData
            {
                ExportIndex = nodeIndex,
                ComponentTemplateIndex = StructuredDataCollector
                    .ExtractSinglePackageIndex(
                        StructuredDataCollector.FindTopLevelProperty(
                            nodeExport,
                            "ComponentTemplate"
                        )?["Value"]
                    ),
                VariableName =
                    ExtractStringProperty(nodeExport, "InternalVariableName") ??
                    ExtractStringProperty(nodeExport, "SCSVariableName") ??
                    "",
                ParentName = ExtractStringProperty(
                    nodeExport,
                    "ParentComponentOrVariableName"
                ),
                AttachSocket = ExtractStringProperty(
                    nodeExport,
                    "AttachToName"
                )
            };
            node.ChildNodeIndices.AddRange(StructuredDataCollector
                .ExtractPackageIndices(
                    StructuredDataCollector.FindTopLevelProperty(
                        nodeExport,
                        "ChildNodes"
                    )?["Value"]
                ).Where(index => index > 0));
            parsed[nodeIndex] = node;
            foreach (var childIndex in node.ChildNodeIndices)
            {
                pending.Enqueue(childIndex);
            }
        }
        return parsed;
    }

    private static string? ExtractStringProperty(
        JObject export,
        string propertyName
    )
    {
        var token = StructuredDataCollector.FindTopLevelProperty(
            export,
            propertyName
        )?["Value"];
        return token?.Type == JTokenType.String
            ? token.Value<string>()
            : null;
    }

    private static string NormalizeComponentName(string objectName) =>
        objectName.EndsWith(
            "_GEN_VARIABLE",
            StringComparison.OrdinalIgnoreCase
        )
            ? objectName[..^"_GEN_VARIABLE".Length]
            : objectName;

    private static string? NullIfEmpty(string? value) =>
        string.IsNullOrWhiteSpace(value) ||
        string.Equals(value, "None", StringComparison.Ordinal)
            ? null
            : value;

    private static void ApplyComponentData(
        ComponentTreeNode component,
        ComponentSummary summary,
        string layerName
    )
    {
        var hasSerializedOverride =
            summary.PropertyValues.Count > 0 ||
            summary.ObjectReferences.Count > 0 ||
            summary.ParameterBlocks.Count > 0;
        component.ObjectName = summary.ObjectName;
        component.ClassName = summary.ClassName;
        foreach (var property in summary.PropertyValues)
        {
            component.PropertyValues[property.Key] = property.Value;
        }
        foreach (var reference in summary.ObjectReferences)
        {
            component.ObjectReferences[reference.Key] = reference.Value;
        }
        if (summary.ParameterBlocks.Count > 0)
        {
            component.ParameterBlocks = summary.ParameterBlocks.ToList();
        }
        if (!string.Equals(
                component.OriginAsset,
                layerName,
                StringComparison.Ordinal
            ) &&
            hasSerializedOverride &&
            !component.OverrideAssets.Contains(
                layerName,
                StringComparer.Ordinal
            ))
        {
            component.OverrideAssets.Add(layerName);
        }
    }
}

sealed record BlueprintLayerData(
    ParsedPackage Package,
    string PackagePath,
    string? ParentReference
);

sealed class ScsNodeData
{
    public int ExportIndex { get; init; }
    public int ComponentTemplateIndex { get; init; }
    public string VariableName { get; init; } = "";
    public string? ParentName { get; set; }
    public string? AttachSocket { get; init; }
    public List<int> ChildNodeIndices { get; } = [];
}
