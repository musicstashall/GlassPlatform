using System.Runtime.InteropServices;
using System.Text;

namespace GlassSettings.Services;

/// <summary>Вид альбома слайдшоу (live COM / GlassLastSlideshowKind).</summary>
public enum SlideshowAlbumKind
{
    None = 0,
    Folder = 1,
    WindowsDesktopBackgrounds = 2
}

/// <summary>Снимок live-альбома IDesktopWallpaper.GetSlideshow.</summary>
internal readonly struct SlideshowAlbumSnapshot
{
    public SlideshowAlbumSnapshot(
        SlideshowAlbumKind kind,
        string folderPath,
        string displayName,
        IReadOnlyList<string> imagePaths,
        bool isFileSubset)
    {
        Kind = kind;
        FolderPath = folderPath;
        DisplayName = displayName;
        ImagePaths = imagePaths;
        IsFileSubset = isFileSubset;
    }

    public SlideshowAlbumKind Kind { get; }
    /// <summary>FS-папка (Folder) или %SystemRoot%\Web\Wallpaper (Windows album).</summary>
    public string FolderPath { get; }
    public string DisplayName { get; }
    /// <summary>FILESYS-пути из GetSlideshow; пусто при полном контейнере-папке.</summary>
    public IReadOnlyList<string> ImagePaths { get; }
    /// <summary>true — GetSlideshow вернул список файлов, не контейнер.</summary>
    public bool IsFileSubset { get; }
}

/// <summary>COM IDesktopWallpaper — альбом слайдшоу, интервал, shuffle.</summary>
internal static class DesktopWallpaperCom
{
    private const uint SigdnFileSysPath = 0x80058000;
    private const uint SigdnNormalDisplay = 0;
    private const uint SigdnDesktopAbsoluteEditing = 0x8004c000;
    private const uint SigdnUrl = 0x80068000;
    private const int DesktopSlideshowOptionsShuffleImages = 0x1;
    private const int DesktopSlideshowDirectionForward = 0;

    private static readonly Guid IidShellItem = new("43826D1E-E718-42EE-BC55-A1E261C37BFE");
    private static readonly Guid IidShellItemArray = new("b63ea76d-1f85-456f-a19c-48159efa858b");
    private static readonly Guid BhidEnumItems = new("94f60519-2850-4924-aa5a-d15e848520dc");
    private static readonly Guid IidEnumShellItems = new("70629033-e363-4a28-a567-0db78006e6d7");

    public static string GetWindowsDesktopBackgroundsRoot()
    {
        var systemRoot = Environment.GetEnvironmentVariable("SystemRoot") ?? @"C:\Windows";
        return Path.GetFullPath(Path.Combine(systemRoot, "Web", "Wallpaper"));
    }

    public static bool IsUnderWindowsDesktopBackgroundsRoot(string? path)
    {
        if (string.IsNullOrWhiteSpace(path))
            return false;

        try
        {
            var full = Path.GetFullPath(path);
            var root = GetWindowsDesktopBackgroundsRoot()
                .TrimEnd(Path.DirectorySeparatorChar, Path.AltDirectorySeparatorChar);
            if (full.Equals(root, StringComparison.OrdinalIgnoreCase))
                return true;

            var prefix = root + Path.DirectorySeparatorChar;
            return full.StartsWith(prefix, StringComparison.OrdinalIgnoreCase);
        }
        catch
        {
            return false;
        }
    }

    /// <summary>Общий родительский каталог всех файлов; false, если родители разные.</summary>
    private static bool TryGetSharedParentDirectory(IReadOnlyList<string> filePaths, out string parent)
    {
        parent = string.Empty;
        if (filePaths.Count == 0)
            return false;

        string? shared = null;
        for (var i = 0; i < filePaths.Count; i++)
        {
            string dir;
            try
            {
                dir = Path.GetDirectoryName(Path.GetFullPath(filePaths[i])) ?? string.Empty;
                if (string.IsNullOrWhiteSpace(dir))
                    return false;
                dir = Path.GetFullPath(dir);
            }
            catch
            {
                return false;
            }

            if (shared is null)
                shared = dir;
            else if (!shared.Equals(dir, StringComparison.OrdinalIgnoreCase))
                return false;
        }

        parent = shared!;
        return Directory.Exists(parent);
    }

    /// <summary>Имя папки альбома: shell LocalizedResourceName (Течение), не имя файла из GetSlideshow.</summary>
    private static string ResolveFolderAlbumDisplayName(string folderPath, string? firstDisplay)
    {
        if (TryGetShellFolderDisplayName(folderPath, out var shellName) && !string.IsNullOrWhiteSpace(shellName))
            return shellName;

        // firstDisplay у file-subset часто имя файла (img32.jpg) — не использовать как имя папки.
        if (!string.IsNullOrWhiteSpace(firstDisplay)
            && firstDisplay.IndexOf('.') < 0
            && !firstDisplay.Contains('\\')
            && !firstDisplay.Contains('/'))
        {
            return firstDisplay!;
        }

        var name = Path.GetFileName(
            folderPath.TrimEnd(Path.DirectorySeparatorChar, Path.AltDirectorySeparatorChar));
        return string.IsNullOrWhiteSpace(name) ? folderPath : name;
    }

