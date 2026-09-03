sealed class CliOptions
{
    public required string Input { get; init; }
    public required string Format { get; init; }
    public string? ContentRoot { get; init; }
    public List<string> SearchTerms { get; } = [];

    public static CliOptions? Parse(string[] args)
    {
        var input = (string?)null;
        var format = "markdown";
        var contentRoot = (string?)null;
        var terms = new List<string>();

        for (var i = 0; i < args.Length; i++)
        {
            switch (args[i])
            {
                case "--input":
                    input = NextArg(args, ref i);
                    break;
                case "--format":
                    format = NextArg(args, ref i) ?? "markdown";
                    break;
                case "--search":
                    var value = NextArg(args, ref i);
                    if (value is not null)
                    {
                        terms.AddRange(value.Split(
                            ',',
                            StringSplitOptions.RemoveEmptyEntries |
                            StringSplitOptions.TrimEntries
                        ));
                    }
                    break;
                case "--content-root":
                    contentRoot = NextArg(args, ref i);
                    break;
            }
        }

        if (string.IsNullOrWhiteSpace(input) ||
            format is not ("markdown" or "json" or "raw-json"))
        {
            return null;
        }

        var result = new CliOptions
        {
            Input = Path.GetFullPath(input),
            Format = format,
            ContentRoot = string.IsNullOrWhiteSpace(contentRoot)
                ? null
                : Path.GetFullPath(contentRoot)
        };
        result.SearchTerms.AddRange(terms);
        return result;
    }

    private static string? NextArg(string[] args, ref int index)
    {
        if (index + 1 >= args.Length)
        {
            return null;
        }
        index++;
        return args[index];
    }
}
