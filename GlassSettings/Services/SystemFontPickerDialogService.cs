using GlassSettings.Core.Text;
using GlassSettings.Strings;
using Microsoft.UI.Dispatching;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;
using Microsoft.UI.Xaml.Media;
using Windows.Foundation;

namespace GlassSettings.Services;

/// <summary>Hosted-диалог выбора системного шрифта (аналог ChooseFont).</summary>
public static class SystemFontPickerDialogService
{
    private const int MinPointSize = 7;
    private const int MaxPointSize = 72;

    /// <summary>Фиксированная высота списков семейств/начертаний — не сжимать при пустом поиске.</summary>
    private const double FontListHeight = 220;

    private static ScrollViewer? FindScrollViewer(DependencyObject root)
    {
        if (root is ScrollViewer sv)
            return sv;

        var count = VisualTreeHelper.GetChildrenCount(root);
        for (var i = 0; i < count; i++)
        {
            var found = FindScrollViewer(VisualTreeHelper.GetChild(root, i));
            if (found is not null)
                return found;
        }

        return null;
    }

    public static async Task<SystemFontsLogFont?> PickAsync(SystemFontsLogFont initial)
    {
        var charsets = ScreenFontCatalog.GetCommonCharsets();
        var initialFace = string.IsNullOrWhiteSpace(initial.lfFaceName) ? "Segoe UI" : initial.lfFaceName.Trim();
        var initialStyleName = ScreenFontCatalog.GetLocalizedStyleName(initial);

        var allFamilies = ScreenFontCatalog.GetScreenFamilies().ToList();
        if (!allFamilies.Any(f => string.Equals(f.FaceName, initialFace, StringComparison.OrdinalIgnoreCase)))
            allFamilies.Insert(0, ScreenFontCatalog.CreateFamilyEntry(initialFace));

        var filteredFamilies = new List<ScreenFontCatalog.ScreenFontFamilyEntry>(allFamilies);
        var allStyles = new List<ScreenFontCatalog.ScreenFontStyleEntry>();
        var filteredStyles = new List<ScreenFontCatalog.ScreenFontStyleEntry>();

        // TextBox Fluent: крестик очистки появляется при непустом тексте (закрытие режима поиска).
        var fontFilterBox = new TextBox { MinHeight = 32, Text = initialFace };
        var styleFilterBox = new TextBox { MinHeight = 32, Text = initialStyleName };

        var fontList = new ListView
        {
            SelectionMode = ListViewSelectionMode.Single,
            Height = FontListHeight,
            MinHeight = FontListHeight,
            MaxHeight = FontListHeight,
            VerticalAlignment = VerticalAlignment.Top,
            ItemsSource = filteredFamilies,
            ItemTemplate = Application.Current.Resources["SystemFontFamilyItemTemplate"] as DataTemplate
        };

        var styleList = new ListView
        {
            SelectionMode = ListViewSelectionMode.Single,
            Height = FontListHeight,
            MinHeight = FontListHeight,
            MaxHeight = FontListHeight,
            VerticalAlignment = VerticalAlignment.Top,
            ItemsSource = filteredStyles,
            ItemTemplate = Application.Current.Resources["SystemFontStyleItemTemplate"] as DataTemplate
        };

        var charsetCombo = new ComboBox
        {
            MinWidth = 160,
            ItemsSource = charsets,
            DisplayMemberPath = nameof(ScreenFontCatalog.ScreenFontCharsetEntry.DisplayName)
        };

        var sizeBox = new NumberBox
        {
            MinWidth = 80,
            MinHeight = 32,
            MaxHeight = 32,
            Minimum = MinPointSize,
            Maximum = MaxPointSize,
            SpinButtonPlacementMode = NumberBoxSpinButtonPlacementMode.Inline,
            SmallChange = 1,
            LargeChange = 2
        };

        var body = new Grid { MinWidth = 480, ColumnSpacing = 16, RowSpacing = 12 };
        body.ColumnDefinitions.Add(new ColumnDefinition { Width = new GridLength(1, GridUnitType.Star) });
        body.ColumnDefinitions.Add(new ColumnDefinition { Width = new GridLength(1, GridUnitType.Star) });
        body.RowDefinitions.Add(new RowDefinition { Height = GridLength.Auto });
        body.RowDefinitions.Add(new RowDefinition { Height = GridLength.Auto });
        // Фиксированная высота списков — Auto, не Star (иначе пустой поиск сжимает диалог).
        body.RowDefinitions.Add(new RowDefinition { Height = new GridLength(FontListHeight) });
        body.RowDefinitions.Add(new RowDefinition { Height = GridLength.Auto });
        body.RowDefinitions.Add(new RowDefinition { Height = GridLength.Auto });

        body.Children.Add(MakeLabel("Text_FontPickerFont"));
        var styleLabel = MakeLabel("Text_FontPickerStyle");
        Grid.SetColumn(styleLabel, 1);
        body.Children.Add(styleLabel);

        Grid.SetRow(fontFilterBox, 1);
        body.Children.Add(fontFilterBox);
        Grid.SetRow(styleFilterBox, 1);
        Grid.SetColumn(styleFilterBox, 1);
        body.Children.Add(styleFilterBox);

        Grid.SetRow(fontList, 2);
        body.Children.Add(fontList);
        Grid.SetRow(styleList, 2);
        Grid.SetColumn(styleList, 1);
        body.Children.Add(styleList);

        var divider = new Border
        {
            Height = 1,
            Margin = new Thickness(0, 4, 0, 4),
            Background = Application.Current.Resources["DividerStrokeColorDefaultBrush"] as Brush
        };
        Grid.SetRow(divider, 3);
        Grid.SetColumnSpan(divider, 2);
        body.Children.Add(divider);

        var bottom = new Grid { ColumnSpacing = 16 };
        bottom.ColumnDefinitions.Add(new ColumnDefinition { Width = new GridLength(1, GridUnitType.Star) });
        bottom.ColumnDefinitions.Add(new ColumnDefinition { Width = GridLength.Auto });
        var charsetStack = new StackPanel { Spacing = 4 };
        charsetStack.Children.Add(MakeLabel("Text_FontPickerCharset"));
        charsetStack.Children.Add(charsetCombo);
        bottom.Children.Add(charsetStack);
        var sizeStack = new StackPanel { Spacing = 4 };
        sizeStack.Children.Add(MakeLabel("Text_FontPickerSize"));
        sizeStack.Children.Add(sizeBox);
        Grid.SetColumn(sizeStack, 1);
        bottom.Children.Add(sizeStack);
        Grid.SetRow(bottom, 4);
        Grid.SetColumnSpan(bottom, 2);
        body.Children.Add(bottom);

        SystemFontsLogFont? result = null;
        ScreenFontCatalog.ScreenFontFamilyEntry? selectedFamily = null;
        ScreenFontCatalog.ScreenFontStyleEntry? selectedStyle = null;
        ScreenFontCatalog.ScreenFontCharsetEntry? selectedCharset = null;
        var suppress = false;
        var suppressFilter = false;
        var fontSearchActive = false;
        var styleSearchActive = false;
        var fontSearchPriming = false; // GotFocus clear — не трактовать пустой Text как крестик
        var styleSearchPriming = false;
        HostedDialogHostContext? dialogCtx = null;
        var dispatcher = DispatcherQueue.GetForCurrentThread();

        void BeginSuppressFilter()
        {
            suppressFilter = true;
        }

        void EndSuppressFilterDeferred()
        {
            // TextChanged от clear приходит отложенно после Normal-enqueue —
            // снимаем suppress на Low, чтобы deferred TextChanged ещё видел suppressFilter=true.
            dispatcher?.TryEnqueue(DispatcherQueuePriority.Low, () =>
            {
                suppressFilter = false;
                fontSearchPriming = false;
                styleSearchPriming = false;
            });
        }

        bool IsValidPointSize(double value) => value is >= MinPointSize and <= MaxPointSize;

        bool TryBuildCurrent(out SystemFontsLogFont lf)
        {
            lf = default;
            if (selectedFamily is null
                || selectedStyle is null
                || selectedCharset is null
                || !IsValidPointSize(sizeBox.Value))
            {
                return false;
            }

            lf = ScreenFontCatalog.BuildLogFont(
                selectedFamily.FaceName,
                selectedStyle,
                selectedCharset.CharSet,
                (int)Math.Round(sizeBox.Value),
                initial);
            return true;
        }

        void RefreshPrimary()
        {
            if (dialogCtx is null)
                return;

            if (!TryBuildCurrent(out var current))
            {
                dialogCtx.SetPrimaryEnabled(false);
                return;
            }

            dialogCtx.SetPrimaryEnabled(!SystemFontsLogFont.EqualsForCompare(initial, current));
        }

        void SyncStyleFilterBoxFromSelection()
        {
            if (styleSearchActive)
                return;

            BeginSuppressFilter();
            styleFilterBox.Text = selectedStyle?.DisplayName ?? string.Empty;
            EndSuppressFilterDeferred();
        }

        void SyncFontFilterBoxFromSelection()
        {
            if (fontSearchActive)
                return;

            BeginSuppressFilter();
            fontFilterBox.Text = selectedFamily?.FaceName ?? string.Empty;
            EndSuppressFilterDeferred();
        }

        void RebindListPreserveOffset(ListView list, object items)
        {
            var sv = FindScrollViewer(list);
            var savedOffset = sv?.VerticalOffset ?? 0;
            suppress = true;
            try
            {
                list.ItemsSource = null;
                list.ItemsSource = items;
            }
            finally
            {
                suppress = false;
            }

            sv = FindScrollViewer(list);
            if (sv is not null && savedOffset > 0)
                sv.ChangeView(null, savedOffset, null, disableAnimation: true);
        }

        void ApplyFontFilter()
        {
            // suppressFilter только для TextChanged — Apply* должны работать из LoadStyles/ExitSearch.
            filteredFamilies.Clear();
            if (!fontSearchActive)
            {
                filteredFamilies.AddRange(allFamilies);
            }
            else
            {
                var query = fontFilterBox.Text;
                foreach (var family in allFamilies)
                {
                    if (string.IsNullOrEmpty(query)
                        || family.FaceName.Contains(query, StringComparison.OrdinalIgnoreCase))
                    {
                        filteredFamilies.Add(family);
                    }
                }
            }

            RebindListPreserveOffset(fontList, filteredFamilies);

            if (filteredFamilies.Count == 0)
            {
                // Поиск без совпадений — не сбрасываем выбранное семейство.
                suppress = true;
                fontList.SelectedIndex = -1;
                suppress = false;
                return;
            }

            var pick = selectedFamily is not null
                ? filteredFamilies.FirstOrDefault(f =>
                      ReferenceEquals(f, selectedFamily)
                      || string.Equals(f.FaceName, selectedFamily.FaceName, StringComparison.OrdinalIgnoreCase))
                : null;

            suppress = true;
            if (pick is not null)
            {
                selectedFamily = pick;
                fontList.SelectedItem = pick;
            }
            else
            {
                fontList.SelectedIndex = -1;
            }

            suppress = false;
        }

        void ApplyStyleFilter()
        {
            filteredStyles.Clear();
            if (!styleSearchActive)
            {
                filteredStyles.AddRange(allStyles);
            }
            else
            {
                var query = styleFilterBox.Text;
                foreach (var style in allStyles)
                {
                    if (string.IsNullOrEmpty(query)
                        || style.DisplayName.Contains(query, StringComparison.OrdinalIgnoreCase))
                    {
                        filteredStyles.Add(style);
                    }
                }
            }

            RebindListPreserveOffset(styleList, filteredStyles);

            if (filteredStyles.Count == 0)
            {
                suppress = true;
                styleList.SelectedIndex = -1;
                suppress = false;
                return;
            }

            var pick = selectedStyle is not null
                ? filteredStyles.FirstOrDefault(s =>
                      ReferenceEquals(s, selectedStyle)
                      || (s.Weight == selectedStyle.Weight && s.Italic == selectedStyle.Italic))
                : null;

            suppress = true;
            if (pick is not null)
            {
                selectedStyle = pick;
                styleList.SelectedItem = pick;
            }
            else
            {
                styleList.SelectedIndex = -1;
            }

            suppress = false;
        }

        void RestoreFontFilterBoxText()
        {
            var face = selectedFamily?.FaceName ?? string.Empty;
            if (string.Equals(fontFilterBox.Text, face, StringComparison.Ordinal))
                return;

            BeginSuppressFilter();
            fontFilterBox.Text = face;
            EndSuppressFilterDeferred();
        }

        void RestoreStyleFilterBoxText()
        {
            var name = selectedStyle?.DisplayName ?? string.Empty;
            if (string.Equals(styleFilterBox.Text, name, StringComparison.Ordinal))
                return;

            BeginSuppressFilter();
            styleFilterBox.Text = name;
            EndSuppressFilterDeferred();
        }

        void ExitFontSearch(bool scrollToSelection, bool releaseFilterFocus = false)
        {
            // Rebind только если список реально был сужен поиском — иначе LostFocus дергает ListView.
            var needsListRestore = fontSearchActive && filteredFamilies.Count != allFamilies.Count;

            fontSearchActive = false;
            RestoreFontFilterBoxText();
            if (needsListRestore)
                ApplyFontFilter();
            if (scrollToSelection)
                EnsureFontListSelection(scrollToSelection: true);
            RefreshPrimary();

            // Крестик WinUI виден при фокусе + непустом Text — уводим фокус на список.
            if (releaseFilterFocus)
                _ = fontList.Focus(FocusState.Programmatic);
        }

        void ExitStyleSearch(bool scrollToSelection, bool releaseFilterFocus = false)
        {
            var needsListRestore = styleSearchActive && filteredStyles.Count != allStyles.Count;

            styleSearchActive = false;
            RestoreStyleFilterBoxText();
            if (needsListRestore)
                ApplyStyleFilter();
            if (scrollToSelection)
                EnsureStyleListSelection(scrollToSelection: true);
            RefreshPrimary();

            if (releaseFilterFocus)
                _ = styleList.Focus(FocusState.Programmatic);
        }

        void ScrollItemToVerticalCenter(ListView list, object item)
        {
            // Сначала реализуем контейнер (виртуализация), затем центрируем по ScrollViewer.
            list.ScrollIntoView(item);
            list.UpdateLayout();

            var sv = FindScrollViewer(list);
            if (sv?.Content is not UIElement content)
                return;
            if (list.ContainerFromItem(item) is not FrameworkElement container)
                return;

            try
            {
                var bounds = container.TransformToVisual(content)
                    .TransformBounds(new Rect(0, 0, container.ActualWidth, container.ActualHeight));
                var itemCenter = bounds.Y + bounds.Height / 2.0;
                var target = itemCenter - sv.ViewportHeight / 2.0;
                if (sv.ScrollableHeight > 0)
                    target = Math.Clamp(target, 0, sv.ScrollableHeight);
                else
                    target = 0;

                sv.ChangeView(null, target, null, disableAnimation: true);
            }
            catch
            {
                // ScrollIntoView уже выполнен выше.
            }
        }

        void EnsureFontListSelection(bool scrollToSelection)
        {
            if (selectedFamily is null || filteredFamilies.Count == 0)
                return;

            var familyIndex = filteredFamilies.FindIndex(f =>
                ReferenceEquals(f, selectedFamily)
                || string.Equals(f.FaceName, selectedFamily.FaceName, StringComparison.OrdinalIgnoreCase));
            if (familyIndex < 0)
                return;

            suppress = true;
            fontList.SelectedIndex = familyIndex;
            selectedFamily = filteredFamilies[familyIndex];
            suppress = false;

            if (scrollToSelection)
                ScrollItemToVerticalCenter(fontList, selectedFamily);
        }

        void EnsureStyleListSelection(bool scrollToSelection)
        {
            if (selectedStyle is null || filteredStyles.Count == 0)
                return;

            var styleIndex = filteredStyles.FindIndex(s =>
                ReferenceEquals(s, selectedStyle)
                || (s.Weight == selectedStyle.Weight && s.Italic == selectedStyle.Italic));
            if (styleIndex < 0)
                return;

            suppress = true;
            styleList.SelectedIndex = styleIndex;
            selectedStyle = filteredStyles[styleIndex];
            suppress = false;

            if (scrollToSelection)
                ScrollItemToVerticalCenter(styleList, selectedStyle);
        }

        void ApplyListSelection()
        {
            EnsureFontListSelection(scrollToSelection: true);
            EnsureStyleListSelection(scrollToSelection: true);
        }

        void TryApplyInitialListSelection()
        {
            if (fontList.ActualHeight <= 0 && styleList.ActualHeight <= 0)
                return;

            ApplyListSelection();
        }

        void LoadStylesForFamily(string faceName, int? selectWeight, bool? selectItalic, byte charSet)
        {
            allStyles = ScreenFontCatalog.GetStylesForFace(faceName, charSet).ToList();
            if (selectWeight is not null)
                ScreenFontCatalog.EnsureStylePresent(allStyles, faceName, selectWeight.Value, selectItalic ?? false);

            if (selectWeight is not null)
            {
                selectedStyle = allStyles.FirstOrDefault(s =>
                                    s.Weight == selectWeight && s.Italic == (selectItalic ?? false))
                                ?? allStyles[0];
            }
            else if (allStyles.Count > 0)
            {
                selectedStyle = allStyles[0];
            }

            styleSearchActive = false;
            BeginSuppressFilter();
            styleFilterBox.Text = selectedStyle?.DisplayName ?? string.Empty;
            EndSuppressFilterDeferred();
            ApplyStyleFilter();
        }

        selectedFamily = allFamilies.FirstOrDefault(f =>
                             string.Equals(f.FaceName, initialFace, StringComparison.OrdinalIgnoreCase))
                         ?? allFamilies.FirstOrDefault();

        selectedCharset = charsets.FirstOrDefault(c => c.CharSet == initial.lfCharSet) ?? charsets[1];
        charsetCombo.SelectedItem = selectedCharset;
        sizeBox.Value = Math.Clamp(SystemFontsModel.ReadPointSize(initial), MinPointSize, MaxPointSize);

        if (selectedFamily is not null)
        {
            LoadStylesForFamily(
                selectedFamily.FaceName,
                initial.lfWeight,
                initial.lfItalic != 0,
                selectedCharset.CharSet);
        }

        fontSearchActive = false;
        styleSearchActive = false;
        ApplyFontFilter();

        void ReloadStylesForCurrentFamily(int? selectWeight, bool? selectItalic)
        {
            if (selectedFamily is null || selectedCharset is null)
                return;

            LoadStylesForFamily(
                selectedFamily.FaceName,
                selectWeight,
                selectItalic,
                selectedCharset.CharSet);
        }

        fontFilterBox.GotFocus += (_, _) =>
        {
            // Очистить поле для ввода. Режим поиска включается только при вводе (не на GotFocus).
            fontSearchPriming = true;
            BeginSuppressFilter();
            fontFilterBox.Text = string.Empty;
            EndSuppressFilterDeferred();
        };

        fontFilterBox.LostFocus += (_, _) =>
        {
            if (fontSearchActive)
                ExitFontSearch(scrollToSelection: true);
            else
                RestoreFontFilterBoxText();
        };

        fontFilterBox.TextChanged += (_, _) =>
        {
            if (suppressFilter || fontSearchPriming)
                return;

            // Пустой ввод во время поиска — полный список, режим поиска не завершаем.
            // Выход из поиска: LostFocus / выбор в списке.
            if (string.IsNullOrEmpty(fontFilterBox.Text))
            {
                if (fontSearchActive)
                {
                    ApplyFontFilter();
                    RefreshPrimary();
                }

                return;
            }

            fontSearchActive = true;
            ApplyFontFilter();
            RefreshPrimary();
        };

        styleFilterBox.GotFocus += (_, _) =>
        {
            styleSearchPriming = true;
            BeginSuppressFilter();
            styleFilterBox.Text = string.Empty;
            EndSuppressFilterDeferred();
        };

        styleFilterBox.LostFocus += (_, _) =>
        {
            if (styleSearchActive)
                ExitStyleSearch(scrollToSelection: true);
            else
                RestoreStyleFilterBoxText();
        };

        styleFilterBox.TextChanged += (_, _) =>
        {
            if (suppressFilter || styleSearchPriming)
                return;

            // Пустой ввод во время поиска — полный список, режим поиска не завершаем.
            if (string.IsNullOrEmpty(styleFilterBox.Text))
            {
                if (styleSearchActive)
                {
                    ApplyStyleFilter();
                    RefreshPrimary();
                }

                return;
            }

            styleSearchActive = true;
            ApplyStyleFilter();
            RefreshPrimary();
        };

        fontList.SelectionChanged += (_, _) =>
        {
            if (suppress)
                return;

            // Не затирать selectedFamily при временном SelectedItem=null (rebind ItemsSource).
            if (fontList.SelectedItem is not ScreenFontCatalog.ScreenFontFamilyEntry pick)
                return;

            selectedFamily = pick;
            // Выбор в найденном: сразу выйти из поиска, полный список, scroll, имя в поле.
            if (fontSearchActive)
                ExitFontSearch(scrollToSelection: true);
            else
                SyncFontFilterBoxFromSelection();

            ReloadStylesForCurrentFamily(null, null);
            RefreshPrimary();
        };

        styleList.SelectionChanged += (_, _) =>
        {
            if (suppress)
                return;

            if (styleList.SelectedItem is not ScreenFontCatalog.ScreenFontStyleEntry pick)
                return;

            selectedStyle = pick;
            if (styleSearchActive)
                ExitStyleSearch(scrollToSelection: true);
            else
                SyncStyleFilterBoxFromSelection();

            RefreshPrimary();
        };

        charsetCombo.SelectionChanged += (_, _) =>
        {
            if (suppress)
                return;

            selectedCharset = charsetCombo.SelectedItem as ScreenFontCatalog.ScreenFontCharsetEntry;
            if (selectedCharset is null)
                return;

            ReloadStylesForCurrentFamily(
                selectedStyle?.Weight ?? initial.lfWeight,
                selectedStyle?.Italic ?? initial.lfItalic != 0);
            RefreshPrimary();
        };

        fontList.Loaded += (_, _) => TryApplyInitialListSelection();
        styleList.Loaded += (_, _) => TryApplyInitialListSelection();

        var dialogResult = await HostedDialogService.ShowAsync(
            new HostedDialogSpec
            {
                Title = GlassSettingsResources.Get("Text_FontPickerTitle"),
                Body = HostedDialogLayout.WrapBodyWithContentPadding(body),
                MinWidth = 520,
                MaxWidth = 640,
                Footer = new HostedDialogFooterSpec
                {
                    SecondaryButtonText = GlassSettingsResources.Get("Dialog_Cancel"),
                    PrimaryButtonText = GlassSettingsResources.Get("CmdOk"),
                    PrimaryButtonWidth = 88,
                    SecondaryButtonWidth = 88,
                    IsPrimaryEnabled = false
                }
            },
            ctx =>
            {
                dialogCtx = ctx;
                RefreshPrimary();
                sizeBox.ValueChanged += (_, _) => RefreshPrimary();

                DispatcherQueue.GetForCurrentThread()?.TryEnqueue(DispatcherQueuePriority.Normal, () =>
                {
                    ApplyListSelection();
                    RefreshPrimary();
                });
            }).ConfigureAwait(true);

        if (dialogResult != HostedDialogResult.Primary
            || selectedFamily is null
            || selectedStyle is null
            || selectedCharset is null
            || !IsValidPointSize(sizeBox.Value))
        {
            return null;
        }

        result = ScreenFontCatalog.BuildLogFont(
            selectedFamily.FaceName,
            selectedStyle,
            selectedCharset.CharSet,
            (int)Math.Round(sizeBox.Value),
            initial);
        return result;
    }

    private static TextBlock MakeLabel(string resourceKey) => new()
    {
        Text = GlassSettingsResources.Get(resourceKey),
        Style = Application.Current.Resources["CaptionTextBlockStyle"] as Style
                  ?? Application.Current.Resources["BodyTextBlockStyle"] as Style,
        Margin = new Thickness(0, 0, 0, 4)
    };
}