    /// <summary>
    /// Локализованное имя папки как в Проводнике (desktop.ini / themeui.dll), не FS-имя ThemeA.
    /// </summary>
    public static bool TryGetShellFolderDisplayName(string folderPath, out string displayName)
    {
        displayName = string.Empty;
        if (string.IsNullOrWhiteSpace(folderPath) || !Directory.Exists(folderPath))
            return false;

        try
        {
            var full = Path.GetFullPath(folderPath);
            var iid = IidShellItem;
            var hr = SHCreateItemFromParsingName(full, IntPtr.Zero, ref iid, out var item);
            if (hr < 0 || item is null)
                return false;

            var name = GetDisplayName(item, SigdnNormalDisplay);
            if (string.IsNullOrWhiteSpace(name))
                return false;

            displayName = name;
            return true;
        }
        catch
        {
            return false;
        }
    }

    /// <summary>
    /// FS-папка альбома (Directory.Exists). Для search-folder «Фоны…» / file-subset — false;
    /// используйте <see cref="TryGetSlideshowAlbum"/>.
    /// </summary>
    public static bool TryGetSlideshowFolder(out string path, out string displayName)
    {
        path = string.Empty;
        displayName = string.Empty;
        if (!TryGetSlideshowAlbum(out var snap) || snap.Kind != SlideshowAlbumKind.Folder)
            return false;

        if (string.IsNullOrWhiteSpace(snap.FolderPath) || !Directory.Exists(snap.FolderPath))
            return false;

        path = snap.FolderPath;
        displayName = snap.DisplayName;
        return true;
    }

    public static bool TryGetSlideshowAlbum(out SlideshowAlbumSnapshot snapshot)
    {
        snapshot = default;
        try
        {
            var wall = CreateWallpaper();
            wall.GetSlideshow(out var items);
            if (items is null)
                return false;

            items.GetCount(out var count);
            if (count == 0)
                return false;

            var filePaths = new List<string>((int)count);
            string? firstDisplay = null;
            string? firstEditing = null;
            string? firstFileSys = null;
            var anySearchMs = false;

            for (uint i = 0; i < count; i++)
            {
                items.GetItemAt(i, out var item);
                if (item is null)
                    continue;

                var fileSys = GetDisplayName(item, SigdnFileSysPath);
                var display = GetDisplayName(item, SigdnNormalDisplay);
                var editing = GetDisplayName(item, SigdnDesktopAbsoluteEditing);
                var url = GetDisplayName(item, SigdnUrl);

                if (i == 0)
                {
                    firstDisplay = display;
                    firstEditing = string.IsNullOrWhiteSpace(editing) ? url : editing;
                    firstFileSys = fileSys;
                }

                if (IsSearchMs(editing) || IsSearchMs(display) || IsSearchMs(url))
                    anySearchMs = true;

                if (!string.IsNullOrWhiteSpace(fileSys) && File.Exists(fileSys))
                    filePaths.Add(Path.GetFullPath(fileSys));
            }

            // Полный контейнер: один item — папка FS.
            if (count == 1
                && filePaths.Count == 0
                && !string.IsNullOrWhiteSpace(firstFileSys)
                && Directory.Exists(firstFileSys))
            {
                var folder = Path.GetFullPath(firstFileSys);
                if (folder.Equals(GetWindowsDesktopBackgroundsRoot(), StringComparison.OrdinalIgnoreCase)
                    || anySearchMs)
                {
                    snapshot = new SlideshowAlbumSnapshot(
                        SlideshowAlbumKind.WindowsDesktopBackgrounds,
                        GetWindowsDesktopBackgroundsRoot(),
                        ResolveWindowsDesktopBackgroundsDisplayName(firstDisplay, firstEditing),
                        Array.Empty<string>(),
                        isFileSubset: false);
                    return true;
                }

                var name = ResolveFolderAlbumDisplayName(folder, firstDisplay);
                snapshot = new SlideshowAlbumSnapshot(
                    SlideshowAlbumKind.Folder,
                    folder,
                    name,
                    Array.Empty<string>(),
                    isFileSubset: false);
                return true;
            }

            // File-subset: все файлы в одной FS-папке (ThemeA/Nature/…) → Folder.
            // Нельзя считать «под Web\Wallpaper» = «Фоны Windows» (ломало Theme* → Windows).
            if (filePaths.Count > 0
                && TryGetSharedParentDirectory(filePaths, out var sharedParent)
                && !sharedParent.Equals(GetWindowsDesktopBackgroundsRoot(), StringComparison.OrdinalIgnoreCase))
            {
                var name = ResolveFolderAlbumDisplayName(sharedParent, firstDisplay);
                snapshot = new SlideshowAlbumSnapshot(
                    SlideshowAlbumKind.Folder,
                    sharedParent,
                    name,
                    filePaths,
                    isFileSubset: true);
                return true;
            }

            // Windows-альбом: search-ms или файлы из нескольких папок под Web\Wallpaper.
            if (anySearchMs
                || (filePaths.Count > 0 && filePaths.All(IsUnderWindowsDesktopBackgroundsRoot)))
            {
                var displayName = ResolveWindowsDesktopBackgroundsDisplayName(firstDisplay, firstEditing);
                if (filePaths.Count > 0
                    && !string.IsNullOrWhiteSpace(firstDisplay)
                    && filePaths.Any(p =>
                        Path.GetFileName(p).Equals(firstDisplay, StringComparison.OrdinalIgnoreCase)))
                {
                    displayName = ExtractSearchMsDisplayName(firstEditing)
                        ?? ExtractSearchMsDisplayName(firstDisplay)
                        ?? displayName;
                }

                snapshot = new SlideshowAlbumSnapshot(
                    SlideshowAlbumKind.WindowsDesktopBackgrounds,
                    GetWindowsDesktopBackgroundsRoot(),
                    displayName,
                    filePaths,
                    isFileSubset: filePaths.Count > 0);
                return true;
            }

            if (filePaths.Count > 0)
            {
                var parent = Path.GetDirectoryName(filePaths[0]) ?? string.Empty;
                if (!string.IsNullOrWhiteSpace(parent))
                    parent = Path.GetFullPath(parent);
                var name = ResolveFolderAlbumDisplayName(parent, firstDisplay);
                snapshot = new SlideshowAlbumSnapshot(
                    SlideshowAlbumKind.Folder,
                    parent,
                    name,
                    filePaths,
                    isFileSubset: true);
                return true;
            }

            // Виртуальный search-folder без FILESYS.
            if (anySearchMs || IsWindowsDesktopBackgroundsEditing(firstEditing))
            {
                snapshot = new SlideshowAlbumSnapshot(
                    SlideshowAlbumKind.WindowsDesktopBackgrounds,
                    GetWindowsDesktopBackgroundsRoot(),
                    ResolveWindowsDesktopBackgroundsDisplayName(firstDisplay, firstEditing),
                    Array.Empty<string>(),
                    isFileSubset: false);
                return true;
            }

            return false;
        }
        catch
        {
            snapshot = default;
            return false;
        }
    }

