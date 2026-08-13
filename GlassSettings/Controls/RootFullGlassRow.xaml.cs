using GlassSettings.Core.Customization;
using GlassSettings.Core.Platform;
using GlassSettings.Core.Registry;
using GlassSettings.Core.Snapshot;
using GlassSettings.Services;
using GlassSettings.Strings;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;
using Microsoft.UI.Xaml.Controls.Primitives;
using Microsoft.UI.Xaml.Input;

namespace GlassSettings.Controls;

/// <summary>ToggleButton «Сплошное стекло» → Root\NoBackdrop (1 или ключ удалён).</summary>
public sealed partial class RootFullGlassRow : UserControl, ISettingsUnsavedScopeHost
{
    private readonly ToggleButton _fullGlassToggle;
    private bool _loading;

    public RootFullGlassRow()
    {
        InitializeComponent();

        _fullGlassToggle = new ToggleButton
        {
            MinWidth = 140,
            Content = GlassSettingsResources.Get("Root_FullGlassToggle"),
            IsChecked = true,
        };
        _fullGlassToggle.Checked += (_, _) => OnToggleCommitted(true);
        _fullGlassToggle.Unchecked += (_, _) => OnToggleCommitted(false);

        HeaderChrome.HeaderActions = _fullGlassToggle;
        UnsavedScope = SettingsCommitScope.ForArea(CustomizationAreaId.Root);

        RootBorder.PointerEntered += RootBorder_OnPointerEntered;
        RootBorder.PointerExited += RootBorder_OnPointerExited;
        Loaded += OnLoaded;
        Unloaded += OnUnloaded;
    }

    public ToggleButton FullGlassToggle => _fullGlassToggle;

    public SettingsCommitScope? UnsavedScope { get; set; }

    public bool IsUnsavedPending
    {
        get => HeaderChrome.IsUnsavedPending;
        set => HeaderChrome.IsUnsavedPending = value;
    }

    /// <summary>Ключ отсутствует или NoBackdrop=1 → сплошное стекло вкл/выкл.</summary>
    public bool IsFullGlassEnabled => _fullGlassToggle.IsChecked == true;

    public event EventHandler? FullGlassChanged;

    public void RefreshUnsavedIndicator()
    {
        IsUnsavedPending = UnsavedScope is { } scope && SettingsSession.Current.HasUnsavedInScope(scope);
    }

    public void SyncFromRegistry()
    {
        _loading = true;
        try
        {
            _fullGlassToggle.IsChecked = RootNoBackdropPolicy.IsFullGlassEnabled;
        }
        finally
        {
            _loading = false;
            FullGlassChanged?.Invoke(this, EventArgs.Empty);
        }
    }

    public void ApplyLocalizedStrings(string title, string? description = null)
    {
        HeaderChrome.Title = title;
        HeaderChrome.Description = description ?? string.Empty;
        _fullGlassToggle.Content = GlassSettingsResources.Get("Root_FullGlassToggle");
    }

    private void OnLoaded(object sender, RoutedEventArgs e)
    {
        SettingsSession.Current.DirtyChanged += Session_OnDirtyChanged;
        SyncFromRegistry();
        RefreshUnsavedIndicator();
    }

    private void OnUnloaded(object sender, RoutedEventArgs e) =>
        SettingsSession.Current.DirtyChanged -= Session_OnDirtyChanged;

    private void Session_OnDirtyChanged(object? sender, EventArgs e) =>
        RefreshUnsavedIndicator();

    private void OnToggleCommitted(bool fullGlassOn)
    {
        if (_loading)
            return;

        if (fullGlassOn)
            SettingsSession.Current.DeleteGlassValue("Root", "NoBackdrop", liveFlyout: false);
        else
            SettingsSession.Current.WriteGlassDword("Root", "NoBackdrop", 1);

        RootBackdropLiveReload.NotifyNoBackdropChange(fullGlassOn ? 0u : 1u);
        FullGlassChanged?.Invoke(this, EventArgs.Empty);
    }

    private void RootBorder_OnPointerEntered(object sender, PointerRoutedEventArgs e) =>
        VisualStateManager.GoToState(this, "PointerOver", true);

    private void RootBorder_OnPointerExited(object sender, PointerRoutedEventArgs e) =>
        VisualStateManager.GoToState(this, "Normal", true);
}
