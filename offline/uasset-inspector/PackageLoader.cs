using System.Text;
using Newtonsoft.Json.Linq;
using UAssetAPI;
using UAssetAPI.UnrealTypes;

static partial class PackageLoader
{
    private static readonly EngineVersion[] Versions =
    [
        EngineVersion.VER_UE4_27,
        EngineVersion.VER_UE5_0,
        EngineVersion.VER_UE5_1,
        EngineVersion.VER_UE5_2,
        EngineVersion.VER_UE5_3
    ];

    public static ParsedPackage ParsePackage(string input)
    {
        var errors = new List<string>();
        foreach (var version in Versions)
        {
            try
            {
                var asset = new UAsset(input, version);
                var rawJson = asset.SerializeJson(true);
                return new ParsedPackage(
                    input,
                    JObject.Parse(rawJson),
                    rawJson,
                    version.ToString(),
                    errors
                );
            }
            catch (Exception ex)
            {
                errors.Add($"{version}: {DescribeParseError(ex)}");
            }
        }

        return new ParsedPackage(input, null, null, null, errors);
    }

    /// <summary>
    /// 文件被占用（常见于 UE 编辑器打开中）时给出明确提示，避免误判为解析失败。
    /// </summary>
    private static string DescribeParseError(Exception ex)
    {
        if (ex is IOException &&
            ex.Message.Contains(
                "being used by another process",
                StringComparison.OrdinalIgnoreCase
            ))
        {
            return "IOException: 文件被其他进程占用（可能 UE 编辑器打开中），关闭后重试";
        }
        return $"{ex.GetType().Name}: {ex.Message}";
    }

    public static string? ResolveContentRoot(
        string input,
        string? explicitContentRoot
    )
    {
        if (!string.IsNullOrWhiteSpace(explicitContentRoot))
        {
            return Directory.Exists(explicitContentRoot)
                ? Path.GetFullPath(explicitContentRoot)
                : null;
        }

        var fullPath = Path.GetFullPath(input);
        var separator = Path.DirectorySeparatorChar;
        var marker = $"{separator}Content{separator}";
        var index = fullPath.IndexOf(
            marker,
            StringComparison.OrdinalIgnoreCase
        );
        if (index < 0)
        {
            return null;
        }
        return fullPath[..(index + marker.Length - 1)];
    }

    public static List<string> FindCompanions(string input)
    {
        var directory = Path.GetDirectoryName(input) ?? ".";
        var stem = Path.GetFileNameWithoutExtension(input);
        var companions = new List<string>();
        foreach (var extension in new[] { ".uexp", ".ubulk", ".uptnl" })
        {
            var candidate = Path.Combine(directory, stem + extension);
            if (File.Exists(candidate))
            {
                companions.Add(candidate);
            }
        }
        return companions;
    }

    public static List<StringSource> LoadStringSources(
        string input,
        IReadOnlyList<string> companionFiles
    )
    {
        var files = new[] { input }.Concat(companionFiles);
        var sources = new List<StringSource>();
        foreach (var file in files)
        {
            byte[] bytes;
            try
            {
                bytes = File.ReadAllBytes(file);
            }
            catch (IOException)
            {
                // 文件被占用（UE 编辑器打开中）时跳过该字符串源，
                // 占用提示已由 ParsePackage 记录到 ParseErrors。
                continue;
            }
            sources.Add(new StringSource(
                Path.GetFileName(file),
                ExtractStrings(bytes)
            ));
        }
        return sources;
    }

    private static List<string> ExtractStrings(byte[] bytes)
    {
        var values = new HashSet<string>(StringComparer.Ordinal);
        ExtractAscii(bytes, values);
        ExtractUtf16Le(bytes, values);
        return values
            .OrderBy(value => value, StringComparer.Ordinal)
            .ToList();
    }

    private static void ExtractAscii(
        byte[] bytes,
        HashSet<string> output
    )
    {
        var start = -1;
        for (var i = 0; i <= bytes.Length; i++)
        {
            var printable = i < bytes.Length &&
                (bytes[i] is >= 0x20 and <= 0x7e ||
                 bytes[i] is 0x09 or 0x0a or 0x0d);
            if (printable)
            {
                if (start < 0)
                {
                    start = i;
                }
                continue;
            }

            if (start >= 0 && i - start >= 4)
            {
                output.Add(Encoding.UTF8.GetString(bytes, start, i - start));
            }
            start = -1;
        }
    }

    private static void ExtractUtf16Le(
        byte[] bytes,
        HashSet<string> output
    )
    {
        for (var parity = 0; parity < 2; parity++)
        {
            var start = -1;
            var i = parity;
            for (; i + 1 < bytes.Length; i += 2)
            {
                var character = bytes[i] | bytes[i + 1] << 8;
                var printable = character is >= 0x20 and <= 0x7e ||
                    character is 0x09 or 0x0a or 0x0d;
                if (printable)
                {
                    if (start < 0)
                    {
                        start = i;
                    }
                    continue;
                }

                if (start >= 0 && i - start >= 8)
                {
                    output.Add(Encoding.Unicode.GetString(
                        bytes,
                        start,
                        i - start
                    ));
                }
                start = -1;
            }

            if (start >= 0 && i - start >= 8)
            {
                output.Add(Encoding.Unicode.GetString(
                    bytes,
                    start,
                    i - start
                ));
            }
        }
    }
}

sealed record StringSource(
    string Name,
    IReadOnlyList<string> Strings
);

sealed record ParsedPackage(
    string File,
    JObject? Root,
    string? RawJson,
    string? EngineVersion,
    List<string> Errors
);