    public static bool TryGetSlideshowOptions(out bool shuffle, out uint intervalMs)
    {
        shuffle = false;
        intervalMs = 600_000;
        try
        {
            var wall = CreateWallpaper();
            wall.GetSlideshowOptions(out var options, out intervalMs);
            shuffle = (options & DesktopSlideshowOptionsShuffleImages) != 0;
            if (intervalMs == 0)
                intervalMs = 600_000;
            return true;
        }
        catch
        {
            return false;
        }
    }

    public static void SetSlideshowOptions(bool shuffle, uint intervalMs)
    {
        try
        {
            var wall = CreateWallpaper();
            var options = shuffle ? DesktopSlideshowOptionsShuffleImages : 0;
            if (intervalMs == 0)
                intervalMs = 600_000;
            wall.SetSlideshowOptions(options, intervalMs);
        }
        catch
        {
            // ignore
        }
    }

    public static bool TrySetSlideshowFolder(string folderPath)
    {
        if (string.IsNullOrWhiteSpace(folderPath) || !Directory.Exists(folderPath))
            return false;

        IntPtr pidl = IntPtr.Zero;
        try
        {
            var full = Path.GetFullPath(folderPath);
            // Как Параметры Windows: ILCreateFromPath + SHCreateShellItemArrayFromIDLists.
            pidl = ILCreateFromPathW(full);
            if (pidl == IntPtr.Zero)
                return false;

            var pidls = new[] { pidl };
            var hr = SHCreateShellItemArrayFromIDLists(1, pidls, out var array);
            if (hr < 0 || array is null)
                return false;

            // Как Win32 «Фоновый рисунок»: SetSlideshow+Enable, без Advance
            // (Advance гоняет кадры подряд до «стабилизации»).
            return ApplySlideshowArray(array, advanceSlideshow: false);
        }
        catch
        {
            return false;
        }
        finally
        {
            if (pidl != IntPtr.Zero)
                ILFree(pidl);
        }
    }

