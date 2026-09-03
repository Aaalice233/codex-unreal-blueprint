using System.Text;

/// <summary>Markdown 报告渲染（含 Niagara 参数类型与发射器列表）。</summary>
static partial class MarkdownRenderer
{
    public static string Render(InspectionResult result)
    {
        var output = new StringBuilder();
        output.AppendLine("# Unreal UAsset static inspection");
        output.AppendLine();
        output.AppendLine($"- File: `{result.File}`");
        output.AppendLine($"- Size: {result.Size:N0} bytes");
        output.AppendLine($"- Classification: {result.Classification}");
        output.AppendLine(
            $"- Structured parse: {(result.Parsed ? "yes" : "no")}"
        );
        if (result.EngineVersion is not null)
        {
            output.AppendLine($"- Engine version: {result.EngineVersion}");
        }
        if (result.CompanionFiles.Count > 0)
        {
            output.AppendLine(
                "- Companions: " +
                string.Join(", ", result.CompanionFiles.Select(
                    path => $"`{path}`"
                ))
            );
        }
        if (result.ContentRoot is not null)
        {
            output.AppendLine($"- Content root: `{result.ContentRoot}`");
        }

        if (result.ParseErrors.Count > 0 && !result.Parsed)
        {
            output.AppendLine();
            output.AppendLine("## Parse errors");
            output.AppendLine();
            foreach (var error in result.ParseErrors)
            {
                output.AppendLine($"- {EscapeInline(error)}");
            }
        }

        output.AppendLine();
        output.AppendLine("## Blueprint inheritance");
        output.AppendLine();
        if (result.BlueprintInheritance.Count == 0)
        {
            output.AppendLine("_No Blueprint inheritance data found._");
        }
        else
        {
            for (var index = 0;
                 index < result.BlueprintInheritance.Count;
                 index++)
            {
                var layer = result.BlueprintInheritance[index];
                var marker = index == result.BlueprintInheritance.Count - 1
                    ? "└─"
                    : "├─";
                output.AppendLine(
                    $"{marker} `{layer.PackagePath}` " +
                    $"({layer.EngineVersion ?? "unparsed"})"
                );
                if (index == result.BlueprintInheritance.Count - 1 &&
                    layer.ParentReference is not null &&
                    !layer.ParentReference.StartsWith(
                        "/Game/",
                        StringComparison.Ordinal
                    ))
                {
                    output.AppendLine(
                        $"   └─ external parent `{layer.ParentReference}`"
                    );
                }
            }
        }

        output.AppendLine();
        output.AppendLine("## Blueprint component tree");
        output.AppendLine();
        RenderComponentTree(output, result.ComponentTree);

        output.AppendLine();
        output.AppendLine("## UMG widget tree");
        output.AppendLine();
        RenderWidgetTree(output, result.WidgetTree);

        output.AppendLine();
        output.AppendLine("## UMG widget details");
        output.AppendLine();
        if (result.WidgetTree.Count == 0)
        {
            output.AppendLine("_No UMG widget data found._");
        }
        else
        {
            output.AppendLine("| Widget | Class | Variable | Visibility | Text | Slot |");
            output.AppendLine("|---|---|---|---|---|---|");
            foreach (var widget in result.WidgetTree.Take(300))
            {
                output.AppendLine(
                    $"| {EscapeTable(widget.Name)} | " +
                    $"{EscapeTable(widget.ClassName)} | " +
                    $"{(widget.IsVariable ? "yes" : "")} | " +
                    $"{EscapeTable(widget.Visibility ?? "")} | " +
                    $"{EscapeTable(widget.Text ?? "")} | " +
                    $"{EscapeTable(widget.SlotInfo ?? "")} |"
                );
            }
        }

        output.AppendLine();
        output.AppendLine("## Effective component details");
        output.AppendLine();
        if (result.ComponentTree.Count == 0)
        {
            output.AppendLine("_No merged component data found._");
        }
        else
        {
            foreach (var component in result.ComponentTree
                         .OrderBy(item => item.Name, StringComparer.Ordinal))
            {
                output.AppendLine($"### {EscapeInline(component.Name)}");
                output.AppendLine();
                output.AppendLine($"- Class: `{component.ClassName}`");
                output.AppendLine(
                    $"- Parent: `{component.ParentName ?? "<root>"}`"
                );
                if (component.AttachSocket is not null)
                {
                    output.AppendLine(
                        $"- Attach socket: `{component.AttachSocket}`"
                    );
                }
                output.AppendLine($"- Origin: `{component.OriginAsset}`");
                if (component.OverrideAssets.Count > 0)
                {
                    output.AppendLine(
                        "- Serialized overrides: " +
                        string.Join(", ", component.OverrideAssets.Select(
                            value => $"`{value}`"
                        ))
                    );
                }
                if (!string.Equals(
                        component.RelationStatus,
                        "Resolved",
                        StringComparison.Ordinal
                    ))
                {
                    output.AppendLine(
                        $"- Relation status: {component.RelationStatus}"
                    );
                }
                foreach (var property in component.PropertyValues)
                {
                    output.AppendLine(
                        $"- {property.Key}: `{EscapeInline(property.Value)}`"
                    );
                }
                foreach (var block in component.ParameterBlocks)
                {
                    output.AppendLine(
                        $"- {block.PropertyName}: {block.ByteCount} bytes"
                    );
                    output.AppendLine($"  - Hex: `{block.Hex}`");
                    output.AppendLine(
                        "  - Float32 lanes: " +
                        string.Join(", ", block.Float32Lanes.Select(
                            lane => $"`{lane}`"
                        ))
                    );
                }
                output.AppendLine();
            }
        }

        output.AppendLine("## Blueprint graphs");
        output.AppendLine();
        if (result.BlueprintGraphs.Count == 0)
        {
            output.AppendLine("_No Blueprint graphs found._");
        }
        else
        {
            output.AppendLine("| Graph | Owner | Nodes |");
            output.AppendLine("|---|---|---:|");
            foreach (var graph in result.BlueprintGraphs)
            {
                output.AppendLine(
                    $"| {EscapeTable(graph.Name)} | " +
                    $"{EscapeTable(graph.Owner)} | {graph.NodeCount} |"
                );
            }
        }

        output.AppendLine();
        output.AppendLine("## Dependency check");
        output.AppendLine();
        if (result.Dependencies.Count == 0)
        {
            output.AppendLine("_No /Game dependencies found._");
        }
        else
        {
            output.AppendLine("| Package | Status | Local path |");
            output.AppendLine("|---|---|---|");
            foreach (var dependency in result.Dependencies)
            {
                output.AppendLine(
                    $"| {EscapeTable(dependency.PackagePath)} | " +
                    $"{dependency.Status} | " +
                    $"{EscapeTable(dependency.LocalPath ?? "")} |"
                );
            }
        }

        output.AppendLine();
        output.AppendLine("## Unreal references");
        output.AppendLine();
        AppendItems(output, result.References, 200);

        output.AppendLine();
        output.AppendLine("## Niagara emitters");
        output.AppendLine();
        AppendItems(output, result.NiagaraEmitters, 200);

        output.AppendLine();
        output.AppendLine("## Niagara parameters");
        output.AppendLine();
        if (result.NiagaraParameters.Count == 0)
        {
            output.AppendLine("_None found._");
        }
        else
        {
            foreach (var parameter in result.NiagaraParameters
                         .Take(300))
            {
                var type = result.NiagaraParameterTypes
                    .TryGetValue(parameter, out var resolved)
                    ? resolved
                    : null;
                output.AppendLine(
                    type is null || type == "unknown"
                        ? $"- `{parameter}`"
                        : $"- `{parameter}` ({type})"
                );
            }
            if (result.NiagaraParameters.Count > 300)
            {
                output.AppendLine(
                    $"- _Truncated: {result.NiagaraParameters.Count - 300} more_"
                );
            }
        }

        output.AppendLine();
        output.AppendLine("## Blueprint components");
        output.AppendLine();
        if (result.BlueprintComponents.Count == 0)
        {
            output.AppendLine("_No component template exports found._");
        }
        else
        {
            foreach (var component in result.BlueprintComponents)
            {
                output.AppendLine($"### {EscapeInline(component.ObjectName)}");
                output.AppendLine();
                output.AppendLine($"- Class: `{component.ClassName}`");
                output.AppendLine(
                    "- Properties: " +
                    (component.Properties.Count == 0
                        ? "_none_"
                        : string.Join(", ", component.Properties.Select(
                            property => $"`{property}`"
                        )))
                );
                foreach (var property in component.PropertyValues)
                {
                    output.AppendLine(
                        $"- {property.Key}: `{EscapeInline(property.Value)}`"
                    );
                }
                foreach (var reference in component.ObjectReferences)
                {
                    output.AppendLine(
                        $"- {reference.Key}: `{reference.Value}`"
                    );
                }
                foreach (var block in component.ParameterBlocks)
                {
                    output.AppendLine(
                        $"- {block.PropertyName}: {block.ByteCount} bytes"
                    );
                    output.AppendLine($"  - Hex: `{block.Hex}`");
                    output.AppendLine(
                        "  - Float32 lanes: " +
                        string.Join(", ", block.Float32Lanes.Select(
                            lane => $"`{lane}`"
                        ))
                    );
                }
                output.AppendLine();
            }
        }

        output.AppendLine("## Imports");
        output.AppendLine();
        if (result.Imports.Count == 0)
        {
            output.AppendLine("_No structured imports available._");
        }
        else
        {
            output.AppendLine("| Object | Class package | Class | Outer index |");
            output.AppendLine("|---|---|---|---:|");
            foreach (var import in result.Imports.Take(300))
            {
                output.AppendLine(
                    $"| {EscapeTable(import.ObjectName)} | " +
                    $"{EscapeTable(import.ClassPackage)} | " +
                    $"{EscapeTable(import.ClassName)} | " +
                    $"{import.OuterIndex} |"
                );
            }
        }

        output.AppendLine();
        output.AppendLine("## Exports");
        output.AppendLine();
        if (result.Exports.Count == 0)
        {
            output.AppendLine("_No structured exports available._");
        }
        else
        {
            output.AppendLine("| Object | Class | Properties |");
            output.AppendLine("|---|---|---|");
            foreach (var export in result.Exports.Take(300))
            {
                output.AppendLine(
                    $"| {EscapeTable(export.ObjectName)} | " +
                    $"{EscapeTable(export.ClassName)} | " +
                    $"{EscapeTable(string.Join(", ", export.Properties))} |"
                );
            }
        }

        output.AppendLine();
        output.AppendLine("## Search evidence");
        output.AppendLine();
        if (result.SearchMatches.Count == 0)
        {
            output.AppendLine("_No requested terms found._");
        }
        else
        {
            foreach (var match in result.SearchMatches)
            {
                output.AppendLine(
                    $"### {EscapeInline(match.Term)} — {EscapeInline(match.Source)}"
                );
                output.AppendLine();
                output.AppendLine("```text");
                output.AppendLine(match.Context.Replace("```", "'''"));
                output.AppendLine("```");
                output.AppendLine();
            }
        }

        return output.ToString().TrimEnd();
    }

