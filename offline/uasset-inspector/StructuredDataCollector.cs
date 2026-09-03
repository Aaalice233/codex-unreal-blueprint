using System.Globalization;
using Newtonsoft.Json;
using Newtonsoft.Json.Linq;

/// <summary>Imports/Exports 结构化摘要与属性值的通用解析辅助。</summary>
static partial class StructuredDataCollector
{
    public static void Collect(
        JObject root,
        InspectionResult result
    )
    {
        var imports = root["Imports"] as JArray ?? [];
        var exports = root["Exports"] as JArray ?? [];
        foreach (var import in imports.OfType<JObject>())
        {
            result.Imports.Add(new ImportSummary(
                import.Value<string>("ObjectName") ?? "<unnamed>",
                import.Value<string>("ClassPackage") ?? "",
                import.Value<string>("ClassName") ?? "",
                import.Value<int?>("OuterIndex") ?? 0
            ));
        }

        foreach (var export in exports.OfType<JObject>())
        {
            var objectName = export.Value<string>("ObjectName") ?? "<unnamed>";
            var className = ResolveClassName(export, imports);
            var properties = (export["Data"] as JArray ?? [])
                .OfType<JObject>()
                .Select(item => item.Value<string>("Name"))
                .Where(name => !string.IsNullOrWhiteSpace(name))
                .Select(name => name!)
                .Distinct(StringComparer.Ordinal)
                .OrderBy(name => name, StringComparer.Ordinal)
                .ToList();

            result.Exports.Add(new ExportSummary(
                objectName,
                className,
                properties
            ));

            if (string.Equals(
                className,
                "EdGraph",
                StringComparison.Ordinal
            ))
            {
                var nodesProperty = FindTopLevelProperty(export, "Nodes");
                var nodeCount = nodesProperty?["Value"] is JArray nodes
                    ? nodes.Count
                    : 0;
                result.BlueprintGraphs.Add(new BlueprintGraphSummary(
                    objectName,
                    className,
                    ResolvePackageIndex(
                        export.Value<int?>("OuterIndex") ?? 0,
                        imports,
                        exports
                    ) ?? "",
                    nodeCount
                ));
            }

            if (objectName.EndsWith(
                "_GEN_VARIABLE",
                StringComparison.OrdinalIgnoreCase
            ))
            {
                result.BlueprintComponents.Add(new ComponentSummary(
                    objectName,
                    className,
                    properties,
                    FindPropertyValues(export, imports, exports),
                    FindObjectReferences(export, imports, exports),
                    FindParameterBlocks(export)
                ));
            }
        }
    }

    public static JObject? FindTopLevelProperty(
        JObject export,
        string propertyName
    ) => (export["Data"] as JArray ?? [])
        .OfType<JObject>()
        .FirstOrDefault(property => string.Equals(
            property.Value<string>("Name"),
            propertyName,
            StringComparison.Ordinal
        ));

    public static Dictionary<string, string> FindPropertyValues(
        JObject export,
        JArray imports,
        JArray exports
    )
    {
        var values = new Dictionary<string, string>(StringComparer.Ordinal);
        foreach (var property in (export["Data"] as JArray ?? [])
                     .OfType<JObject>())
        {
            var name = property.Value<string>("Name");
            if (string.IsNullOrWhiteSpace(name))
            {
                continue;
            }

            var objectReference = TryResolveObjectProperty(
                property,
                imports,
                exports
            );
            values[name] = objectReference ??
                SummarizeToken(UnwrapPropertyValue(property["Value"]));
        }
        return values;
    }

    public static JToken? UnwrapPropertyValue(JToken? token)
    {
        var current = token;
        for (var depth = 0; depth < 8 && current is not null; depth++)
        {
            if (current is JArray array &&
                array.Count == 1 &&
                array[0] is JObject arrayObject &&
                arrayObject["Value"] is not null)
            {
                current = arrayObject["Value"];
                continue;
            }
            if (current is JObject valueObject &&
                valueObject["Value"] is not null &&
                valueObject.Properties().All(property =>
                    property.Name == "Value" ||
                    property.Name.StartsWith("$", StringComparison.Ordinal) ||
                    property.Name is
                        "Name" or
                        "ArrayIndex" or
                        "PropertyGuid" or
                        "IsZero" or
                        "PropertyTagFlags" or
                        "PropertyTagExtensions" or
                        "PropertyTypeName"))
            {
                current = valueObject["Value"];
                continue;
            }
            break;
        }
        return current;
    }