    /// <summary>
    /// Альбом «Фоны рабочего стола Windows»: search-folder scope = %SystemRoot%\Web\Wallpaper.
    /// </summary>
    public static bool TrySetWindowsDesktopBackgroundsSlideshow(string? displayName = null)
    {
        var root = GetWindowsDesktopBackgroundsRoot();
        if (!Directory.Exists(root))
            return false;

        IntPtr scopePidl = IntPtr.Zero;
        try
        {
            scopePidl = ILCreateFromPathW(root);
            if (scopePidl == IntPtr.Zero)
                return false;

            var hr = SHCreateShellItemArrayFromIDLists(1, new[] { scopePidl }, out var scopeArray);
            if (hr < 0 || scopeArray is null)
                return false;

            var factory = (ISearchFolderItemFactory)new SearchFolderItemFactoryCtor();
            var name = string.IsNullOrWhiteSpace(displayName)
                ? ResolveWindowsDesktopBackgroundsDisplayName(null, null)
                : displayName.Trim();
            factory.SetDisplayName(name);
            factory.SetScope(scopeArray);

            var iid = IidShellItem;
            hr = factory.GetShellItem(ref iid, out var shellItemUnk);
            if (hr < 0 || shellItemUnk is null)
                return false;

            var shellItem = (IShellItem)shellItemUnk;
            var iidArray = IidShellItemArray;
            hr = SHCreateShellItemArrayFromShellItem(shellItem, ref iidArray, out var slideshowArray);
            if (hr < 0 || slideshowArray is null)
                return false;

            // Без Advance: на search-folder Advance часто обнуляет GetSlideshow → чёрный стол.
            return ApplySlideshowArray(slideshowArray, advanceSlideshow: false);
        }
        catch
        {
            return false;
        }
        finally
        {
            if (scopePidl != IntPtr.Zero)
                ILFree(scopePidl);
        }
    }

    /// <summary>
    /// Подмножество файлов альбома.
    /// Windows-альбом: items из search-folder (файлы из разных подпапок); голый FILESYS PIDL
    /// из разных директорий SetSlideshow отвергает / молча урезает.
    /// </summary>
    public static bool TrySetSlideshowImages(
        IReadOnlyList<string> imagePaths,
        SlideshowAlbumKind albumKind,
        string? albumDisplayName = null)
    {
        if (imagePaths is null || imagePaths.Count == 0)
            return false;

        var existing = new List<string>(imagePaths.Count);
        foreach (var path in imagePaths)
        {
            if (string.IsNullOrWhiteSpace(path) || !File.Exists(path))
                continue;
            existing.Add(Path.GetFullPath(path));
        }

        if (existing.Count == 0)
            return false;

        if (albumKind == SlideshowAlbumKind.WindowsDesktopBackgrounds)
        {
            var display = string.IsNullOrWhiteSpace(albumDisplayName)
                ? ResolveWindowsDesktopBackgroundsDisplayName(null, null)
                : albumDisplayName.Trim();

            // Успех только при Verify (Get = file subset). Set без Verify (search-ms)
            // оставлял чёрный стол — тогда restore полного search-folder.
            return TrySetWindowsSlideshowImagesStrict(existing, display);
        }

        return TrySetSlideshowImagesFromFilePaths(existing)
               && VerifySlideshowFileSubset(existing);
    }

    /// <summary>
    /// Windows subset: Set + обязательный Verify; при провале после Set — restore полного search-folder.
    /// </summary>
    private static bool TrySetWindowsSlideshowImagesStrict(
        IReadOnlyList<string> existing,
        string display)
    {
        if (TryWindowsSubsetPath(
                () => TrySetSlideshowImagesFromSearchFolderEnum(existing, display),
                existing,
                display))
        {
            return true;
        }

        // FILESYS PIDL (как items в GetSlideshow после Win32-subset).
        if (TryWindowsSubsetPath(
                () => TrySetSlideshowImagesFromFilePaths(existing),
                existing,
                display))
        {
            return true;
        }

        if (TryWindowsSubsetPath(
                () => TrySetSlideshowImagesViaSearchMs(existing, display),
                existing,
                display))
        {
            return true;
        }

        return false;
    }

    private static bool TryWindowsSubsetPath(
        Func<bool> setAction,
        IReadOnlyList<string> existing,
        string display)
    {
        var setOk = setAction();
        if (setOk && VerifySlideshowFileSubset(existing))
            return true;

        if (setOk)
        {
            // Set уже мог обнулить кадр — вернуть полный Windows search-folder.
            _ = TrySetWindowsDesktopBackgroundsSlideshow(display);
        }

        return false;
    }

    /// <summary>
    /// Собрать PIDL выбранных файлов как children search-folder «Фоны…» (тот же контейнер, что Win32).
    /// </summary>
    private static bool TrySetSlideshowImagesFromSearchFolderEnum(
        IReadOnlyList<string> imagePaths,
        string displayName)
    {
        if (!TryCreateWindowsSearchFolderShellItem(displayName, out var searchFolder)
            || searchFolder is null)
        {
            return false;
        }

        var wanted = new HashSet<string>(imagePaths, StringComparer.OrdinalIgnoreCase);
        var pidls = new List<IntPtr>(wanted.Count);
        try
        {
            CollectSearchScopedFilePidls(searchFolder, wanted, pidls);
            if (pidls.Count == 0)
                return false;

            var hr = SHCreateShellItemArrayFromIDLists((uint)pidls.Count, pidls.ToArray(), out var array);
            if (hr < 0 || array is null)
                return false;

            return ApplySlideshowArray(array);
        }
        catch
        {
            return false;
        }
        finally
        {
            foreach (var pidl in pidls)
            {
                if (pidl != IntPtr.Zero)
                    ILFree(pidl);
            }
        }
    }

