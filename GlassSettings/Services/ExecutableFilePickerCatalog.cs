using GlassSettings.Core.Registry;
using GlassSettings.Strings;

namespace GlassSettings.Services;

/// <summary>Чтение каталогов для hosted file picker (System.IO, elevated-safe).</summary>
public static class ExecutableFilePickerCatalog
{
    private static readonly string[] SupportedExecutableExtensionList = [".exe", ".com", ".bat", ".cmd"];

    /// <summary>Как в native GlassSettings_PickImageFile / CurrentWallpaperResolver.</summary>
    private static readonly string[] SupportedImageExtensionList =
    [
        ".jpg", ".jpeg", ".png", ".bmp", ".gif", ".tif", ".tiff", ".jfif", ".webp", ".heic"
    ];

    public static IReadOnlyList<string> SupportedExecutableExtensions { get; } = SupportedExecutableExtensionList;

    public static IReadOnlyList<string> SupportedImageExtensions { get; } = SupportedImageExtensionList;

    private static readonly HashSet<string> ExecutableExtensions = new(SupportedExecutableExtensionList, StringComparer.OrdinalIgnoreCase);

    private static readonly HashSet<string> ImageExtensions = new(SupportedImageExtensionList, StringComparer.OrdinalIgnoreCase);

    public static bool IsExecutableExtension(string path) =>
        ExecutableExtensions.Contains(Path.GetExtension(path));

    public static bool IsImageExtension(string path) =>
        ImageExtensions.Contains(Path.GetExtension(path));

    public static string GetInitialDirectory()
    {
        var stored = GlassSettingsRegistry.ReadExecutableFilePickerLastPath();
        if (!string.IsNullOrWhiteSpace(stored) && IsValidDirectory(stored))
            return NormalizeBrowsablePath(stored);

        var desktop = Environment.GetFolderPath(Environment.SpecialFolder.DesktopDirectory);
        if (Directory.Exists(desktop))
            return desktop;

        var documents = Environment.GetFolderPath(Environment.SpecialFolder.MyDocuments);
        return Directory.Exists(documents) ? documents : @"C:\";
    }

    public static bool RememberLastDirectory(string path)
    {
        if (!IsValidDirectory(path))
            return false;

        GlassSettingsRegistry.WriteExecutableFilePickerLastPath(NormalizeBrowsablePath(path));
        return true;
    }

    public static IReadOnlyList<ExecutableFilePickerQuickLink> GetQuickLinks()
    {
        var links = new List<ExecutableFilePickerQuickLink>();

        TryAddKnownFolder(links, Environment.SpecialFolder.DesktopDirectory, "Attach_FilePickerQuickDesktop");
        TryAddKnownFolder(links, Environment.SpecialFolder.MyDocuments, "Attach_FilePickerQuickDocuments");
        TryAddKnownFolder(links, Environment.SpecialFolder.MyPictures, "Attach_FilePickerQuickPictures");
        TryAddDownloads(links);

        foreach (var drive in DriveInfo.GetDrives())
        {
            if (!drive.IsReady)
                continue;

            var root = drive.RootDirectory.FullName;
            links.Add(new ExecutableFilePickerQuickLink
            {
                DisplayName = drive.Name.TrimEnd('\\'),
                Path = root
            });
        }

        return links;
    }

    public static IReadOnlyList<ExecutableFilePickerExtensionFilterItem> GetExtensionFilters()
    {
        var filters = new List<ExecutableFilePickerExtensionFilterItem>
        {
            new()
            {
                Extensions = null,
                DisplayName = GlassSettingsResources.Get("Attach_FilePickerAllFiles")
            }
        };

        foreach (var extension in SupportedExecutableExtensionList)
        {
            filters.Add(new ExecutableFilePickerExtensionFilterItem
            {
                Extension = extension,
                DisplayName = GetExtensionFilterLabel(extension)
            });
        }

        return filters;
    }

    public static IReadOnlyList<ExecutableFilePickerExtensionFilterItem> GetImageExtensionFilters()
    {
        var filters = new List<ExecutableFilePickerExtensionFilterItem>
        {
            new()
            {
                Extensions = SupportedImageExtensionList,
                DisplayName = GlassSettingsResources.Get("Themes_Bg_FilePickerAllImages")
            }
        };

        foreach (var extension in SupportedImageExtensionList)
        {
            filters.Add(new ExecutableFilePickerExtensionFilterItem
            {
                Extension = extension,
                DisplayName = GetImageExtensionFilterLabel(extension)
            });
        }

        return filters;
    }

