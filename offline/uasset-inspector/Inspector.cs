using System.Text.RegularExpressions;

/// <summary>解析主流程：组合各收集器，产出 InspectionResult。</summary>
static partial class Inspector
{
    private static readonly Regex UnrealPathRegex = new(
        @"(?:/Game|/Engine|/Script)/[A-Za-z0-9_./:\-]+",
        RegexOptions.Compiled
    );

    public static InspectionResult Inspect(
        string input,
        IReadOnlyList<string> searchTerms,
        string? explicitContentRoot
    )
    {
        var companionFiles = PackageLoader.FindCompanions(input);
        var result = new InspectionResult
        {
            File = input,
            Size = new FileInfo(input).Length,
            CompanionFiles = companionFiles,
            ContentRoot = PackageLoader.ResolveContentRoot(
                input,
                explicitContentRoot
            )
        };

        var parsed = PackageLoader.ParsePackage(input);
        var root = parsed.Root;
        result.RawJson = parsed.RawJson;
        result.Parsed = root is not null;
        result.EngineVersion = parsed.EngineVersion;
        result.ParseErrors.AddRange(parsed.Errors);

        var sources = PackageLoader.LoadStringSources(
            input,
            companionFiles
        );
        var nameMap = root?["NameMap"]?
            .Values<string>()
            .Where(value => !string.IsNullOrWhiteSpace(value))
            .Select(value => value!)
            .ToList() ?? [];

        CollectReferences(result.References, nameMap);
        foreach (var source in sources)
        {
            CollectReferences(result.References, source.Strings);
        }

        NiagaraCollector.CollectParameters(result.NiagaraParameters, nameMap);
        foreach (var source in sources)
        {
            NiagaraCollector.CollectParameters(
                result.NiagaraParameters,
                source.Strings
            );
        }

        NiagaraCollector.CollectEmitters(result.NiagaraEmitters, nameMap);
        foreach (var source in sources)
        {
            NiagaraCollector.CollectEmitters(
                result.NiagaraEmitters,
                source.Strings
            );
        }

        if (root is not null)
        {
            StructuredDataCollector.Collect(root, result);
            NiagaraCollector.CollectParameterTypes(
                result.NiagaraParameterTypes,
                root
            );
            NiagaraCollector.CollectEmittersStructured(
                result.NiagaraEmitters,
                root
            );
            WidgetTreeCollector.Collect(root, result);
            BlueprintCollector.Collect(
                input,
                parsed,
                result.ContentRoot,
                result
            );
        }

        result.Classification = Classify(nameMap, sources, result);
        CollectDependencyStatus(result);
        CollectSearchMatches(result, sources, nameMap, searchTerms);
        return result;
    }

    private static void CollectReferences(
        List<string> target,
        IEnumerable<string> values
    )
    {
        var set = target.ToHashSet(StringComparer.Ordinal);
        foreach (var value in values)
        {
            foreach (Match match in UnrealPathRegex.Matches(value))
            {
                var reference = match.Value.TrimEnd('.', ':');
                if (set.Add(reference))
                {
                    target.Add(reference);
                }
            }
        }
        target.Sort(StringComparer.Ordinal);
    }

    private static void CollectDependencyStatus(InspectionResult result)
    {
        var packagePaths = result.References
            .Where(reference =>
                reference.StartsWith("/Game/", StringComparison.Ordinal))
            .Select(BlueprintCollector.NormalizePackagePath)
            .Distinct(StringComparer.Ordinal)
            .OrderBy(reference => reference, StringComparer.Ordinal);

        var selfPackage = BlueprintCollector.ToPackagePath(
            result.File,
            result.ContentRoot
        );
        foreach (var packagePath in packagePaths)
        {
            var localPath = ReferenceToLocalAsset(
                packagePath,
                result.ContentRoot
            );
            var status = string.Equals(
                packagePath,
                selfPackage,
                StringComparison.Ordinal
            )
                ? "Self"
                : localPath is null
                    ? "Unresolved"
                    : File.Exists(localPath)
                        ? "Exists"
                        : File.Exists(Path.ChangeExtension(localPath, ".umap"))
                            ? "Exists"
                            : "Missing";
            result.Dependencies.Add(new DependencySummary(
                packagePath,
                status,
                localPath
            ));
        }
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

        var packagePath = BlueprintCollector.NormalizePackagePath(reference);
        var relative = packagePath["/Game/".Length..]
            .Replace('/', Path.DirectorySeparatorChar);
        return Path.Combine(contentRoot, relative + ".uasset");
    }

    private static string Classify(
        IReadOnlyList<string> nameMap,
        IReadOnlyList<StringSource> sources,
        InspectionResult result
    )
    {
        var hasBlueprint =
            result.BlueprintComponents.Count > 0 ||
            nameMap.Any(value =>
                value.Contains("BlueprintGeneratedClass", StringComparison.Ordinal) ||
                value.Contains("SimpleConstructionScript", StringComparison.Ordinal)
            );
        var hasNiagara =
            result.NiagaraParameters.Count > 0 ||
            nameMap.Any(value =>
                value.Contains("Niagara", StringComparison.OrdinalIgnoreCase)
            ) ||
            sources.Any(source =>
                source.Strings.Any(value =>
                    value.Contains("Niagara", StringComparison.OrdinalIgnoreCase)
                )
            );

        return (hasBlueprint, hasNiagara) switch
        {
            (true, true) => "Blueprint with Niagara",
            (true, false) => "Blueprint",
            (false, true) => "Niagara",
            _ => "Unknown Unreal package"
        };
    }

    private static void CollectSearchMatches(
        InspectionResult result,
        IReadOnlyList<StringSource> sources,
        IReadOnlyList<string> nameMap,
        IReadOnlyList<string> terms
    )
    {
        var seen = new HashSet<string>(StringComparer.Ordinal);
        foreach (var term in terms.Where(term =>
                     !string.IsNullOrWhiteSpace(term)
                 ))
        {
            foreach (var value in nameMap)
            {
                AddMatches(result, seen, "NameMap", value, term, 6);
            }

            foreach (var source in sources)
            {
                foreach (var value in source.Strings)
                {
                    AddMatches(result, seen, source.Name, value, term, 12);
                    if (result.SearchMatches.Count(match =>
                            match.Term == term
                        ) >= 18)
                    {
                        break;
                    }
                }
            }
        }
    }

    private static void AddMatches(
        InspectionResult result,
        HashSet<string> seen,
        string source,
        string value,
        string term,
        int perValueLimit
    )
    {
        var start = 0;
        var count = 0;
        while (count < perValueLimit)
        {
            var index = value.IndexOf(
                term,
                start,
                StringComparison.OrdinalIgnoreCase
            );
            if (index < 0)
            {
                break;
            }

            var contextStart = Math.Max(0, index - 180);
            var contextLength = Math.Min(
                value.Length - contextStart,
                term.Length + 360
            );
            var context = value
                .Substring(contextStart, contextLength)
                .Replace("\0", "")
                .Trim();
            var key = $"{source}\n{term}\n{context}";
            if (seen.Add(key))
            {
                result.SearchMatches.Add(new SearchMatch(
                    source,
                    term,
                    context
                ));
            }

            count++;
            start = index + term.Length;
        }
    }
}