    private static bool TryCreateWindowsSearchFolderShellItem(string displayName, out IShellItem? searchFolder)
    {
        searchFolder = null;
        var root = GetWindowsDesktopBackgroundsRoot();
        if (!Directory.Exists(root))
            return false;

        IntPtr scopePidl = IntPtr.Zero;
        try
        {
            scopePidl = ILCreateFromPathW(root);
            if (scopePidl == IntPtr.Zero)
                return false;

            var hr = SHCreateShellItemArrayFromIDLists(1, new[] { scopePidl }, out var scopeArray);
            if (hr < 0 || scopeArray is null)
                return false;

            var factory = (ISearchFolderItemFactory)new SearchFolderItemFactoryCtor();
            factory.SetDisplayName(displayName);
            factory.SetScope(scopeArray);

            var iid = IidShellItem;
            hr = factory.GetShellItem(ref iid, out var shellItemUnk);
            if (hr < 0 || shellItemUnk is null)
                return false;

            searchFolder = (IShellItem)shellItemUnk;
            return true;
        }
        catch
        {
            searchFolder = null;
            return false;
        }
        finally
        {
            if (scopePidl != IntPtr.Zero)
                ILFree(scopePidl);
        }
    }

    private static void CollectSearchScopedFilePidls(
        IShellItem folder,
        HashSet<string> wantedPaths,
        List<IntPtr> pidls)
    {
        if (wantedPaths.Count == 0)
            return;

        if (!TryBindEnumShellItems(folder, out var enumer) || enumer is null)
            return;

        try
        {
            while (enumer.Next(1, out var child, out var fetched) >= 0 && fetched == 1 && child is not null)
            {
                var fileSys = GetDisplayName(child, SigdnFileSysPath);
                if (!string.IsNullOrWhiteSpace(fileSys) && File.Exists(fileSys))
                {
                    var full = Path.GetFullPath(fileSys);
                    if (wantedPaths.Contains(full)
                        && SHGetIDListFromObject(child, out var pidl) >= 0
                        && pidl != IntPtr.Zero)
                    {
                        pidls.Add(pidl);
                        wantedPaths.Remove(full);
                        if (wantedPaths.Count == 0)
                            return;
                    }

                    continue;
                }

                // Подпапка search-scope — обойти.
                if (!string.IsNullOrWhiteSpace(fileSys) && Directory.Exists(fileSys))
                    CollectSearchScopedFilePidls(child, wantedPaths, pidls);
                else
                    CollectSearchScopedFilePidls(child, wantedPaths, pidls);

                if (wantedPaths.Count == 0)
                    return;
            }
        }
        finally
        {
            if (enumer is IDisposable disposable)
                disposable.Dispose();
            else if (Marshal.IsComObject(enumer))
                Marshal.ReleaseComObject(enumer);
        }
    }

    private static bool TryBindEnumShellItems(IShellItem folder, out IEnumShellItems? enumer)
    {
        enumer = null;
        try
        {
            var bhid = BhidEnumItems;
            var iid = IidEnumShellItems;
            folder.BindToHandler(IntPtr.Zero, ref bhid, ref iid, out var ppv);
            if (ppv == IntPtr.Zero)
                return false;

            enumer = (IEnumShellItems)Marshal.GetObjectForIUnknown(ppv);
            Marshal.Release(ppv);
            return enumer is not null;
        }
        catch
        {
            enumer = null;
            return false;
        }
    }

    private static bool TrySetSlideshowImagesViaSearchMs(
        IReadOnlyList<string> imagePaths,
        string? albumDisplayName)
    {
        var display = string.IsNullOrWhiteSpace(albumDisplayName)
            ? ResolveWindowsDesktopBackgroundsDisplayName(null, null)
            : albumDisplayName.Trim();
        var root = GetWindowsDesktopBackgroundsRoot();
        var pidls = new List<IntPtr>(imagePaths.Count);

        try
        {
            foreach (var path in imagePaths)
            {
                if (!IsUnderWindowsDesktopBackgroundsRoot(path))
                    continue;

                var fileName = Path.GetFileName(path);
                IntPtr pidl = IntPtr.Zero;
                foreach (var parsing in BuildSearchMsFileParsingNameCandidates(display, root, fileName, path))
                {
                    var iid = IidShellItem;
                    var hr = SHCreateItemFromParsingName(parsing, IntPtr.Zero, ref iid, out var item);
                    if (hr < 0 || item is null)
                        continue;

                    hr = SHGetIDListFromObject(item, out pidl);
                    if (hr >= 0 && pidl != IntPtr.Zero)
                        break;

                    pidl = IntPtr.Zero;
                }

                if (pidl == IntPtr.Zero)
                    continue;

                pidls.Add(pidl);
            }

            if (pidls.Count == 0)
                return false;

            var hrArr = SHCreateShellItemArrayFromIDLists(
                (uint)pidls.Count,
                pidls.ToArray(),
                out var array);
            if (hrArr < 0 || array is null)
                return false;

            return ApplySlideshowArray(array);
        }
        catch
        {
            return false;
        }
        finally
        {
            foreach (var pidl in pidls)
            {
                if (pidl != IntPtr.Zero)
                    ILFree(pidl);
            }
        }
    }

