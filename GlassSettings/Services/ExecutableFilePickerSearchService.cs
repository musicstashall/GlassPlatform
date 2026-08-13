namespace GlassSettings.Services;

/// <summary>Рекурсивный поиск файлов/папок в hosted file picker.</summary>
internal static class ExecutableFilePickerSearchService
{
    private const long SmallMinBytes = 16L * 1024;
    private const long SmallMaxBytes = 1024L * 1024;
    private const long MediumMaxBytes = 128L * 1024 * 1024;
    private const long LargeMaxBytes = 1024L * 1024 * 1024;

    public static Task<IReadOnlyList<ExecutableFilePickerItem>> SearchAsync(
        string rootPath,
        string query,
        ExecutableFilePickerSearchOptions options,
        IReadOnlyList<string>? catalogExtensionFilter,
        CancellationToken cancellationToken,
        bool foldersOnly = false) =>
        Task.Run(
            () => foldersOnly
                ? SearchFoldersCore(rootPath, query, options, cancellationToken)
                : SearchCore(rootPath, query, options, catalogExtensionFilter, cancellationToken),
            cancellationToken);

    private static IReadOnlyList<ExecutableFilePickerItem> SearchCore(
        string rootPath,
        string query,
        ExecutableFilePickerSearchOptions options,
        IReadOnlyList<string>? catalogExtensionFilter,
        CancellationToken cancellationToken)
    {
        var items = new List<ExecutableFilePickerItem>();
        var normalized = ExecutableFilePickerCatalog.NormalizeBrowsablePath(rootPath);
        if (!ExecutableFilePickerCatalog.IsValidDirectory(normalized) || string.IsNullOrWhiteSpace(query))
            return items;

        var normalizedQuery = query.Trim();
        var enumerateOptions = new EnumerationOptions
        {
            RecurseSubdirectories = options.Scope == ExecutableFilePickerSearchScope.AllNestedFolders,
            IgnoreInaccessible = true,
            AttributesToSkip = FileAttributes.Hidden | FileAttributes.System
        };

        IEnumerable<string> files;
        try
        {
            files = Directory.EnumerateFiles(normalized, "*", enumerateOptions);
        }
        catch
        {
            return items;
        }

        IEnumerator<string>? enumerator = null;
        try
        {
            enumerator = files.GetEnumerator();
            while (true)
            {
                cancellationToken.ThrowIfCancellationRequested();

                bool hasNext;
                try
                {
                    hasNext = enumerator.MoveNext();
                }
                catch (UnauthorizedAccessException)
                {
                    continue;
                }
                catch (DirectoryNotFoundException)
                {
                    continue;
                }

                if (!hasNext)
                    break;

                var file = enumerator.Current;
                try
                {
                    if ((File.GetAttributes(file) & (FileAttributes.Hidden | FileAttributes.System)) != 0)
                        continue;

                    var name = Path.GetFileName(file);
                    if (string.IsNullOrEmpty(name))
                        continue;

                    if (!name.Contains(normalizedQuery, StringComparison.OrdinalIgnoreCase))
                        continue;

                    var extension = Path.GetExtension(file);
                    if (!ExecutableFilePickerCatalog.MatchesExtensionFilter(extension, catalogExtensionFilter))
                        continue;

                    if (options.TypeExtension is not null
                        && !string.Equals(extension, options.TypeExtension, StringComparison.OrdinalIgnoreCase))
                        continue;

                    long? sizeBytes = null;
                    try
                    {
                        sizeBytes = new FileInfo(file).Length;
                    }
                    catch
                    {
                        // размер необязателен
                    }

                    if (options.SizeFilter is not null
                        && (!sizeBytes.HasValue || !MatchesSizeFilter(sizeBytes.Value, options.SizeFilter.Value)))
                        continue;

                    DateTime? modifiedUtc = null;
                    try
                    {
                        modifiedUtc = new FileInfo(file).LastWriteTimeUtc;
                    }
                    catch
                    {
                        // метаданные необязательны
                    }

                    items.Add(new ExecutableFilePickerItem
                    {
                        Name = name,
                        FullPath = file,
                        IsFolder = false,
                        TypeLabel = ExecutableFilePickerFileTypeHelper.GetTypeLabel(file),
                        DateModifiedUtc = modifiedUtc,
                        SizeBytes = sizeBytes
                    });
                }
                catch
                {
                    // пропуск недоступного файла
                }
            }
        }
        finally
        {
            enumerator?.Dispose();
        }

        items.Sort(static (a, b) => string.Compare(a.Name, b.Name, StringComparison.OrdinalIgnoreCase));
        return items;
    }

