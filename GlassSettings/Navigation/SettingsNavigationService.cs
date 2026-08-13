using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;
using Microsoft.UI.Xaml.Media.Animation;

namespace GlassSettings.Navigation;

public sealed class SettingsNavigationService
{
    public static SettingsNavigationService Current { get; } = new();

    private Frame? _frame;
    private readonly List<SettingsBreadcrumbItem> _trail = new();
    private readonly Stack<SettingsNavigationSnapshot> _globalBackStack = new();
    private bool _isRestoring;
    /// <summary>До первого Frame.Navigate BackStack на x86 WinUI может AV при обращении.</summary>
    private bool _frameHasNavigated;

    public IReadOnlyList<SettingsBreadcrumbItem> Trail => _trail;

    public event EventHandler? TrailChanged;

    public string? CurrentSidebarTag { get; set; }

    public bool CanPop => Trail.Count > 1;

    public bool IsDrillActive => Trail.Count > 1;

    public bool CanGoBackGlobal => _globalBackStack.Count > 0;

    public void Attach(Frame frame) => _frame = frame;

    /// <summary>Запомнить текущее состояние перед переходом вперёд.</summary>
    public void RememberForBack(string? sidebarTag)
    {
        if (_isRestoring || _frame is null || _trail.Count == 0)
            return;

        _globalBackStack.Push(CreateSnapshot(sidebarTag));
    }

    public bool TryGoBackGlobal(out SettingsNavigationSnapshot? restored)
    {
        restored = null;
        if (_frame is null || _globalBackStack.Count == 0)
            return false;

        restored = _globalBackStack.Pop();
        RestoreSnapshot(restored);
        return true;
    }

    public void ResetTrail(string rootTitle, Type rootPageType, object? rootParameter = null, bool raiseTrailChanged = true)
    {
        var resolvedTitle = BreadcrumbTitleResolver.Resolve(rootPageType, rootParameter, rootTitle);
        _trail.Clear();
        _trail.Add(new SettingsBreadcrumbItem(resolvedTitle, rootPageType, rootParameter));
        if (!raiseTrailChanged)
        {
            return;
        }

        TrailChanged?.Invoke(this, EventArgs.Empty);
    }

    /// <summary>Пересчитать заголовки trail после смены языка.</summary>
    public void RefreshTrailTitles()
    {
        if (_trail.Count == 0)
            return;

        for (var i = 0; i < _trail.Count; i++)
        {
            var item = _trail[i];
            var title = BreadcrumbTitleResolver.Resolve(item.PageType, item.Parameter, item.Name);
            _trail[i] = new SettingsBreadcrumbItem(title, item.PageType, item.Parameter);
        }

        TrailChanged?.Invoke(this, EventArgs.Empty);
    }

    public void NavigateToRoot(Type pageType, object? parameter, string title, NavigationTransitionInfo? transition = null)
    {
        if (_frame is null)
            return;

        // x86: TrailChanged→BreadcrumbBar.ItemsSource внутри ResetTrail (до Navigate) → AV.
        // Сначала trail без UI, Navigate, потом TrailChanged.
        ResetTrail(title, pageType, parameter, raiseTrailChanged: false);
        if (_frameHasNavigated)
        {
            ClearBackStack();
        }
        else
        {
        }

        // Первый cold-start — Suppress (x86 AV на slide); дальше — обычный ForwardSlide.
        var info = transition ?? (_frameHasNavigated
            ? ForwardSlide()
            : new SuppressNavigationTransitionInfo());
        _frame.Navigate(pageType, parameter, info);
        _frameHasNavigated = true;
        TrailChanged?.Invoke(this, EventArgs.Empty);
    }

    public void Push(Type pageType, object? parameter, string title, bool rememberForBack = true)
    {
        if (_frame is null)
            return;

        if (rememberForBack)
            RememberForBack(CurrentSidebarTag);

        var resolvedTitle = BreadcrumbTitleResolver.Resolve(pageType, parameter, title);
        _trail.Add(new SettingsBreadcrumbItem(resolvedTitle, pageType, parameter));
        TrailChanged?.Invoke(this, EventArgs.Empty);

        _frame.Navigate(pageType, parameter, ForwardSlide());
        _frameHasNavigated = true;
    }

    /// <summary>
    /// Переход с «Обзора» сразу на hub ± detail: один шаг в глобальном стеке «Назад», без промежуточного hub.
    /// </summary>
    public void NavigateHomeDrill(
        Type hubPageType,
        string hubTitle,
        object? hubParameter,
        Type? detailPageType,
        object? detailParameter,
        string? detailTitle)
    {
        if (_frame is null)
            return;

        _trail.Clear();
        var resolvedHubTitle = BreadcrumbTitleResolver.Resolve(hubPageType, hubParameter, hubTitle);
        _trail.Add(new SettingsBreadcrumbItem(resolvedHubTitle, hubPageType, hubParameter));
        if (detailPageType is not null && detailTitle is not null)
        {
            var resolvedDetailTitle = BreadcrumbTitleResolver.Resolve(detailPageType, detailParameter, detailTitle);
            _trail.Add(new SettingsBreadcrumbItem(resolvedDetailTitle, detailPageType, detailParameter));
        }

        TrailChanged?.Invoke(this, EventArgs.Empty);
        ClearBackStack();

        if (detailPageType is not null)
            _frame.Navigate(detailPageType, detailParameter, ForwardSlide());
        else
            _frame.Navigate(hubPageType, hubParameter, ForwardSlide());
        _frameHasNavigated = true;
    }