    private static bool TrySetSlideshowImagesFromFilePaths(IReadOnlyList<string> imagePaths)
    {
        var pidls = new List<IntPtr>(imagePaths.Count);
        try
        {
            foreach (var path in imagePaths)
            {
                var pidl = ILCreateFromPathW(path);
                if (pidl == IntPtr.Zero)
                    continue;
                pidls.Add(pidl);
            }

            if (pidls.Count == 0)
                return false;

            var hr = SHCreateShellItemArrayFromIDLists((uint)pidls.Count, pidls.ToArray(), out var array);
            if (hr < 0 || array is null)
                return false;

            return ApplySlideshowArray(array);
        }
        catch
        {
            return false;
        }
        finally
        {
            foreach (var pidl in pidls)
            {
                if (pidl != IntPtr.Zero)
                    ILFree(pidl);
            }
        }
    }

    /// <summary>После SetSlideshow — GetSlideshow FILESYS должен совпасть с запрошенным subset.</summary>
    private static bool VerifySlideshowFileSubset(IReadOnlyList<string> expectedPaths)
    {
        if (!TryGetSlideshowAlbum(out var snap)
            || !snap.IsFileSubset
            || snap.ImagePaths.Count == 0)
        {
            return false;
        }

        var live = new HashSet<string>(
            snap.ImagePaths.Select(Path.GetFullPath),
            StringComparer.OrdinalIgnoreCase);
        foreach (var path in expectedPaths)
        {
            if (!live.Contains(Path.GetFullPath(path)))
                return false;
        }

        return live.Count == expectedPaths.Count;
    }

    /// <param name="advanceSlideshow">
    /// По умолчанию false — как диалог Win32. true только если нужен явный шаг кадра.
    /// </param>
    private static bool ApplySlideshowArray(IShellItemArray array, bool advanceSlideshow = false)
    {
        var wall = CreateWallpaper();
        wall.SetSlideshow(array);
        wall.Enable(true);
        if (!advanceSlideshow)
            return true;

        try
        {
            wall.AdvanceSlideshow(null, DesktopSlideshowDirectionForward);
        }
        catch
        {
            // ignore
        }

        return true;
    }

    /// <summary>
    /// Статичная обоина. SetWallpaper сам выключает слайдшоу (не путать с Enable(false) — тот гасит весь фон).
    /// </summary>
    public static bool TrySetWallpaper(string imagePath)
    {
        if (string.IsNullOrWhiteSpace(imagePath) || !File.Exists(imagePath))
            return false;

        try
        {
            var full = Path.GetFullPath(imagePath);
            var wall = CreateWallpaper();
            wall.Enable(true);
            wall.SetWallpaper(null, full);
            return true;
        }
        catch
        {
            return false;
        }
    }

    /// <summary>Сплошной цвет: COLORREF (0x00BBGGRR).</summary>
    public static bool TryGetBackgroundColor(out uint colorRef)
    {
        colorRef = 0;
        try
        {
            CreateWallpaper().GetBackgroundColor(out colorRef);
            return true;
        }
        catch
        {
            return false;
        }
    }

    public static bool TrySetSolidBackground(uint colorRef)
    {
        try
        {
            var wall = CreateWallpaper();
            wall.Enable(true);
            wall.SetBackgroundColor(colorRef);
            wall.SetWallpaper(null, string.Empty);
            return true;
        }
        catch
        {
            return false;
        }
    }

    public static void EnableDesktopBackground(bool enable)
    {
        try
        {
            CreateWallpaper().Enable(enable);
        }
        catch
        {
            // ignore
        }
    }

    private static string ResolveWindowsDesktopBackgroundsDisplayName(string? display, string? editing)
    {
        var fromSearch = ExtractSearchMsDisplayName(editing) ?? ExtractSearchMsDisplayName(display);
        if (!string.IsNullOrWhiteSpace(fromSearch))
            return fromSearch!;

        // Локализованное имя как в Параметрах (ru / en).
        try
        {
            var ui = System.Globalization.CultureInfo.CurrentUICulture.TwoLetterISOLanguageName;
            if (ui.Equals("ru", StringComparison.OrdinalIgnoreCase))
                return "Фоны рабочего стола Windows";
        }
        catch
        {
            // ignore
        }

        return "Windows Desktop Backgrounds";
    }