    public static string SummarizeToken(JToken? token)
    {
        if (token is null || token.Type == JTokenType.Null)
        {
            return "null";
        }
        if (token is JValue value)
        {
            return Convert.ToString(
                value.Value,
                CultureInfo.InvariantCulture
            ) ?? "null";
        }
        if (token is JObject objectValue)
        {
            var useful = new JObject();
            foreach (var property in objectValue.Properties())
            {
                if (property.Name.StartsWith("$", StringComparison.Ordinal))
                {
                    continue;
                }
                useful[property.Name] = property.Value;
            }
            return Truncate(useful.ToString(Formatting.None), 320);
        }
        if (token is JArray array)
        {
            return Truncate(array.ToString(Formatting.None), 320);
        }
        return Truncate(token.ToString(Formatting.None), 320);
    }

    public static string Truncate(string value, int maxLength) =>
        value.Length <= maxLength
            ? value
            : value[..maxLength] + "...";

    public static string? TryResolveObjectProperty(
        JObject property,
        JArray imports,
        JArray exports
    )
    {
        var propertyType = property.Value<string>("$type") ?? "";
        var valueToken = property["Value"];
        if (propertyType.Contains(
                "ObjectPropertyData",
                StringComparison.Ordinal
            ) &&
            valueToken?.Type == JTokenType.Integer)
        {
            return ResolvePackageIndex(
                valueToken.Value<int>(),
                imports,
                exports
            );
        }

        if (!propertyType.Contains(
                "ArrayPropertyData",
                StringComparison.Ordinal
            ) ||
            !string.Equals(
                property.Value<string>("ArrayType"),
                "ObjectProperty",
                StringComparison.Ordinal
            ) ||
            valueToken is not JArray array)
        {
            return null;
        }

        var references = array
            .OfType<JObject>()
            .Select(item => item["Value"])
            .Where(token => token?.Type == JTokenType.Integer)
            .Select(token => ResolvePackageIndex(
                token!.Value<int>(),
                imports,
                exports
            ))
            .Where(reference => !string.IsNullOrWhiteSpace(reference))
            .ToList();
        return references.Count == 0
            ? null
            : "[" + string.Join(", ", references) + "]";
    }

    public static Dictionary<string, string> FindObjectReferences(
        JObject export,
        JArray imports,
        JArray exports
    )
    {
        var references = new Dictionary<string, string>(StringComparer.Ordinal);
        foreach (var property in (export["Data"] as JArray ?? [])
                     .OfType<JObject>())
        {
            var name = property.Value<string>("Name");
            if (string.IsNullOrWhiteSpace(name))
            {
                continue;
            }

            var resolved = TryResolveObjectProperty(
                property,
                imports,
                exports
            );
            if (resolved is not null)
            {
                references[name] = resolved;
            }
        }
        return references;
    }

    public static string? ResolvePackageIndex(
        int packageIndex,
        JArray imports,
        JArray exports
    )
    {
        if (packageIndex < 0)
        {
            return ResolveImportPath(-packageIndex - 1, imports, 0);
        }
        if (packageIndex > 0)
        {
            var exportIndex = packageIndex - 1;
            if (exportIndex >= 0 && exportIndex < exports.Count &&
                exports[exportIndex] is JObject export)
            {
                return export.Value<string>("ObjectName") ??
                    $"Export[{exportIndex}]";
            }
        }
        return null;
    }

