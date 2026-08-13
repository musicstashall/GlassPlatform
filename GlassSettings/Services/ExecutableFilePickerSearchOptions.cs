namespace GlassSettings.Services;

internal enum ExecutableFilePickerSearchScope
{
    AllNestedFolders,
    CurrentFolder
}

internal enum ExecutableFilePickerSearchSizeFilter
{
    Small,
    Medium,
    Large
}

/// <summary>Параметры поиска file picker (живут только в рамках одного диалога).</summary>
internal sealed class ExecutableFilePickerSearchOptions
{
    public ExecutableFilePickerSearchScope Scope { get; set; } = ExecutableFilePickerSearchScope.AllNestedFolders;

    /// <summary>null — любой тип.</summary>
    public string? TypeExtension { get; set; }

    /// <summary>null — любой размер.</summary>
    public ExecutableFilePickerSearchSizeFilter? SizeFilter { get; set; }

    public ExecutableFilePickerSearchOptions Clone() => new()
    {
        Scope = Scope,
        TypeExtension = TypeExtension,
        SizeFilter = SizeFilter
    };
}

internal sealed class ExecutableFilePickerNavigationEntry
{
    public required string FolderPath { get; init; }

    public bool IsSearchPage { get; init; }

    public string? SearchQuery { get; init; }
}
