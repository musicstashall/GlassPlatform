using System.ComponentModel;
using System.Runtime.CompilerServices;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Media;

namespace GlassSettings.Controls.Themes;

/// <summary>Группа плиток альбома (подпапка или одна FS-папка).</summary>
public sealed class SlideshowAlbumGroup
{
    public SlideshowAlbumGroup(string folderName, IList<SlideshowAlbumImageItem> images)
    {
        FolderName = folderName;
        Images = images;
    }

    public string FolderName { get; }

    public IList<SlideshowAlbumImageItem> Images { get; }
}

/// <summary>Плитка изображения альбома слайдшоу.</summary>
public sealed class SlideshowAlbumImageItem : INotifyPropertyChanged
{
    private bool _isIncluded;
    private ImageSource? _thumbnail;

    public SlideshowAlbumImageItem(string fullPath, bool isIncluded)
    {
        FullPath = fullPath;
        FileName = Path.GetFileName(fullPath);
        FolderKey = Path.GetDirectoryName(fullPath) ?? string.Empty;
        _isIncluded = isIncluded;
    }

    public string FullPath { get; }

    public string FileName { get; }

    public string FolderKey { get; }

    public bool IsIncluded
    {
        get => _isIncluded;
        set
        {
            if (_isIncluded == value)
                return;
            _isIncluded = value;
            OnPropertyChanged();
            InclusionChanged?.Invoke(this, _isIncluded);
        }
    }

    public ImageSource? Thumbnail
    {
        get => _thumbnail;
        set
        {
            if (ReferenceEquals(_thumbnail, value))
                return;
            _thumbnail = value;
            OnPropertyChanged();
            OnPropertyChanged(nameof(ThumbnailVisibility));
            OnPropertyChanged(nameof(PlaceholderVisibility));
            OnPropertyChanged(nameof(LoadingRingVisibility));
        }
    }

    /// <summary>Превью загружено.</summary>
    public Visibility ThumbnailVisibility =>
        _thumbnail is null ? Visibility.Collapsed : Visibility.Visible;

    /// <summary>Иконка-палитра, пока нет превью.</summary>
    public Visibility PlaceholderVisibility =>
        _thumbnail is null ? Visibility.Visible : Visibility.Collapsed;

    /// <summary>Бублик на плитке, пока грузится превью.</summary>
    public Visibility LoadingRingVisibility =>
        _thumbnail is null ? Visibility.Visible : Visibility.Collapsed;

    public event PropertyChangedEventHandler? PropertyChanged;

    public event Action<SlideshowAlbumImageItem, bool>? InclusionChanged;

    /// <summary>Установить галочку без InclusionChanged (откат «последней»).</summary>
    public void SetIncludedSilent(bool value)
    {
        if (_isIncluded == value)
            return;
        _isIncluded = value;
        OnPropertyChanged(nameof(IsIncluded));
    }

    private void OnPropertyChanged([CallerMemberName] string? name = null) =>
        PropertyChanged?.Invoke(this, new PropertyChangedEventArgs(name));
}
