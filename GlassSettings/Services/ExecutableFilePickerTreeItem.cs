using System.Collections.ObjectModel;
using System.ComponentModel;
using System.Runtime.CompilerServices;
using Microsoft.UI.Xaml.Media;

namespace GlassSettings.Services;

/// <summary>Узел иерархического TreeView в file picker (официальный ItemsSource-паттерн WinUI).</summary>
public sealed class ExecutableFilePickerTreeItem : INotifyPropertyChanged
{
    private bool _isExpanded;
    private ImageSource? _icon;

    public string Name { get; set; } = string.Empty;

    public string? FullPath { get; set; }

    public ExecutableFilePickerTreeItemKind Kind { get; set; }

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

    private bool _hasUnloadedChildren;

    public bool HasUnloadedChildren
    {
        get => _hasUnloadedChildren;
        set
        {
            if (_hasUnloadedChildren == value)
                return;

            _hasUnloadedChildren = value;
            OnPropertyChanged();
        }
    }

    public bool IsExpanded
    {
        get => _isExpanded;
        set
        {
            if (_isExpanded == value)
                return;

            _isExpanded = value;
            OnPropertyChanged();
        }
    }

    public ObservableCollection<ExecutableFilePickerTreeItem> Children { get; } = new();

    public bool IsNavigableFolder =>
        Kind is ExecutableFilePickerTreeItemKind.Folder or ExecutableFilePickerTreeItemKind.Drive
        && !string.IsNullOrWhiteSpace(FullPath);

    public event PropertyChangedEventHandler? PropertyChanged;

    private void OnPropertyChanged([CallerMemberName] string? propertyName = null) =>
        PropertyChanged?.Invoke(this, new PropertyChangedEventArgs(propertyName));
}

public enum ExecutableFilePickerTreeItemKind
{
    QuickAccessGroup,
    ThisComputer,
    Drive,
    Folder,
    LoadingPlaceholder
}