    private static void RenderComponentTree(
        StringBuilder output,
        IReadOnlyList<ComponentTreeNode> components
    )
    {
        if (components.Count == 0)
        {
            output.AppendLine("_No SCS component hierarchy found._");
            return;
        }

        var byName = components.ToDictionary(
            component => component.Name,
            component => component,
            StringComparer.Ordinal
        );
        var children = components
            .Where(component =>
                component.ParentName is not null &&
                byName.ContainsKey(component.ParentName))
            .GroupBy(component => component.ParentName!, StringComparer.Ordinal)
            .ToDictionary(
                group => group.Key,
                group => group
                    .OrderBy(item => item.Name, StringComparer.Ordinal)
                    .ToList(),
                StringComparer.Ordinal
            );
        var roots = components
            .Where(component =>
                component.ParentName is null ||
                !byName.ContainsKey(component.ParentName))
            .OrderBy(component => component.Name, StringComparer.Ordinal)
            .ToList();
        var visited = new HashSet<string>(StringComparer.Ordinal);

        output.AppendLine("```text");
        for (var index = 0; index < roots.Count; index++)
        {
            RenderComponentNode(
                output,
                roots[index],
                children,
                "",
                index == roots.Count - 1,
                visited
            );
        }
        foreach (var component in components
                     .Where(component => !visited.Contains(component.Name))
                     .OrderBy(component => component.Name, StringComparer.Ordinal))
        {
            output.AppendLine(
                $"└─ {component.Name} : {component.ClassName} [cycle/unlinked]"
            );
        }
        output.AppendLine("```");
    }

