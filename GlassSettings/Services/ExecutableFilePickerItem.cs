using System.ComponentModel;
using System.Runtime.CompilerServices;
using GlassSettings.Strings;
using Microsoft.UI.Xaml.Media;

namespace GlassSettings.Services;

/// <summary>Строка списка в hosted file picker (папка или исполняемый файл).</summary>
public sealed class ExecutableFilePickerItem : INotifyPropertyChanged
{
    private ImageSource? _icon;

    public required string Name { get; init; }

    public required string FullPath { get; init; }

    public bool IsFolder { get; init; }

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

    public string? TypeLabel { get; init; }

    public DateTime? DateModifiedUtc { get; init; }

    public long? SizeBytes { get; init; }

    public string DetailLine => IsFolder
        ? GlassSettingsResources.Get("Attach_FilePickerFolderType")
        : TypeLabel ?? FullPath;

    public event PropertyChangedEventHandler? PropertyChanged;

    private void OnPropertyChanged([CallerMemberName] string? propertyName = null) =>
        PropertyChanged?.Invoke(this, new PropertyChangedEventArgs(propertyName));
}

/// <summary>Быстрый переход (Desktop, Documents, диск и т.д.).</summary>
public sealed class ExecutableFilePickerQuickLink
{
    public required string DisplayName { get; init; }

    public required string Path { get; init; }
}

/// <summary>Пункт ComboBox фильтра по расширению.</summary>
public sealed class ExecutableFilePickerExtensionFilterItem
{
    /// <summary>Одно расширение (exe-режим). Игнорируется, если задан <see cref="Extensions"/>.</summary>
    public string? Extension { get; init; }

    /// <summary>Набор расширений (напр. все изображения). null = все файлы.</summary>
    public IReadOnlyList<string>? Extensions { get; init; }

    public required string DisplayName { get; init; }

    /// <summary>null = без ограничения; иначе allow-list расширений (с точкой).</summary>
    public IReadOnlyList<string>? ResolveExtensions()
    {
        if (Extensions is not null)
            return Extensions;

        return Extension is null ? null : [Extension];
    }
}