    private static string? ExtractSearchMsDisplayName(string? value)
    {
        if (string.IsNullOrWhiteSpace(value) || !IsSearchMs(value))
            return null;

        const string key = "displayname=";
        var idx = value.IndexOf(key, StringComparison.OrdinalIgnoreCase);
        if (idx < 0)
            return null;

        var start = idx + key.Length;
        var end = value.IndexOf('&', start);
        var raw = end >= 0 ? value[start..end] : value[start..];
        try
        {
            return Uri.UnescapeDataString(raw.Replace('+', ' '));
        }
        catch
        {
            return raw;
        }
    }

    private static bool IsSearchMs(string? value) =>
        !string.IsNullOrWhiteSpace(value)
        && value.StartsWith("search-ms:", StringComparison.OrdinalIgnoreCase);

    private static bool IsWindowsDesktopBackgroundsEditing(string? value)
    {
        if (string.IsNullOrWhiteSpace(value))
            return false;

        if (value.IndexOf(@"\Web\Wallpaper", StringComparison.OrdinalIgnoreCase) < 0
            && value.IndexOf("/Web/Wallpaper", StringComparison.OrdinalIgnoreCase) < 0)
        {
            return false;
        }

        return IsSearchMs(value)
               || value.IndexOf("location:", StringComparison.OrdinalIgnoreCase) >= 0;
    }

    /// <summary>
    /// Варианты search-ms как в pageWallpaper (ru: имяфайла:(…); en: filename:(…)).
    /// </summary>
    private static IEnumerable<string> BuildSearchMsFileParsingNameCandidates(
        string displayName,
        string wallpaperRoot,
        string fileName,
        string fullPath)
    {
        _ = wallpaperRoot;
        var dn = Uri.EscapeDataString(displayName);
        var fn = Uri.EscapeDataString(fileName);
        var loc = Uri.EscapeDataString(fullPath);
        yield return $"search-ms:displayname={dn}&crumb=имяфайла:({fn})&crumb=location:{loc}";
        yield return $"search-ms:displayname={dn}&crumb=filename:({fn})&crumb=location:{loc}";
        yield return $"search-ms:displayname={dn}&crumb=имяфайла:{fn}&crumb=location:{loc}";
        yield return $"search-ms:displayname={dn}&crumb=filename:{fn}&crumb=location:{loc}";
    }

    private static string GetDisplayName(IShellItem item, uint sigdn)
    {
        // Search-folder / virtual PIDL: SIGDN_FILESYSPATH часто E_INVALIDARG →
        // ArgumentException валил весь TryGetSlideshowAlbum → ложный fallback на Nature.
        try
        {
            item.GetDisplayName(sigdn, out var ptr);
            try
            {
                return ptr != IntPtr.Zero ? Marshal.PtrToStringUni(ptr) ?? string.Empty : string.Empty;
            }
            finally
            {
                if (ptr != IntPtr.Zero)
                    Marshal.FreeCoTaskMem(ptr);
            }
        }
        catch (Exception)
        {
            // E_INVALIDARG / E_NOTIMPL / прочее на virtual PIDL — не валить GetSlideshow.
            return string.Empty;
        }
    }

    private static IDesktopWallpaper CreateWallpaper() =>
        (IDesktopWallpaper)new DesktopWallpaperCtor();

    [DllImport("shell32.dll", CharSet = CharSet.Unicode)]
    private static extern IntPtr ILCreateFromPathW(string pszPath);

    [DllImport("shell32.dll")]
    private static extern void ILFree(IntPtr pidl);

    [DllImport("shell32.dll")]
    private static extern int SHCreateShellItemArrayFromIDLists(
        uint cidl,
        [In] IntPtr[] rgpidl,
        out IShellItemArray ppsiItemArray);

    [DllImport("shell32.dll", CharSet = CharSet.Unicode)]
    private static extern int SHCreateItemFromParsingName(
        string pszPath,
        IntPtr pbc,
        ref Guid riid,
        out IShellItem ppv);

    [DllImport("shell32.dll")]
    private static extern int SHCreateShellItemArrayFromShellItem(
        IShellItem psi,
        ref Guid riid,
        out IShellItemArray ppsia);

    [DllImport("shell32.dll")]
    private static extern int SHGetIDListFromObject(
        [MarshalAs(UnmanagedType.IUnknown)] object punk,
        out IntPtr ppidl);