    public static bool MatchesExtensionFilter(string extension, IReadOnlyList<string>? extensionFilter)
    {
        if (extensionFilter is null)
            return true;

        for (var i = 0; i < extensionFilter.Count; i++)
        {
            if (string.Equals(extension, extensionFilter[i], StringComparison.OrdinalIgnoreCase))
                return true;
        }

        return false;
    }

    public static bool ExtensionFiltersEqual(IReadOnlyList<string>? left, IReadOnlyList<string>? right)
    {
        if (ReferenceEquals(left, right))
            return true;

        if (left is null || right is null)
            return left is null && right is null;

        if (left.Count != right.Count)
            return false;

        for (var i = 0; i < left.Count; i++)
        {
            if (!string.Equals(left[i], right[i], StringComparison.OrdinalIgnoreCase))
                return false;
        }

        return true;
    }

    public static Task<IReadOnlyList<ExecutableFilePickerItem>> LoadDirectoryAsync(
        string path,
        IReadOnlyList<string>? extensionFilter = null) =>
        Task.Run(() => LoadDirectoryCore(path, extensionFilter));

    public static Task<IReadOnlyList<ExecutableFilePickerItem>> LoadSubfoldersAsync(string path) =>
        Task.Run(() => LoadSubfoldersCore(path));

    public static bool HasSubfolders(string path)
    {
        if (!IsValidDirectory(path))
            return false;

        try
        {
            foreach (var dir in Directory.EnumerateDirectories(path))
            {
                try
                {
                    if ((File.GetAttributes(dir) & FileAttributes.Hidden) != 0)
                        continue;

                    return true;
                }
                catch
                {
                    // пропуск
                }
            }
        }
        catch
        {
            return false;
        }

        return false;
    }

    public static bool TryGetParentPath(string path, out string? parent)
    {
        parent = null;
        if (string.IsNullOrWhiteSpace(path))
            return false;

        var normalized = path.TrimEnd(Path.DirectorySeparatorChar, Path.AltDirectorySeparatorChar);
        var root = Path.GetPathRoot(normalized);
        if (string.IsNullOrEmpty(root))
            return false;

        if (string.Equals(normalized, root.TrimEnd(Path.DirectorySeparatorChar, Path.AltDirectorySeparatorChar),
                StringComparison.OrdinalIgnoreCase))
            return false;

        var parentInfo = Directory.GetParent(normalized);
        if (parentInfo is null)
            return false;

        parent = parentInfo.FullName;
        return true;
    }

    public static bool IsValidDirectory(string path)
    {
        if (string.IsNullOrWhiteSpace(path))
            return false;

        try
        {
            return Directory.Exists(NormalizeBrowsablePath(path));
        }
        catch
        {
            return false;
        }
    }

    /// <summary>Канонический путь папки; корень диска всегда с завершающим '\'.</summary>
    public static string NormalizeBrowsablePath(string path)
    {
        if (string.IsNullOrWhiteSpace(path))
            return string.Empty;

        var trimmed = path.Trim();
        var root = Path.GetPathRoot(trimmed);
        if (string.IsNullOrEmpty(root))
            return trimmed.TrimEnd(Path.DirectorySeparatorChar, Path.AltDirectorySeparatorChar);

        var relative = trimmed.Substring(root.Length)
            .TrimStart(Path.DirectorySeparatorChar, Path.AltDirectorySeparatorChar);
        if (string.IsNullOrEmpty(relative))
            return root;

        return root + relative.TrimEnd(Path.DirectorySeparatorChar, Path.AltDirectorySeparatorChar);
    }

    public static bool PathsEqual(string? left, string? right) =>
        string.Equals(
            NormalizeBrowsablePath(left ?? string.Empty),
            NormalizeBrowsablePath(right ?? string.Empty),
            StringComparison.OrdinalIgnoreCase);

    private static DateTime? TryGetFileLastWriteTimeUtc(string path)
    {
        try
        {
            return new FileInfo(path).LastWriteTimeUtc;
        }
        catch
        {
            return null;
        }
    }

    private static long? TryGetFileLength(string path)
    {
        try
        {
            return new FileInfo(path).Length;
        }
        catch
        {
            return null;
        }
    }

    private static string GetExtensionFilterLabel(string extension) =>
        extension.ToLowerInvariant() switch
        {
            ".exe" => GlassSettingsResources.Get("Attach_FilePickerFilterExe"),
            ".com" => GlassSettingsResources.Get("Attach_FilePickerFilterCom"),
            ".bat" => GlassSettingsResources.Get("Attach_FilePickerFilterBat"),
            ".cmd" => GlassSettingsResources.Get("Attach_FilePickerFilterCmd"),
            _ => extension.ToUpperInvariant()
        };