    private static void RenderWidgetTree(
        StringBuilder output,
        IReadOnlyList<WidgetTreeNode> widgets
    )
    {
        if (widgets.Count == 0)
        {
            output.AppendLine("_No UMG widget hierarchy found._");
            return;
        }

        var byName = widgets.ToDictionary(
            widget => widget.Name,
            widget => widget,
            StringComparer.Ordinal
        );
        var children = widgets
            .Where(widget =>
                widget.ParentName is not null &&
                byName.ContainsKey(widget.ParentName))
            .GroupBy(widget => widget.ParentName!, StringComparer.Ordinal)
            .ToDictionary(
                group => group.Key,
                group => group
                    .OrderBy(item => item.Name, StringComparer.Ordinal)
                    .ToList(),
                StringComparer.Ordinal
            );
        var roots = widgets
            .Where(widget =>
                widget.ParentName is null ||
                !byName.ContainsKey(widget.ParentName))
            .OrderBy(widget => widget.Name, StringComparer.Ordinal)
            .ToList();
        var visited = new HashSet<string>(StringComparer.Ordinal);

        output.AppendLine("```text");
        for (var index = 0; index < roots.Count; index++)
        {
            RenderWidgetNode(
                output,
                roots[index],
                children,
                "",
                index == roots.Count - 1,
                visited
            );
        }
        foreach (var widget in widgets
                     .Where(widget => !visited.Contains(widget.Name))
                     .OrderBy(widget => widget.Name, StringComparer.Ordinal))
        {
            output.AppendLine(
                $"└─ {widget.Name} : {widget.ClassName} [cycle/unlinked]"
            );
        }
        output.AppendLine("```");
    }