    [ComImport]
    [Guid("43826D1E-E718-42EE-BC55-A1E261C37BFE")]
    [InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
    private interface IShellItem
    {
        void BindToHandler(IntPtr pbc, ref Guid bhid, ref Guid riid, out IntPtr ppv);
        void GetParent(out IShellItem ppsi);
        void GetDisplayName(uint sigdnName, out IntPtr ppszName);
        void GetAttributes(uint sfgaoMask, out uint psfgaoAttribs);
        void Compare(IShellItem psi, uint hint, out int piOrder);
    }

    [ComImport]
    [Guid("b63ea76d-1f85-456f-a19c-48159efa858b")]
    [InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
    private interface IShellItemArray
    {
        void BindToHandler(IntPtr pbc, ref Guid bhid, ref Guid riid, out IntPtr ppvOut);
        void GetPropertyStore(int flags, ref Guid riid, out IntPtr ppv);
        void GetPropertyDescriptionList(ref PropertyKey keyType, ref Guid riid, out IntPtr ppv);
        void GetAttributes(int attribFlags, uint sfgaoMask, out uint psfgaoAttribs);
        void GetCount(out uint pdwNumItems);
        void GetItemAt(uint dwIndex, out IShellItem ppsi);
        void EnumItems(out IntPtr ppenumShellItems);
    }

    [ComImport]
    [Guid("70629033-e363-4a28-a567-0db78006e6d7")]
    [InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
    private interface IEnumShellItems
    {
        [PreserveSig]
        int Next(uint celt, out IShellItem rgelt, out uint pceltFetched);

        [PreserveSig]
        int Skip(uint celt);

        [PreserveSig]
        int Reset();

        [PreserveSig]
        int Clone(out IEnumShellItems ppenum);
    }

    /// <summary>ISearchFolderItemFactory — только нужные методы (PreserveSig для HRESULT).</summary>
    [ComImport]
    [Guid("a0ffbc28-5482-4366-be27-3e81e78e06c2")]
    [InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
    private interface ISearchFolderItemFactory
    {
        [PreserveSig]
        int SetDisplayName([MarshalAs(UnmanagedType.LPWStr)] string pszDisplayName);

        [PreserveSig]
        int SetFolderTypeID(ref Guid ftid);

        [PreserveSig]
        int SetFolderLogicalViewMode(int flvm);

        [PreserveSig]
        int SetIconSize(int iIconSize);

        [PreserveSig]
        int SetVisibleColumns(uint cVisibleColumns, IntPtr rgKey);

        [PreserveSig]
        int SetSortColumns(uint cSortColumns, IntPtr rgSortColumns);

        [PreserveSig]
        int SetGroupColumn(ref PropertyKey keyGroup);

        [PreserveSig]
        int SetStacks(uint cStackKeys, IntPtr rgStackKeys);

        [PreserveSig]
        int SetScope(IShellItemArray psiaScope);

        [PreserveSig]
        int SetCondition(IntPtr pCondition);

        [PreserveSig]
        int GetShellItem(ref Guid riid, [MarshalAs(UnmanagedType.IUnknown)] out object ppv);

        [PreserveSig]
        int GetIDList(out IntPtr ppidl);
    }

    [ComImport]
    [Guid("14010e02-bbbd-41f0-88e3-eda371216584")]
    private class SearchFolderItemFactoryCtor
    {
    }

    [StructLayout(LayoutKind.Sequential, Pack = 4)]
    private struct PropertyKey
    {
        public Guid Fmtid;
        public uint Pid;
    }

    [ComImport]
    [Guid("B92B56A9-8B55-4E14-9A89-0199BBB6F93B")]
    [InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
    private interface IDesktopWallpaper
    {
        void SetWallpaper([MarshalAs(UnmanagedType.LPWStr)] string? monitorID, [MarshalAs(UnmanagedType.LPWStr)] string wallpaper);
        void GetWallpaper([MarshalAs(UnmanagedType.LPWStr)] string? monitorID, out IntPtr wallpaper);
        void GetMonitorDevicePathAt(uint monitorIndex, out IntPtr monitorID);
        void GetMonitorDevicePathCount(out uint count);
        void GetMonitorRECT([MarshalAs(UnmanagedType.LPWStr)] string monitorID, out Rect displayRect);
        void SetBackgroundColor(uint color);
        void GetBackgroundColor(out uint color);
        void SetPosition(int position);
        void GetPosition(out int position);
        void SetSlideshow(IShellItemArray items);
        void GetSlideshow(out IShellItemArray items);
        void SetSlideshowOptions(int options, uint slideshowTick);
        void GetSlideshowOptions(out int options, out uint slideshowTick);
        void AdvanceSlideshow([MarshalAs(UnmanagedType.LPWStr)] string? monitorID, int direction);
        void GetStatus(out int state);
        void Enable([MarshalAs(UnmanagedType.Bool)] bool enable);
    }

    [StructLayout(LayoutKind.Sequential)]
    private struct Rect
    {
        public int Left;
        public int Top;
        public int Right;
        public int Bottom;
    }

    [ComImport]
    [Guid("C2CF3110-460E-4fc1-B9D0-8A1C0C9CC4BD")]
    private class DesktopWallpaperCtor
    {
    }
}
