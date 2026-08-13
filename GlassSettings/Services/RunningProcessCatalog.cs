using System.Diagnostics;
using GlassSettings.Core.Registry;
namespace GlassSettings.Services;

/// <summary>Снимок запущенных процессов — уникальные exe, отсортированные по имени.</summary>
public static class RunningProcessCatalog
{
    private static readonly Lazy<string> CurrentAttachExeName = new(GetCurrentAttachExeName);

    public static Task<IReadOnlyList<RunningProcessInfo>> GetRunningProcessesAsync() =>
        Task.Run(() => GetRunningProcesses(ProcessPickerTarget.AttachList));

    public static Task<IReadOnlyList<RunningProcessInfo>> GetRunningProcessesForBlockListAsync() =>
        Task.Run(() => GetRunningProcesses(ProcessPickerTarget.BlockList));

    public static IReadOnlyList<RunningProcessInfo> GetRunningProcesses(ProcessPickerTarget target)
    {
        var byExe = new Dictionary<string, RunningProcessInfo>(StringComparer.OrdinalIgnoreCase);
        var currentSessionId = Process.GetCurrentProcess().SessionId;
        var currentExeName = CurrentAttachExeName.Value;

        foreach (var process in Process.GetProcesses())
        {
            try
            {
                if (!ShouldIncludeProcess(process, currentSessionId, currentExeName))
                    continue;

                var exeName = ToAttachExeName(process.ProcessName);
                if (string.IsNullOrEmpty(exeName))
                    continue;

                if (ShouldExcludeFromPicker(exeName, target))
                    continue;

                string? path = null;
                try
                {
                    path = process.MainModule?.FileName;
                }
                catch
                {
                    // доступ к модулю запрещён
                }

                if (byExe.TryGetValue(exeName, out var existing))
                {
                    if (string.IsNullOrEmpty(existing.FilePath) && !string.IsNullOrEmpty(path))
                        byExe[exeName] = CreateInfo(exeName, path);
                    continue;
                }

                byExe[exeName] = CreateInfo(exeName, path);
            }
            catch
            {
                // процесс завершился или недоступен
            }
            finally
            {
                process.Dispose();
            }
        }

        return byExe.Values
            .OrderBy(static p => p.DisplayName, StringComparer.OrdinalIgnoreCase)
            .ToList();
    }

    private static string GetCurrentAttachExeName()
    {
        var path = Environment.ProcessPath;
        if (!string.IsNullOrWhiteSpace(path))
            return ToAttachExeName(Path.GetFileName(path));

        return ToAttachExeName(Process.GetCurrentProcess().ProcessName);
    }

    /// <summary>Исключаем службы (session 0) и текущий процесс GlassSettings.</summary>
    private static bool ShouldIncludeProcess(Process process, int currentSessionId, string currentExeName)
    {
        try
        {
            if (process.SessionId == 0)
                return false;

            if (process.SessionId != currentSessionId)
                return false;

            if (process.Id == Environment.ProcessId)
                return false;

            var exeName = ToAttachExeName(process.ProcessName);
            if (string.IsNullOrEmpty(exeName))
                return false;

            return !string.Equals(exeName, currentExeName, StringComparison.OrdinalIgnoreCase);
        }
        catch
        {
            return false;
        }
    }

    private static bool ShouldExcludeFromPicker(string exeName, ProcessPickerTarget target) =>
        target switch
        {
            ProcessPickerTarget.BlockList => BlockListRegistry.IsBlocked(exeName),
            _ => ProcessPickerExclusions.IsExcluded(exeName)
        };

    internal static string ToAttachExeName(string processName)
    {
        if (string.IsNullOrWhiteSpace(processName))
            return string.Empty;

        var name = processName.Trim();
        if (!name.EndsWith(".exe", StringComparison.OrdinalIgnoreCase))
            name += ".exe";

        return AttachListRegistry.NormalizeExeName(name);
    }

    private static RunningProcessInfo CreateInfo(string exeName, string? path)
    {
        string? product = null;
        string? description = null;
        string? company = null;

        if (!string.IsNullOrEmpty(path))
        {
            try
            {
                var version = FileVersionInfo.GetVersionInfo(path);
                product = string.IsNullOrWhiteSpace(version.ProductName) ? null : version.ProductName.Trim();
                description = string.IsNullOrWhiteSpace(version.FileDescription) ? null : version.FileDescription.Trim();
                company = string.IsNullOrWhiteSpace(version.CompanyName) ? null : version.CompanyName.Trim();
            }
            catch
            {
                // version resource недоступен
            }
        }

        var displayName = ProcessEntryModel.ResolveDisplayName(product, description, exeName);
        if (string.IsNullOrWhiteSpace(displayName))
            displayName = Path.GetFileNameWithoutExtension(exeName);

        var detailLine = BuildDetailLine(displayName, exeName, description, company);
        return new RunningProcessInfo(exeName, path, displayName, description, company, detailLine);
    }

    private static string BuildDetailLine(string displayName, string exeName, string? description, string? company)
    {
        var parts = new List<string> { exeName };

        if (!string.IsNullOrWhiteSpace(company))
            parts.Add(company);

        var desc = string.Equals(description, displayName, StringComparison.OrdinalIgnoreCase)
            ? null
            : description;
        if (!string.IsNullOrWhiteSpace(desc))
            parts.Add(desc);

        return string.Join(" | ", parts);
    }
}
