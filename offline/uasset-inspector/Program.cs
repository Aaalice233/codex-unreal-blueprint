using System.Text;
using Newtonsoft.Json;

Console.OutputEncoding = new UTF8Encoding(false);

var options = CliOptions.Parse(args);
if (options is null)
{
    Console.Error.WriteLine(
        "Usage: UnrealUAssetInspector --input <asset> " +
        "[--format markdown|json|raw-json] [--search <term>] " +
        "[--content-root <Content directory>]"
    );
    return 2;
}

if (!File.Exists(options.Input))
{
    Console.Error.WriteLine($"Asset not found: {options.Input}");
    return 2;
}

var inspection = Inspector.Inspect(
    options.Input,
    options.SearchTerms,
    options.ContentRoot
);
if (options.Format == "raw-json")
{
    if (inspection.RawJson is null)
    {
        Console.Error.WriteLine(
            "Structured parsing failed; raw UAssetAPI JSON is unavailable. " +
            string.Join(" | ", inspection.ParseErrors)
        );
        return 1;
    }

    Console.WriteLine(inspection.RawJson);
    return 0;
}

if (options.Format == "json")
{
    Console.WriteLine(JsonConvert.SerializeObject(
        inspection.ForOutput(),
        Formatting.Indented
    ));
    return 0;
}

Console.WriteLine(MarkdownRenderer.Render(inspection));
return 0;
