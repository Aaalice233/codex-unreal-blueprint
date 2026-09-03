using System.Text.RegularExpressions;
using Newtonsoft.Json.Linq;

/// <summary>
/// Niagara 参数名、参数类型与发射器名的提取。
/// 类型提取为启发式：从 NiagaraScriptVariable 的 Variable 属性树中匹配已知类型关键字，
/// 拿不到时标记 unknown，不阻断主流程。
/// </summary>
static partial class NiagaraCollector
{
    private static readonly Regex NiagaraParameterRegex = new(
        @"\b(?:User|NPC|System|Emitter|Particles)\.[A-Za-z_][A-Za-z0-9_.]*",
        RegexOptions.Compiled
    );

    private static readonly Regex NiagaraEmitterRegex = new(
        @":(NE_[A-Za-z0-9_]+)\.",
        RegexOptions.Compiled
    );

    private static readonly string[] KnownTypeNames =
    [
        "Bool",
        "Int32",
        "Float",
        "Double",
        "Vector2D",
        "Vector3",
        "Vector4",
        "LinearColor",
        "Quat",
        "NiagaraID",
        "Position",
        "String",
        "Texture2D",
        "TextureRenderTarget",
        "MaterialInstanceConstant",
        "SkeletalMesh",
        "StaticMesh",
        "NiagaraDataInterfaceTexture2D",
        "NiagaraDataInterfaceRenderTarget2D",
        "NiagaraDataInterfaceSkeletalMesh",
        "NiagaraDataInterfaceStaticMesh",
        "NiagaraDataInterfaceAudioPlayer",
        "NiagaraDataInterfaceCollisionQuery",
        "NiagaraDataInterfaceNeighborGrid3D",
        "NiagaraDataInterfaceGrid2DCollection",
        "NiagaraDataInterfaceGrid3DCollection",
        "NiagaraDataInterfaceParticleRead",
        "NiagaraDataInterfaceUObjectPropertyReader",
        "NiagaraDataInterfaceUtilities"
    ];

    public static void CollectParameters(
        List<string> target,
        IEnumerable<string> values
    )
    {
        var set = target.ToHashSet(StringComparer.Ordinal);
        foreach (var value in values)
        {
            foreach (Match match in NiagaraParameterRegex.Matches(value))
            {
                if (set.Add(match.Value))
                {
                    target.Add(match.Value);
                }
            }
        }
        target.Sort(StringComparer.Ordinal);
    }

    public static void CollectEmitters(
        List<string> target,
        IEnumerable<string> values
    )
    {
        var set = target.ToHashSet(StringComparer.Ordinal);
        foreach (var value in values)
        {
            foreach (Match match in NiagaraEmitterRegex.Matches(value))
            {
                var emitter = match.Groups[1].Value;
                if (set.Add(emitter))
                {
                    target.Add(emitter);
                }
            }
        }
        target.Sort(StringComparer.Ordinal);
    }

    /// <summary>
    /// 从结构化导出中收集发射器名。
    /// 资产内嵌发射器是 NiagaraEmitter 导出，ObjectName 即发射器名；
    /// 比 `:NE_xxx.` 字符串模式可靠，后者只在发射器恰好按 NE_ 前缀命名时才命中。
    /// </summary>
    public static void CollectEmittersStructured(
        List<string> target,
        JObject? root
    )
    {
        if (root is null)
        {
            return;
        }

        var imports = root["Imports"] as JArray ?? [];
        var exports = root["Exports"] as JArray ?? [];
        var set = target.ToHashSet(StringComparer.Ordinal);
        foreach (var export in exports.OfType<JObject>())
        {
            var className = StructuredDataCollector.ResolveClassName(
                export,
                imports
            );
            if (!string.Equals(
                className,
                "NiagaraEmitter",
                StringComparison.Ordinal
            ))
            {
                continue;
            }

            var name = export.Value<string>("ObjectName");
            if (!string.IsNullOrWhiteSpace(name) && set.Add(name))
            {
                target.Add(name);
            }
        }
        target.Sort(StringComparer.Ordinal);
    }

