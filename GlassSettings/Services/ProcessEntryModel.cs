using System.ComponentModel;
using System.IO;
using System.Runtime.CompilerServices;
using GlassSettings.Core.Registry;
using Microsoft.UI.Xaml.Media;

namespace GlassSettings.Services;

public sealed class ProcessEntryModel : INotifyPropertyChanged
{
    private const string DetailSeparator = " | ";

    public ProcessEntryModel(
        string exeName,
        AttachProcessMode mode,
        string? productName,
        string? fileDescription,
        string? resolvedPath,
        string? companyName,
        string? fileVersion,
        string? fileModifiedLabel)
    {
        ExeName = exeName;
        ProductName = productName;
        FileDescription = fileDescription;
        ResolvedPath = resolvedPath;
        CompanyName = companyName;
        FileVersion = fileVersion;
        FileModifiedLabel = fileModifiedLabel;
        DisplayName = ResolveDisplayName(productName, fileDescription, exeName);
        Mode = mode;
    }

    public ProcessSection Section { get; init; } = ProcessSection.AttachList;

    public bool IsBuiltInBlockList { get; init; }

    public string ExeName { get; }

    public string DisplayName { get; }

    public string? ProductName { get; }

    public AttachProcessMode Mode { get; private set; }

    public string? FileDescription { get; }

    public string? ResolvedPath { get; }

    public string? CompanyName { get; }

    public string? FileVersion { get; }

    public string? FileModifiedLabel { get; }

    private ImageSource? _icon;

    public ImageSource? Icon
    {
        get => _icon;
        set
        {
            if (ReferenceEquals(_icon, value))
                return;
            _icon = value;
            OnPropertyChanged();
        }
    }

    public bool IsForbidden => Mode == AttachProcessMode.Forbidden;

    private bool _isListHighlighted;

    /// <summary>Подсветка после добавления процесса в AttachList.</summary>
    public bool IsListHighlighted => _isListHighlighted;

    public void SetListHighlighted(bool highlighted)
    {
        if (_isListHighlighted == highlighted)
            return;

        _isListHighlighted = highlighted;
        OnPropertyChanged(nameof(IsListHighlighted));
    }

    public string ModeLabel => AttachListRegistry.ModeLabel(Mode);

    /// <summary>Заголовок в представлении «Список» — описание и при необходимости имя exe в скобках.</summary>
    public string ListDisplayTitle =>
        ShouldAppendExeNameToListTitle() ? $"{DisplayName} ({ExeName})" : DisplayName;

    /// <summary>Строка под заголовком в представлении «Список».</summary>
    public string ListDetailLine
    {
        get
        {
            var description = string.Equals(FileDescription, DisplayName, StringComparison.OrdinalIgnoreCase)
                ? null
                : FileDescription;

            if (Section == ProcessSection.BlockList)
                return JoinDetailParts(CompanyName, description);

            return JoinDetailParts(ModeLabel, CompanyName, description);
        }
    }

    /// <summary>Строка под заголовком в представлении «Плитки» — только разработчик.</summary>
    public string? GridCompanyLine =>
        string.IsNullOrWhiteSpace(CompanyName) ? null : CompanyName;

    public void SetMode(AttachProcessMode mode)
    {
        if (Mode == mode)
            return;
        Mode = mode;
        OnPropertyChanged(nameof(Mode));
        OnPropertyChanged(nameof(ModeLabel));
        OnPropertyChanged(nameof(ListDetailLine));
        OnPropertyChanged(nameof(IsForbidden));
    }

    public event PropertyChangedEventHandler? PropertyChanged;

    private void OnPropertyChanged([CallerMemberName] string? propertyName = null) =>
        PropertyChanged?.Invoke(this, new PropertyChangedEventArgs(propertyName));

    private bool ShouldAppendExeNameToListTitle()
    {
        if (string.IsNullOrWhiteSpace(ExeName))
            return false;

        if (string.Equals(DisplayName, ExeName, StringComparison.OrdinalIgnoreCase))
            return false;

        var nameWithoutExtension = Path.GetFileNameWithoutExtension(ExeName);
        if (string.Equals(DisplayName, nameWithoutExtension, StringComparison.OrdinalIgnoreCase))
            return false;

        return true;
    }

    internal static string ResolveDisplayName(string? productName, string? fileDescription, string exeName)
    {
        if (!string.IsNullOrWhiteSpace(fileDescription) && !IsGenericWindowsVersionLabel(fileDescription))
            return fileDescription.Trim();
        if (!string.IsNullOrWhiteSpace(productName) && !IsGenericWindowsVersionLabel(productName))
            return productName.Trim();
        if (exeName.EndsWith(".exe", StringComparison.OrdinalIgnoreCase))
            return Path.GetFileNameWithoutExtension(exeName);
        return exeName;
    }

    /// <summary>ProductName/FileDescription часто «Microsoft Windows Operating System» — не имя приложения.</summary>
    private static bool IsGenericWindowsVersionLabel(string value)
    {
        var text = value.Trim();
        if (text.Length == 0)
            return true;

        if (text.Contains("Операционная система", StringComparison.OrdinalIgnoreCase))
            return true;
        if (text.Contains("Operating System", StringComparison.OrdinalIgnoreCase))
            return true;
        if (text.Contains("Microsoft® Windows®", StringComparison.OrdinalIgnoreCase))
            return true;
        if (text.Contains("Microsoft Windows", StringComparison.OrdinalIgnoreCase)
            && text.Contains("Operating", StringComparison.OrdinalIgnoreCase))
            return true;

        return false;
    }

    private static string JoinDetailParts(params string?[] parts)
    {
        var segments = parts.Where(static p => !string.IsNullOrWhiteSpace(p)).ToArray();
        return segments.Length == 0 ? string.Empty : string.Join(DetailSeparator, segments);
    }

    /// <summary>Microsoft в CompanyName или типичный путь системного exe.</summary>
    internal static bool IsMicrosoftPublisher(string? companyName, string? resolvedPath)
    {
        if (!string.IsNullOrWhiteSpace(companyName)
            && companyName.Contains("Microsoft", StringComparison.OrdinalIgnoreCase))
            return true;

        if (string.IsNullOrWhiteSpace(resolvedPath))
            return false;

        var path = resolvedPath.Trim();
        return path.Contains(@"\Windows\System32\", StringComparison.OrdinalIgnoreCase)
               || path.Contains(@"\Windows\SysWOW64\", StringComparison.OrdinalIgnoreCase)
               || path.Contains(@"\Windows\SystemApps\", StringComparison.OrdinalIgnoreCase)
               || path.Contains(@"\Windows\ImmersiveControlPanel\", StringComparison.OrdinalIgnoreCase);
    }
}