    /// <summary>
    /// Обновить корневую breadcrumb-запись для активного раздела без перенавигации.
    /// Нужна для hub-состояний (например HKCU/HKLM вкладка DWM), чтобы Back запоминал фактический маршрут.
    /// </summary>
    public void UpdateRootTrail(Type rootPageType, object? rootParameter, string? rootTitle = null)
    {
        if (_trail.Count == 0)
            return;

        var current = _trail[0];
        if (current.PageType != rootPageType)
            return;

        var fallbackTitle = rootTitle ?? current.Name;
        var resolvedTitle = BreadcrumbTitleResolver.Resolve(rootPageType, rootParameter, fallbackTitle);
        if (string.Equals(current.Name, resolvedTitle, StringComparison.Ordinal)
            && Equals(current.Parameter, rootParameter))
        {
            return;
        }

        _trail[0] = new SettingsBreadcrumbItem(resolvedTitle, rootPageType, rootParameter);
        TrailChanged?.Invoke(this, EventArgs.Empty);
    }

    /// <summary>
    /// Переключить корневой зеркальный маршрут hub без Frame.Navigate, но с сохранением шага в глобальный Back.
    /// </summary>
    public void SwitchRootRoute(Type rootPageType, object? rootParameter, string? rootTitle = null)
    {
        if (_trail.Count == 0)
            return;

        var current = _trail[0];
        if (current.PageType != rootPageType)
            return;

        var fallbackTitle = rootTitle ?? current.Name;
        var resolvedTitle = BreadcrumbTitleResolver.Resolve(rootPageType, rootParameter, fallbackTitle);
        if (string.Equals(current.Name, resolvedTitle, StringComparison.Ordinal)
            && Equals(current.Parameter, rootParameter))
        {
            return;
        }

        RememberForBack(CurrentSidebarTag);
        _trail[0] = new SettingsBreadcrumbItem(resolvedTitle, rootPageType, rootParameter);
        TrailChanged?.Invoke(this, EventArgs.Empty);
    }

    public bool TryPop()
    {
        if (_frame is null || Trail.Count <= 1 || !_frame.CanGoBack)
            return false;

        _trail.RemoveAt(_trail.Count - 1);
        TrailChanged?.Invoke(this, EventArgs.Empty);

        _frame.GoBack(BackSlide());
        return true;
    }

    public void NavigateToIndex(int index)
    {
        if (_frame is null || index < 0 || index >= Trail.Count - 1)
            return;

        // Клик по хлебной крошке — фактический переход: накапливаем текущее (более глубокое)
        // состояние в стеке «Назад», чтобы кнопка вернула на покинутую страницу.
        RememberForBack(CurrentSidebarTag);

        var target = _trail[index];

        while (_trail.Count > index + 1)
            _trail.RemoveAt(_trail.Count - 1);

        TrailChanged?.Invoke(this, EventArgs.Empty);

        ClearBackStack();
        _frame.Navigate(target.PageType, target.Parameter, BackSlide());
        _frameHasNavigated = true;
    }

    private SettingsNavigationSnapshot CreateSnapshot(string? sidebarTag)
    {
        var trailCopy = _trail
            .Select(item => new SettingsBreadcrumbItem(item.Name, item.PageType, item.Parameter))
            .ToList();
        return new SettingsNavigationSnapshot(sidebarTag, trailCopy);
    }

    private void RestoreSnapshot(SettingsNavigationSnapshot snapshot)
    {
        _isRestoring = true;
        try
        {
            _trail.Clear();
            foreach (var item in snapshot.Trail)
                _trail.Add(new SettingsBreadcrumbItem(item.Name, item.PageType, item.Parameter));

            TrailChanged?.Invoke(this, EventArgs.Empty);

            ClearBackStack();

            var current = _trail[^1];
            _frame!.Navigate(current.PageType, current.Parameter, BackSlide());
            _frameHasNavigated = true;
        }
        finally
        {
            _isRestoring = false;
        }
    }

    private static SlideNavigationTransitionInfo ForwardSlide() => new()
    {
        Effect = SlideNavigationTransitionEffect.FromRight
    };

    private static SlideNavigationTransitionInfo BackSlide() => new()
    {
        Effect = SlideNavigationTransitionEffect.FromLeft
    };

    private void ClearBackStack()
    {
        if (_frame is null || !_frameHasNavigated)
            return;

        while (_frame.BackStack.Count > 0)
            _frame.BackStack.RemoveAt(_frame.BackStack.Count - 1);
    }
}
