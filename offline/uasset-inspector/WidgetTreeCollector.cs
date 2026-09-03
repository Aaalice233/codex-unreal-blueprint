using Newtonsoft.Json.Linq;

/// <summary>
/// UMG WidgetBlueprint 的 WidgetTree 层级收集。
/// 控件父子关系不在控件自身，而在各 PanelSlot 导出的 Content/Parent 引用上，
/// 因此以 Slot 导出为锚点重建整棵控件树。
/// </summary>
static partial class WidgetTreeCollector
{
    public static void Collect(JObject root, InspectionResult result)
    {
        var imports = root["Imports"] as JArray ?? [];
        var exports = root["Exports"] as JArray ?? [];

        var hasWidgetTree = exports.OfType<JObject>().Any(export =>
            string.Equals(
                StructuredDataCollector.ResolveClassName(export, imports),
                "WidgetTree",
                StringComparison.Ordinal
            ));
        if (!hasWidgetTree)
        {
            return;
        }

        // 同名导出可能重复出现（编辑器资产中模板与生成类各持一份），按名字去重
        var exportByName = new Dictionary<string, (int Index, JObject Export)>(
            StringComparer.Ordinal
        );
        for (var index = 0; index < exports.Count; index++)
        {
            if (exports[index] is not JObject export)
            {
                continue;
            }
            var name = export.Value<string>("ObjectName") ?? "";
            if (name.Length > 0 && !exportByName.ContainsKey(name))
            {
                exportByName[name] = (index + 1, export);
            }
        }

        var parentNameByWidgetName = new Dictionary<string, string>(
            StringComparer.Ordinal
        );
        var slotInfoByWidgetName = new Dictionary<string, string>(
            StringComparer.Ordinal
        );
        var widgetNames = new HashSet<string>(StringComparer.Ordinal);

        foreach (var export in exports.OfType<JObject>())
        {
            var className = StructuredDataCollector.ResolveClassName(
                export,
                imports
            );
            if (!className.EndsWith("Slot", StringComparison.Ordinal))
            {
                continue;
            }

            var contentIndex = StructuredDataCollector.ExtractSinglePackageIndex(
                StructuredDataCollector.FindTopLevelProperty(export, "Content")?["Value"]
            );
            if (contentIndex <= 0 ||
                contentIndex > exports.Count ||
                exports[contentIndex - 1] is not JObject contentExport)
            {
                continue;
            }

            var contentName = contentExport.Value<string>("ObjectName") ?? "";
            if (contentName.Length == 0)
            {
                continue;
            }
            widgetNames.Add(contentName);

            var parentIndex = StructuredDataCollector.ExtractSinglePackageIndex(
                StructuredDataCollector.FindTopLevelProperty(export, "Parent")?["Value"]
            );
            if (parentIndex > 0 &&
                parentIndex <= exports.Count &&
                exports[parentIndex - 1] is JObject parentExport)
            {
                var parentName = parentExport.Value<string>("ObjectName");
                if (!string.IsNullOrWhiteSpace(parentName))
                {
                    parentNameByWidgetName[contentName] = parentName;
                    widgetNames.Add(parentName);
                }
            }

            var slotDetails = new List<string>();
            foreach (var property in (export["Data"] as JArray ?? [])
                         .OfType<JObject>())
            {
                var propertyName = property.Value<string>("Name");
                if (propertyName is "Column" or "Row")
                {
                    var value = StructuredDataCollector.SummarizeToken(
                        StructuredDataCollector.UnwrapPropertyValue(
                            property["Value"]
                        )
                    );
                    slotDetails.Add($"{propertyName}={value}");
                }
            }
            if (slotDetails.Count > 0 &&
                !slotInfoByWidgetName.ContainsKey(contentName))
            {
                slotInfoByWidgetName[contentName] = string.Join(
                    ", ",
                    slotDetails
                );
            }
        }

        // 根控件没有 Slot，从 WidgetTree 导出的 RootWidget 引用补上
        foreach (var export in exports.OfType<JObject>())
        {
            if (!string.Equals(
                    StructuredDataCollector.ResolveClassName(export, imports),
                    "WidgetTree",
                    StringComparison.Ordinal
                ))
            {
                continue;
            }
            var rootIndex = StructuredDataCollector.ExtractSinglePackageIndex(
                StructuredDataCollector.FindTopLevelProperty(export, "RootWidget")?["Value"]
            );
            if (rootIndex > 0 &&
                rootIndex <= exports.Count &&
                exports[rootIndex - 1] is JObject rootExport)
            {
                var rootName = rootExport.Value<string>("ObjectName");
                if (!string.IsNullOrWhiteSpace(rootName))
                {
                    widgetNames.Add(rootName);
                }
            }
        }

        foreach (var name in widgetNames.OrderBy(n => n, StringComparer.Ordinal))
        {
            if (!exportByName.TryGetValue(name, out var entry))
            {
                continue;
            }

            var className = StructuredDataCollector.ResolveClassName(
                entry.Export,
                imports
            );
            // 只保留 UMG 控件，过滤 Slot 等辅助导出误入
            parentNameByWidgetName.TryGetValue(name, out var parentName);
            slotInfoByWidgetName.TryGetValue(name, out var slotInfo);

            var isVariable = false;
            var variableProperty = StructuredDataCollector.FindTopLevelProperty(
                entry.Export,
                "bIsVariable"
            );
            if (variableProperty is not null)
            {
                var unwrapped = StructuredDataCollector.UnwrapPropertyValue(
                    variableProperty["Value"]
                );
                isVariable = unwrapped?.Value<bool?>() == true;
            }

            string? visibility = null;
            var visibilityProperty = StructuredDataCollector.FindTopLevelProperty(
                entry.Export,
                "Visibility"
            );
            if (visibilityProperty is not null)
            {
                visibility = StructuredDataCollector.SummarizeToken(
                    StructuredDataCollector.UnwrapPropertyValue(
                        visibilityProperty["Value"]
                    )
                );
            }

            string? text = null;
            var textProperty = StructuredDataCollector.FindTopLevelProperty(
                entry.Export,
                "Text"
            );
            if (textProperty is not null)
            {
                // TextPropertyData 的 Value 只是哈希，真实文本在 CultureInvariantString
                text = textProperty.Value<string>("CultureInvariantString") ??
                    textProperty.Value<string>("SourceString");
                if (string.IsNullOrWhiteSpace(text))
                {
                    text = null;
                }
                else
                {
                    text = StructuredDataCollector.Truncate(text, 60);
                }
            }

            result.WidgetTree.Add(new WidgetTreeNode
            {
                Name = name,
                ClassName = className,
                ParentName = parentName,
                IsVariable = isVariable,
                Visibility = visibility,
                Text = text,
                SlotInfo = slotInfo
            });
        }
    }
}