    /// <summary>从结构化 JSON 的 NiagaraScriptVariable 导出中提取参数类型。</summary>
    public static void CollectParameterTypes(
        Dictionary<string, string> target,
        JObject? root
    )
    {
        if (root is null)
        {
            return;
        }

        var imports = root["Imports"] as JArray ?? [];
        var exports = root["Exports"] as JArray ?? [];
        foreach (var export in exports.OfType<JObject>())
        {
            var className = StructuredDataCollector.ResolveClassName(
                export,
                imports
            );
            if (!string.Equals(
                className,
                "NiagaraScriptVariable",
                StringComparison.Ordinal
            ))
            {
                continue;
            }

            var name = ExtractParameterName(export);
            if (string.IsNullOrEmpty(name) || target.ContainsKey(name))
            {
                continue;
            }

            var type = ExtractVariableType(export, imports, exports);
            target[name] = type ?? "unknown";
        }
    }

    private static string? ExtractParameterName(JObject export)
    {
        var variable = StructuredDataCollector.FindTopLevelProperty(
            export,
            "Variable"
        );
        if (variable is null)
        {
            return null;
        }

        var value = StructuredDataCollector.UnwrapPropertyValue(
            variable["Value"]
        );
        var name = FindStringField(value, "Name");
        if (!string.IsNullOrWhiteSpace(name))
        {
            return name;
        }

        // FName 有时序列化为 { "Index": n, "Value": "User.X" } 或字符串数组
        var container = value as JContainer;
        if (container is null)
        {
            container = new JArray(value ?? JValue.CreateNull());
        }
        foreach (var token in container.DescendantsAndSelf())
        {
            if (token is JValue jValue &&
                jValue.Type == JTokenType.String &&
                jValue.Value<string>() is { } text &&
                text.StartsWith("User.", StringComparison.Ordinal))
            {
                return text;
            }
        }
        return null;
    }

    private static string? ExtractVariableType(
        JObject export,
        JArray imports,
        JArray exports
    )
    {
        var variable = StructuredDataCollector.FindTopLevelProperty(
            export,
            "Variable"
        );
        if (variable is null)
        {
            return null;
        }

        var token = StructuredDataCollector.UnwrapPropertyValue(
            variable["Value"]
        );
        if (token is null)
        {
            return null;
        }

        // 1) 树中字符串直接匹配已知类型名
        var container = token as JContainer ?? new JArray(token);
        foreach (var jValue in container.DescendantsAndSelf().OfType<JValue>())
        {
            if (jValue.Type != JTokenType.String)
            {
                continue;
            }
            var text = jValue.Value<string>();
            if (string.IsNullOrWhiteSpace(text))
            {
                continue;
            }
            var matched = MatchKnownType(text);
            if (matched is not null)
            {
                return matched;
            }
        }

        // 2) 对象引用（FNiagaraTypeDefinition 的 Struct/Object）解析成包路径后匹配
        var searchRoot = variable["Value"] as JContainer ?? container;
        foreach (var property in searchRoot
                     .DescendantsAndSelf()
                     .OfType<JObject>())
        {
            if (property.Value<int?>("Value") is { } index)
            {
                var resolved = StructuredDataCollector.ResolvePackageIndex(
                    index,
                    imports,
                    exports
                );
                var matched = resolved is null
                    ? null
                    : MatchKnownType(resolved);
                if (matched is not null)
                {
                    return matched;
                }
            }
        }

        return null;
    }

    private static string? MatchKnownType(string text)
    {
        var trimmed = text.Trim();
        if (string.IsNullOrEmpty(trimmed))
        {
            return null;
        }
        foreach (var known in KnownTypeNames)
        {
            if (string.Equals(
                    trimmed,
                    known,
                    StringComparison.OrdinalIgnoreCase
                ) ||
                trimmed.EndsWith(
                    "." + known,
                    StringComparison.OrdinalIgnoreCase
                ) ||
                trimmed.EndsWith(
                    ":" + known,
                    StringComparison.OrdinalIgnoreCase
                ))
            {
                return known;
            }
        }
        return null;
    }

    private static string? FindStringField(JToken? token, string fieldName)
    {
        if (token is null)
        {
            return null;
        }
        var container = token as JContainer ?? new JArray(token);
        foreach (var jObject in container.DescendantsAndSelf().OfType<JObject>())
        {
            if (jObject[fieldName] is JValue value &&
                value.Type == JTokenType.String)
            {
                return value.Value<string>();
            }
        }
        return null;
    }
}