    private static string GetImageExtensionFilterLabel(string extension) =>
        extension.ToLowerInvariant() switch
        {
            ".jpg" => GlassSettingsResources.Get("Themes_Bg_FilePickerFilterJpg"),
            ".jpeg" => GlassSettingsResources.Get("Themes_Bg_FilePickerFilterJpeg"),
            ".png" => GlassSettingsResources.Get("Themes_Bg_FilePickerFilterPng"),
            ".bmp" => GlassSettingsResources.Get("Themes_Bg_FilePickerFilterBmp"),
            ".gif" => GlassSettingsResources.Get("Themes_Bg_FilePickerFilterGif"),
            ".tif" => GlassSettingsResources.Get("Themes_Bg_FilePickerFilterTif"),
            ".tiff" => GlassSettingsResources.Get("Themes_Bg_FilePickerFilterTiff"),
            ".jfif" => GlassSettingsResources.Get("Themes_Bg_FilePickerFilterJfif"),
            ".webp" => GlassSettingsResources.Get("Themes_Bg_FilePickerFilterWebp"),
            ".heic" => GlassSettingsResources.Get("Themes_Bg_FilePickerFilterHeic"),
            _ => extension.ToUpperInvariant()
        };

    private static void TryAddKnownFolder(
        ICollection<ExecutableFilePickerQuickLink> links,
        Environment.SpecialFolder folder,
        string resourceKey)
    {
        var path = Environment.GetFolderPath(folder);
        if (!Directory.Exists(path))
            return;

        links.Add(new ExecutableFilePickerQuickLink
        {
            DisplayName = GlassSettingsResources.Get(resourceKey),
            Path = path
        });
    }

    private static void TryAddDownloads(ICollection<ExecutableFilePickerQuickLink> links)
    {
        var userProfile = Environment.GetFolderPath(Environment.SpecialFolder.UserProfile);
        if (string.IsNullOrWhiteSpace(userProfile))
            return;

        var downloads = Path.Combine(userProfile, "Downloads");
        if (!Directory.Exists(downloads))
            return;

        links.Add(new ExecutableFilePickerQuickLink
        {
            DisplayName = GlassSettingsResources.Get("Attach_FilePickerQuickDownloads"),
            Path = downloads
        });
    }

    private static IReadOnlyList<ExecutableFilePickerItem> LoadDirectoryCore(
        string path,
        IReadOnlyList<string>? extensionFilter)
    {
        var items = new List<ExecutableFilePickerItem>();
        if (!IsValidDirectory(path))
            return items;

        try
        {
            foreach (var dir in Directory.EnumerateDirectories(path))
            {
                try
                {
                    var name = Path.GetFileName(dir);
                    if (string.IsNullOrEmpty(name))
                        continue;

                    if ((File.GetAttributes(dir) & FileAttributes.Hidden) != 0)
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

            items.Sort(static (a, b) => string.Compare(a.Name, b.Name, StringComparison.OrdinalIgnoreCase));

            var files = new List<ExecutableFilePickerItem>();
            foreach (var file in Directory.EnumerateFiles(path))
            {
                try
                {
                    var name = Path.GetFileName(file);
                    if (string.IsNullOrEmpty(name))
                        continue;

                    var extension = Path.GetExtension(file);
                    if (!MatchesExtensionFilter(extension, extensionFilter))
                        continue;

                    files.Add(new ExecutableFilePickerItem
                    {
                        Name = name,
                        FullPath = file,
                        IsFolder = false,
                        TypeLabel = ExecutableFilePickerFileTypeHelper.GetTypeLabel(file),
                        DateModifiedUtc = TryGetFileLastWriteTimeUtc(file),
                        SizeBytes = TryGetFileLength(file)
                    });
                }
                catch
                {
                    // пропуск недоступного файла
                }
            }

            files.Sort(static (a, b) => string.Compare(a.Name, b.Name, StringComparison.OrdinalIgnoreCase));
            items.AddRange(files);
        }
        catch
        {
            return Array.Empty<ExecutableFilePickerItem>();
        }

        return items;
    }

    private static IReadOnlyList<ExecutableFilePickerItem> LoadSubfoldersCore(string path)
    {
        var items = new List<ExecutableFilePickerItem>();
        if (!IsValidDirectory(path))
            return items;

        try
        {
            foreach (var dir in Directory.EnumerateDirectories(path))
            {
                try
                {
                    var name = Path.GetFileName(dir);
                    if (string.IsNullOrEmpty(name))
                        continue;

                    if ((File.GetAttributes(dir) & FileAttributes.Hidden) != 0)
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

            items.Sort(static (a, b) => string.Compare(a.Name, b.Name, StringComparison.OrdinalIgnoreCase));
        }
        catch
        {
            return Array.Empty<ExecutableFilePickerItem>();
        }

        return items;
    }
}