    private static IReadOnlyList<ExecutableFilePickerItem> SearchFoldersCore(
        string rootPath,
        string query,
        ExecutableFilePickerSearchOptions options,
        CancellationToken cancellationToken)
    {
        var items = new List<ExecutableFilePickerItem>();
        var normalized = ExecutableFilePickerCatalog.NormalizeBrowsablePath(rootPath);
        if (!ExecutableFilePickerCatalog.IsValidDirectory(normalized) || string.IsNullOrWhiteSpace(query))
            return items;

        var normalizedQuery = query.Trim();
        var enumerateOptions = new EnumerationOptions
        {
            RecurseSubdirectories = options.Scope == ExecutableFilePickerSearchScope.AllNestedFolders,
            IgnoreInaccessible = true,
            AttributesToSkip = FileAttributes.Hidden | FileAttributes.System
        };

        IEnumerable<string> directories;
        try
        {
            directories = Directory.EnumerateDirectories(normalized, "*", enumerateOptions);
        }
        catch
        {
            return items;
        }

        IEnumerator<string>? enumerator = null;
        try
        {
            enumerator = directories.GetEnumerator();
            while (true)
            {
                cancellationToken.ThrowIfCancellationRequested();

                bool hasNext;
                try
                {
                    hasNext = enumerator.MoveNext();
                }
                catch (UnauthorizedAccessException)
                {
                    continue;
                }
                catch (DirectoryNotFoundException)
                {
                    continue;
                }

                if (!hasNext)
                    break;

                var dir = enumerator.Current;
                try
                {
                    if ((File.GetAttributes(dir) & (FileAttributes.Hidden | FileAttributes.System)) != 0)
                        continue;

                    var name = Path.GetFileName(dir);
                    if (string.IsNullOrEmpty(name))
                        continue;

                    if (!name.Contains(normalizedQuery, StringComparison.OrdinalIgnoreCase))
                        continue;

                    DateTime? modifiedUtc = null;
                    try
                    {
                        modifiedUtc = new DirectoryInfo(dir).LastWriteTimeUtc;
                    }
                    catch
                    {
                        // метаданные необязательны
                    }

                    items.Add(new ExecutableFilePickerItem
                    {
                        Name = name,
                        FullPath = dir,
                        IsFolder = true,
                        DateModifiedUtc = modifiedUtc
                    });
                }
                catch
                {
                    // пропуск недоступной папки
                }
            }
        }
        finally
        {
            enumerator?.Dispose();
        }

        items.Sort(static (a, b) => string.Compare(a.Name, b.Name, StringComparison.OrdinalIgnoreCase));
        return items;
    }

    /// <summary>Фильтр типа/размера по уже загруженным результатам (без повторного обхода диска).</summary>
    public static List<ExecutableFilePickerItem> ApplyClientFilters(
        IReadOnlyList<ExecutableFilePickerItem> items,
        ExecutableFilePickerSearchOptions options)
    {
        var filtered = new List<ExecutableFilePickerItem>(items.Count);
        foreach (var item in items)
        {
            if (item.IsFolder)
            {
                filtered.Add(item);
                continue;
            }

            var extension = Path.GetExtension(item.FullPath);
            if (options.TypeExtension is not null
                && !string.Equals(extension, options.TypeExtension, StringComparison.OrdinalIgnoreCase))
                continue;

            if (options.SizeFilter is not null
                && (!item.SizeBytes.HasValue
                    || !MatchesSizeFilter(item.SizeBytes.Value, options.SizeFilter.Value)))
                continue;

            filtered.Add(item);
        }

        filtered.Sort(static (a, b) => string.Compare(a.Name, b.Name, StringComparison.OrdinalIgnoreCase));
        return filtered;
    }

    private static bool MatchesSizeFilter(long sizeBytes, ExecutableFilePickerSearchSizeFilter filter) =>
        filter switch
        {
            ExecutableFilePickerSearchSizeFilter.Small =>
                sizeBytes >= SmallMinBytes && sizeBytes <= SmallMaxBytes,
            ExecutableFilePickerSearchSizeFilter.Medium =>
                sizeBytes > SmallMaxBytes && sizeBytes <= MediumMaxBytes,
            ExecutableFilePickerSearchSizeFilter.Large =>
                sizeBytes > MediumMaxBytes && sizeBytes <= LargeMaxBytes,
            _ => true
        };
}