    public static string? ResolveImportPath(
        int importIndex,
        JArray imports,
        int depth
    )
    {
        if (depth > 32 ||
            importIndex < 0 ||
            importIndex >= imports.Count ||
            imports[importIndex] is not JObject import)
        {
            return null;
        }

        var name = import.Value<string>("ObjectName") ?? $"Import[{importIndex}]";
        var outerIndex = import.Value<int?>("OuterIndex") ?? 0;
        if (name.StartsWith("/", StringComparison.Ordinal) || outerIndex == 0)
        {
            return name;
        }

        var outer = outerIndex < 0
            ? ResolveImportPath(-outerIndex - 1, imports, depth + 1)
            : null;
        if (string.IsNullOrWhiteSpace(outer))
        {
            return name;
        }
        return outer.StartsWith("/", StringComparison.Ordinal)
            ? $"{outer}.{name}"
            : $"{outer}:{name}";
    }

    public static string ResolveClassName(JObject export, JArray imports)
    {
        var classIndex = export.Value<int?>("ClassIndex") ?? 0;
        if (classIndex < 0)
        {
            var importIndex = -classIndex - 1;
            if (importIndex >= 0 && importIndex < imports.Count &&
                imports[importIndex] is JObject import)
            {
                return import.Value<string>("ObjectName") ??
                    import.Value<string>("ClassName") ??
                    $"Import[{importIndex}]";
            }
        }
        return classIndex == 0 ? "None" : $"ExportIndex({classIndex})";
    }

    public static List<ParameterBlock> FindParameterBlocks(JObject export)
    {
        var blocks = new List<ParameterBlock>();
        foreach (var property in export.DescendantsAndSelf().OfType<JObject>())
        {
            var propertyName = property.Value<string>("Name");
            if (!string.Equals(
                propertyName,
                "ParameterData",
                StringComparison.Ordinal
            ))
            {
                continue;
            }

            if (property["Value"] is not JArray values)
            {
                continue;
            }

            var bytes = new List<byte>();
            foreach (var item in values)
            {
                var number = item.Type == JTokenType.Integer
                    ? item.Value<int?>()
                    : item["Value"]?.Value<int?>();
                if (number is >= byte.MinValue and <= byte.MaxValue)
                {
                    bytes.Add((byte)number.Value);
                }
            }

            if (bytes.Count == 0)
            {
                continue;
            }

            var byteArray = bytes.ToArray();
            var lanes = new List<string>();
            for (var offset = 0; offset + 4 <= bytes.Count; offset += 4)
            {
                var value = BitConverter.ToSingle(byteArray, offset);
                lanes.Add(
                    $"{offset:D3}: {value.ToString("G9", CultureInfo.InvariantCulture)}"
                );
            }

            blocks.Add(new ParameterBlock(
                propertyName!,
                bytes.Count,
                Convert.ToHexString(byteArray),
                lanes
            ));
        }
        return blocks;
    }

    public static ComponentSummary BuildComponentSummary(
        JObject export,
        JArray imports,
        JArray exports
    )
    {
        var objectName = export.Value<string>("ObjectName") ?? "<unnamed>";
        var properties = (export["Data"] as JArray ?? [])
            .OfType<JObject>()
            .Select(item => item.Value<string>("Name"))
            .Where(name => !string.IsNullOrWhiteSpace(name))
            .Select(name => name!)
            .Distinct(StringComparer.Ordinal)
            .OrderBy(name => name, StringComparer.Ordinal)
            .ToList();
        return new ComponentSummary(
            objectName,
            ResolveClassName(export, imports),
            properties,
            FindPropertyValues(export, imports, exports),
            FindObjectReferences(export, imports, exports),
            FindParameterBlocks(export)
        );
    }

    public static List<int> ExtractPackageIndices(JToken? token)
    {
        var values = new List<int>();
        if (token is null)
        {
            return values;
        }
        if (token.Type == JTokenType.Integer)
        {
            values.Add(token.Value<int>());
            return values;
        }
        if (token is JArray array)
        {
            foreach (var item in array)
            {
                if (item.Type == JTokenType.Integer)
                {
                    values.Add(item.Value<int>());
                }
                else if (item is JObject itemObject &&
                         itemObject["Value"]?.Type == JTokenType.Integer)
                {
                    values.Add(itemObject["Value"]!.Value<int>());
                }
            }
        }
        return values;
    }

    public static int ExtractSinglePackageIndex(JToken? token) =>
        ExtractPackageIndices(token).FirstOrDefault();
}
