namespace GlassSettings.Search;

public sealed record SettingsSearchSuggestion(
    string Id,
    string Title,
    string Location,
    string Destination,
    string SearchText)
{
    public string DisplayText => string.IsNullOrWhiteSpace(Location)
        ? Title
        : $"{Title}  ·  {Location}";
}