    private static void RenderWidgetNode(
        StringBuilder output,
        WidgetTreeNode widget,
        IReadOnlyDictionary<string, List<WidgetTreeNode>> children,
        string prefix,
        bool isLast,
        HashSet<string> visited
    )
    {
        if (!visited.Add(widget.Name))
        {
            output.AppendLine(
                $"{prefix}{(isLast ? "└─" : "├─")} " +
                $"{widget.Name} : {widget.ClassName} [cycle]"
            );
            return;
        }

        var notes = new List<string>();
        if (widget.IsVariable)
        {
            notes.Add("var");
        }
        if (widget.Visibility is not null &&
            !widget.Visibility.Contains("SelfHitTestInvisible", StringComparison.Ordinal) &&
            !widget.Visibility.Contains("Visible", StringComparison.Ordinal))
        {
            notes.Add(widget.Visibility);
        }
        if (widget.Text is not null)
        {
            notes.Add($"text=\"{widget.Text}\"");
        }
        if (widget.SlotInfo is not null)
        {
            notes.Add(widget.SlotInfo);
        }

        output.AppendLine(
            $"{prefix}{(isLast ? "└─" : "├─")} " +
            $"{widget.Name} : {widget.ClassName}" +
            (notes.Count > 0 ? $" [{string.Join("; ", notes)}]" : "")
        );

        if (!children.TryGetValue(widget.Name, out var childList))
        {
            return;
        }
        var childPrefix = prefix + (isLast ? "   " : "│  ");
        for (var index = 0; index < childList.Count; index++)
        {
            RenderWidgetNode(
                output,
                childList[index],
                children,
                childPrefix,
                index == childList.Count - 1,
                visited
            );
        }
    }

    private static void RenderComponentNode(
        StringBuilder output,
        ComponentTreeNode component,
        IReadOnlyDictionary<string, List<ComponentTreeNode>> children,
        string prefix,
        bool isLast,
        HashSet<string> visited
    )
    {
        if (!visited.Add(component.Name))
        {
            output.AppendLine(
                $"{prefix}{(isLast ? "└─" : "├─")} " +
                $"{component.Name} : {component.ClassName} [cycle]"
            );
            return;
        }

        var notes = new List<string> { $"origin={component.OriginAsset}" };
        if (component.AttachSocket is not null)
        {
            notes.Add($"socket={component.AttachSocket}");
        }
        if (component.OverrideAssets.Count > 0)
        {
            notes.Add(
                "overrides=" + string.Join(",", component.OverrideAssets)
            );
        }
        if (!string.Equals(
                component.RelationStatus,
                "Resolved",
                StringComparison.Ordinal
            ))
        {
            notes.Add(component.RelationStatus);
        }

        output.AppendLine(
            $"{prefix}{(isLast ? "└─" : "├─")} " +
            $"{component.Name} : {component.ClassName} " +
            $"[{string.Join("; ", notes)}]"
        );

        if (!children.TryGetValue(component.Name, out var childList))
        {
            return;
        }
        var childPrefix = prefix + (isLast ? "   " : "│  ");
        for (var index = 0; index < childList.Count; index++)
        {
            RenderComponentNode(
                output,
                childList[index],
                children,
                childPrefix,
                index == childList.Count - 1,
                visited
            );
        }
    }

    private static void AppendItems(
        StringBuilder output,
        IReadOnlyList<string> items,
        int limit
    )
    {
        if (items.Count == 0)
        {
            output.AppendLine("_None found._");
            return;
        }
        foreach (var item in items.Take(limit))
        {
            output.AppendLine($"- `{item}`");
        }
        if (items.Count > limit)
        {
            output.AppendLine($"- _Truncated: {items.Count - limit} more_");
        }
    }

    private static string EscapeInline(string value) =>
        value.Replace("`", "'");

    private static string EscapeTable(string value) =>
        value.Replace("|", "\\|").Replace("\r", " ").Replace("\n", " ");
}
