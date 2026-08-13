using GlassSettings.Core.Registry;

namespace GlassSettings.Core.Text;

/// <summary>Логика мастера визуальной настройки ink (порт TextVisualTuningDialog.cpp).</summary>
public sealed class TextVisualTuningWizardEngine
{
    private const int MaxPages = 5;
    private const int PairedTopRowBlocks = 3;
    private const int PairedBottomBlockBase = 3;
    private const int PairedBottomPresetCount = 9;
    private const int PairedPageBlockCount = 12;
    private const int BlendPageBlockCount = 6;

    private const uint WizardGammaBase = 970u;
    private const uint WizardBlendSingle = 255u;
    private const uint WizardBlendDual = 170u;

    private static readonly uint[] WizardGammaCols = [770u, WizardGammaBase, 1250u];
    private static readonly uint[] WizardContourRadiusCols = [610u, 610u, 450u];
    private static readonly uint[] WizardContourIntensityCols = [90u, 113u, 150u];
    private static readonly uint[] WizardRadiusRefineCols = [200u, 500u, 900u];

    private readonly ForkBracket _fork = new();
    private readonly WizardPageState[] _pages = new WizardPageState[MaxPages];
    private readonly int[] _selectedBlockPerPage = new int[MaxPages];
    private readonly int[] _topColumnPerPage = new int[MaxPages];
    private readonly int[] _bottomColumnPerPage = new int[MaxPages];
    private readonly bool[] _bottomRowActivePerPage = new bool[MaxPages];

    private GlassInkCapsuleModel _working;
    private readonly GlassInkCapsuleModel _replayBaseline;
    private int _stepIndex;
    private int _stepCount = 4;
    private int _selectedBlock;
    private int _topColumn = 1;
    private int _bottomColumn;
    private bool _bottomRowActive;
    private bool _branchHasPseudo;
    private int _refineGammaCoarseColumn = 1;

    public event Action<int>? SelectionChanged;

    public TextVisualTuningWizardEngine(GlassInkCapsuleModel initial)
    {
        _working = initial.Clone();
        _working.Clamp();
        if (_working.IsAutoMode)
        {
            _working.RenderMode = 1;
            _working.Single = GlassInkProfileModel.FactorySingle;
            _working.Dual = GlassInkProfileModel.FactoryDual;
        }

        _replayBaseline = _working.Clone();
        _replayBaseline.Clamp();

        for (var i = 0; i < MaxPages; i++)
        {
            _selectedBlockPerPage[i] = i == 0 ? 0 : 1;
            _topColumnPerPage[i] = 1;
            _bottomColumnPerPage[i] = 0;
            _bottomRowActivePerPage[i] = false;
            _pages[i] = new WizardPageState();
        }

        ConfigurePage(0);
    }

    public GlassInkCapsuleModel Working => _working;

    public int StepIndex => _stepIndex;

    public int StepCount => _stepCount;

    public bool BranchHasPseudo => _branchHasPseudo;

    public bool CanGoBack => _stepIndex > 0;

    public bool IsLastStep => _stepIndex + 1 >= _stepCount;

    public WizardPageSnapshot CurrentPage => BuildPageSnapshot(_stepIndex);

    public string GetHint() => GetHintForStep(_stepIndex);

    public string GetHintForStep(int stepIndex) => stepIndex switch
    {
        0 => "Text_InkWizardHint_ForkRoot",
        1 => "Text_InkWizardHint_RefineCoarse",
        2 => "Text_InkWizardHint_RefineMid",
        3 when _branchHasPseudo => "Text_InkWizardHint_RefinePseudo",
        _ => "Text_InkWizardHint_RefineFine",
    };

    public int SelectedBlockIndex => _selectedBlock;

    public int TopColumn => _topColumn;

    public int BottomColumn => _bottomColumn;

    public bool BottomRowActive => _bottomRowActive;

    /// <summary>Число шагов для строки «N из M» с учётом выбора во 2-м ярусе на экранах 2–3.</summary>
    public int GetPromptStepCount(int? displayStepIndex = null)
    {
        var step = displayStepIndex ?? _stepIndex;

        if (step <= 0)
            return 4;

        if (step == 1)
            return _bottomRowActive ? 5 : 4;

        if (step == 2)
        {
            if (_bottomRowActivePerPage[1])
                return 5;

            return _bottomRowActive ? 5 : 4;
        }

        return _stepCount;
    }

    public bool IsPairedPage(int pageIndex) =>
        pageIndex is 1 or 2 || (pageIndex == 3 && _branchHasPseudo);

    public WizardPageSnapshot GetPageSnapshot(int pageIndex) => BuildPageSnapshot(pageIndex);

    public void EnsurePageConfigured(int pageIndex) => ConfigurePage(pageIndex);

    public void SelectBlock(int blockIndex)
    {
        var page = _pages[_stepIndex];
        blockIndex = Math.Clamp(blockIndex, 0, Math.Max(0, page.BlockCount - 1));
        if (_selectedBlock == blockIndex)
            return;

        _selectedBlock = blockIndex;
        _selectedBlockPerPage[_stepIndex] = blockIndex;
        OnSelectionMutated();
    }

    public void SelectTopColumn(int column)
    {
        if (!IsPairedPage(_stepIndex))
            return;

        column = Math.Clamp(column, 0, PairedTopRowBlocks - 1);
        if (_topColumn == column)
            return;

        _topColumn = column;
        _topColumnPerPage[_stepIndex] = column;
        ClearBottomSelectionInternal();
        OnSelectionMutated();
    }

    private static int BottomPresetIndex(int topColumn, int contourColumn) =>
        PairedBottomBlockBase + topColumn * PairedTopRowBlocks + contourColumn;

    public void SelectBottomColumn(int column)
    {
        if (!IsPairedPage(_stepIndex))
            return;

        column = Math.Clamp(column, 0, PairedTopRowBlocks - 1);
        _bottomColumn = column;
        _bottomColumnPerPage[_stepIndex] = column;
        _bottomRowActive = true;
        _bottomRowActivePerPage[_stepIndex] = true;
        OnSelectionMutated();
    }

    public void ClearBottomSelection()
    {
        if (!IsPairedPage(_stepIndex))
            return;

        ClearBottomSelectionInternal();
        OnSelectionMutated();
    }

    private void ClearBottomSelectionInternal()
    {
        _bottomRowActive = false;
        _bottomRowActivePerPage[_stepIndex] = false;
    }

    public void GoBack()
    {
        if (_stepIndex <= 0)
            return;

        _stepIndex--;
        RestorePageSelection(_stepIndex);
    }

    public void GoNext()
    {
        ApplyStepChoice();
        if (IsLastStep)
        {
            FinalizeWorkingCapsule();
            return;
        }

        _stepIndex++;
        RestorePageSelection(_stepIndex);
    }

    public void FinalizeWorkingCapsule()
    {
        if (!_branchHasPseudo)
        {
            var single = _working.Single;
            var dual = _working.Dual;
            ApplyPseudoOff(ref single);
            ApplyPseudoOff(ref dual);
            _working.Single = single;
            _working.Dual = dual;
        }
    }

    private void RestorePageSelection(int pageIndex)
    {
        _selectedBlock = _selectedBlockPerPage[pageIndex];
        _topColumn = _topColumnPerPage[pageIndex];
        _bottomColumn = _bottomColumnPerPage[pageIndex];
        _bottomRowActive = _bottomRowActivePerPage[pageIndex];
    }

    /// <summary>Восстановление плоского выбора из метки ArN[col2] (без replay).</summary>
    public void RestoreFlatSelection(int markedRow)
    {
        var page = _pages[_stepIndex];
        var row = markedRow;
        if (row < 0)
            row = _selectedBlockPerPage[_stepIndex];

        row = Math.Clamp(row, 0, Math.Max(0, page.BlockCount - 1));
        _selectedBlock = row;
        _selectedBlockPerPage[_stepIndex] = row;
    }

    /// <summary>Восстановление ярусного выбора из меток ArN[col2] (без replay).</summary>
    public void RestorePairedSelection(int topRow, int bottomRow)
    {
        topRow = Math.Clamp(topRow, 0, PairedTopRowBlocks - 1);
        _topColumn = topRow;
        _topColumnPerPage[_stepIndex] = topRow;

        if (bottomRow < 0)
        {
            _bottomColumn = 0;
            _bottomColumnPerPage[_stepIndex] = 0;
            _bottomRowActive = false;
            _bottomRowActivePerPage[_stepIndex] = false;
            return;
        }

        bottomRow = Math.Clamp(bottomRow, 0, PairedTopRowBlocks - 1);
        _bottomColumn = bottomRow;
        _bottomColumnPerPage[_stepIndex] = bottomRow;
        _bottomRowActive = true;
        _bottomRowActivePerPage[_stepIndex] = true;
    }

    private WizardPageSnapshot BuildPageSnapshot(int pageIndex)
    {
        var page = _pages[pageIndex];
        var blocks = new WizardPreviewBlock[page.BlockCount];
        for (var i = 0; i < page.BlockCount; i++)
            blocks[i] = page.Configs[i];

        return new WizardPageSnapshot(blocks, IsPairedPage(pageIndex));
    }

    private void ConfigurePage(int pageIndex)
    {
        var page = _pages[pageIndex];
        switch (pageIndex)
        {
        case 0:
            BuildForkRootConfigs(page);
            break;
        case 1:
            BuildGammaContourPage(page, narrowGamma: false);
            break;
        case 2:
            BuildGammaContourPage(page, narrowGamma: true);
            break;
        case 3:
            if (_branchHasPseudo)
                BuildPseudoRadiusPage(page);
            else
                BuildBlendPage(page);
            break;
        default:
            BuildBlendPage(page);
            break;
        }
    }

    public void ApplySelection(WizardSelectionPath path, bool toggle)
    {
        switch (path.Role)
        {
        case WizardSelectionRole.FlatBlock:
            SelectBlock(path.Row);
            break;
        case WizardSelectionRole.TopRow:
            SelectTopColumn(path.Row);
            break;
        case WizardSelectionRole.BottomRow:
            if (toggle && _bottomRowActive && _bottomColumn == path.PresetIndex && _topColumn == path.Row)
            {
                ClearBottomSelection();
                break;
            }

            if (path.Row != _topColumn)
                return;

            SelectBottomColumn(path.PresetIndex);
            break;
        }
    }

    public void ReplayWorkingProfileUpTo(int stepIndex)
    {
        _working = _replayBaseline.Clone();
        _working.Clamp();
        _branchHasPseudo = false;
        _stepCount = 4;
        _refineGammaCoarseColumn = 1;

        var savedStep = _stepIndex;
        var savedBlock = _selectedBlock;
        var savedTop = _topColumn;
        var savedBottom = _bottomColumn;
        var savedBottomActive = _bottomRowActive;

        try
        {
            for (var page = 0; page < stepIndex; page++)
            {
                _stepIndex = page;
                RestorePageSelection(page);
                EnsurePageConfigured(page);
                ApplyStepChoice();
            }

            _stepIndex = stepIndex;
            RestorePageSelection(stepIndex);
            ApplyPreviewChoiceForCurrentStep();
        }
        finally
        {
            _stepIndex = savedStep;
            _selectedBlock = savedBlock;
            _topColumn = savedTop;
            _bottomColumn = savedBottom;
            _bottomRowActive = savedBottomActive;
        }
    }

    private void ApplyPreviewChoiceForCurrentStep()
    {
        var page = _pages[_stepIndex];

        if (_stepIndex == 0)
        {
            var chosen = page.Configs[_selectedBlock];
            _working.RenderMode = chosen.RenderMode;
            if (chosen.RenderMode == 1u)
                _working.Single = chosen.Profile;
            else
                _working.Dual = chosen.Profile;
            return;
        }

        if (_stepIndex is 1 or 2)
        {
            var profile = BuildGammaContourChoiceProfile(page);
            if (_bottomRowActive)
            {
                _branchHasPseudo = true;
                _fork.AnchorPseudoR = profile.PseudoGlowRadiusThousandths;
                _fork.AnchorPseudoI = profile.PseudoGlowIntensity;
            }

            ApplyProfileToWorking(profile);
            _stepCount = _branchHasPseudo ? 5 : 4;
            return;
        }

        if (_branchHasPseudo && _stepIndex == 3)
        {
            var profile = BuildPseudoRadiusChoiceProfile(page);
            ApplyProfileToWorking(profile);
            return;
        }

        ApplyProfileToWorking(page.Configs[_selectedBlock].Profile);
    }

    private void OnSelectionMutated()
    {
        ReplayWorkingProfileUpTo(_stepIndex);
        ResetForwardSelections(_stepIndex + 1);
        SelectionChanged?.Invoke(_stepIndex);
    }

    private void ResetForwardSelections(int fromPage)
    {
        if (fromPage < 0)
            return;

        for (var i = fromPage; i < MaxPages; i++)
        {
            _selectedBlockPerPage[i] = i == 0 ? 0 : 1;
            _topColumnPerPage[i] = 1;
            _bottomColumnPerPage[i] = 0;
            _bottomRowActivePerPage[i] = false;
        }
    }

    private void ApplyStepChoice()
    {
        var page = _pages[_stepIndex];

        if (_stepIndex == 0)
        {
            var chosen = page.Configs[_selectedBlock];
            _working.RenderMode = chosen.RenderMode;
            if (chosen.RenderMode == 1u)
                _working.Single = chosen.Profile;
            else
                _working.Dual = chosen.Profile;

            _branchHasPseudo = false;
            _stepCount = 4;
            InitForkBracketFromProfile(chosen.Profile);
            return;
        }

        if (_stepIndex is 1 or 2)
        {
            var profile = BuildGammaContourChoiceProfile(page);
            if (_bottomRowActive)
            {
                _branchHasPseudo = true;
                _fork.AnchorPseudoR = profile.PseudoGlowRadiusThousandths;
                _fork.AnchorPseudoI = profile.PseudoGlowIntensity;
            }

            if (_stepIndex == 1)
                _refineGammaCoarseColumn = _topColumn;

            ApplyProfileToWorking(profile);
            NarrowForkBracket(profile);
            _stepCount = _branchHasPseudo ? 5 : 4;
            return;
        }

        if (_branchHasPseudo && _stepIndex == 3)
        {
            var profile = BuildPseudoRadiusChoiceProfile(page);
            ApplyProfileToWorking(profile);
            _fork.AnchorPseudoR = profile.PseudoGlowRadiusThousandths;
            _fork.AnchorPseudoI = profile.PseudoGlowIntensity;
            return;
        }

        ApplyProfileToWorking(page.Configs[_selectedBlock].Profile);
    }

    private GlassInkProfileModel BuildGammaContourChoiceProfile(WizardPageState page)
    {
        var profile = page.Configs[_topColumn].Profile;
        if (_bottomRowActive)
        {
            var bottom = page.Configs[BottomPresetIndex(_topColumn, _bottomColumn)].Profile;
            ApplyPseudoOn(
                ref profile,
                bottom.PseudoGlowRadiusThousandths,
                bottom.PseudoGlowIntensity);
        }
        else
        {
            ApplyPseudoOff(ref profile);
        }

        profile.WriteNeutralDeprecatedSlots();
        return profile;
    }

    private GlassInkProfileModel BuildPseudoRadiusChoiceProfile(WizardPageState page)
    {
        var baseProfile = BuildWorkingProfile();
        var top = page.Configs[_topColumn].Profile;
        var profile = baseProfile;
        ApplyPseudoOn(
            ref profile,
            top.PseudoGlowRadiusThousandths,
            top.PseudoGlowIntensity);
        if (_bottomRowActive)
        {
            var bottom = page.Configs[BottomPresetIndex(_topColumn, _bottomColumn)].Profile;
            profile.PseudoGlowIntensity = bottom.PseudoGlowIntensity;
            profile.Clamp();
        }

        profile.WriteNeutralDeprecatedSlots();
        return profile;
    }

    private void BuildForkRootConfigs(WizardPageState page)
    {
        page.BlockCount = 2;

        var single = default(GlassInkProfileModel);
        single.CoverageGammaThousandths = WizardGammaBase;
        single.TargetBlendAlpha = WizardBlendSingle;
        ApplyPseudoOff(ref single);

        var dual = default(GlassInkProfileModel);
        dual.CoverageGammaThousandths = WizardGammaBase;
        dual.TargetBlendAlpha = WizardBlendDual;
        ApplyPseudoOff(ref dual);

        page.Configs[0] = new WizardPreviewBlock(1u, single);
        page.Configs[1] = new WizardPreviewBlock(2u, dual);
        StampNeutralSlots(page, page.BlockCount);
    }

    private void BuildGammaContourPage(WizardPageState page, bool narrowGamma)
    {
        page.BlockCount = PairedPageBlockCount;
        var rm = _working.RenderMode;
        var baseProfile = BuildWorkingProfile();
        var blend = (byte)baseProfile.TargetBlendAlpha;

        for (var topCol = 0; topCol < PairedTopRowBlocks; topCol++)
        {
            var gamma = GammaForWizardColumn(topCol, narrowGamma);
            var profileOff = baseProfile;
            profileOff.CoverageGammaThousandths = gamma;
            profileOff.TargetBlendAlpha = blend;
            ApplyPseudoOff(ref profileOff);
            page.Configs[topCol] = new WizardPreviewBlock(rm, profileOff);

            for (var j = 0; j < PairedTopRowBlocks; j++)
            {
                var profileOn = baseProfile;
                profileOn.CoverageGammaThousandths = gamma;
                profileOn.TargetBlendAlpha = blend;
                var contourR = WizardContourRadiusCols[j];
                var contourI = ContourIntensityForWizardColumn(j);
                ApplyPseudoOn(ref profileOn, contourR, contourI);
                page.Configs[BottomPresetIndex(topCol, j)] = new WizardPreviewBlock(rm, profileOn);
            }
        }

        StampNeutralSlots(page, page.BlockCount);
    }

    private void BuildPseudoRadiusPage(WizardPageState page)
    {
        page.BlockCount = PairedPageBlockCount;
        var rm = _working.RenderMode;
        var baseProfile = BuildWorkingProfile();
        var anchorR = _fork.AnchorPseudoR;
        var anchorI = _fork.AnchorPseudoI;

        for (var topCol = 0; topCol < PairedTopRowBlocks; topCol++)
        {
            var radius = RadiusPresetForWizardColumn(topCol, anchorR);
            var profile = baseProfile;
            ApplyPseudoOn(ref profile, radius, anchorI);
            page.Configs[topCol] = new WizardPreviewBlock(rm, profile);

            for (var j = 0; j < PairedTopRowBlocks; j++)
            {
                var intensity = EquivalentContourIntensity(
                    anchorI,
                    anchorR,
                    RadiusPresetForWizardColumn(j, anchorR));
                var bottomProfile = baseProfile;
                ApplyPseudoOn(ref bottomProfile, radius, intensity);
                page.Configs[BottomPresetIndex(topCol, j)] = new WizardPreviewBlock(rm, bottomProfile);
            }
        }

        StampNeutralSlots(page, page.BlockCount);
    }

    private void BuildBlendPage(WizardPageState page)
    {
        page.BlockCount = BlendPageBlockCount;
        var rm = _working.RenderMode;
        var baseProfile = BuildWorkingProfile();
        var anchorBlend = baseProfile.TargetBlendAlpha;

        var blendMin = ClampU(
            anchorBlend >= 80u ? anchorBlend - 80u : 0u,
            InkCapsuleInterop.UiBlendMin,
            InkCapsuleInterop.UiBlendMax);
        var blendMax = ClampU(
            anchorBlend + 80u,
            InkCapsuleInterop.UiBlendMin,
            InkCapsuleInterop.UiBlendMax);

        for (var i = 0; i < page.BlockCount; i++)
        {
            var profile = baseProfile;
            var col = i % 3;
            profile.TargetBlendAlpha = (byte)LerpBracket(blendMin, blendMax, col, 2);
            page.Configs[i] = new WizardPreviewBlock(rm, profile);
        }

        StampNeutralSlots(page, page.BlockCount);
    }

    private GlassInkProfileModel BuildWorkingProfile() =>
        _working.RenderMode == 2u ? _working.Dual : _working.Single;

    private void ApplyProfileToWorking(GlassInkProfileModel profile)
    {
        if (_working.RenderMode == 2u)
            _working.Dual = profile;
        else
            _working.Single = profile;
        _working.Single.WriteNeutralDeprecatedSlots();
        _working.Dual.WriteNeutralDeprecatedSlots();
    }

    private void InitForkBracketFromProfile(GlassInkProfileModel profile)
    {
        var gamma = profile.CoverageGammaThousandths;
        var blend = profile.TargetBlendAlpha;

        _fork.AnchorGamma = gamma;
        _fork.AnchorBlend = blend;
        _fork.GammaMin = ClampU(
            gamma / 2u,
            InkCapsuleInterop.UiGammaMinThousandths,
            InkCapsuleInterop.UiGammaMaxThousandths);
        _fork.GammaMax = ClampU(
            gamma * 2u,
            InkCapsuleInterop.UiGammaMinThousandths,
            InkCapsuleInterop.UiGammaMaxThousandths);
        _fork.BlendMin = ClampU(blend * 3u / 4u, InkCapsuleInterop.UiBlendMin, InkCapsuleInterop.UiBlendMax);
        _fork.BlendMax = ClampU(blend * 5u / 4u, InkCapsuleInterop.UiBlendMin, InkCapsuleInterop.UiBlendMax);
    }

    private void NarrowForkBracket(GlassInkProfileModel chosen)
    {
        Narrow(ref _fork.GammaMin, ref _fork.GammaMax, chosen.CoverageGammaThousandths,
            InkCapsuleInterop.UiGammaMinThousandths, InkCapsuleInterop.UiGammaMaxThousandths);
        Narrow(ref _fork.BlendMin, ref _fork.BlendMax, chosen.TargetBlendAlpha,
            InkCapsuleInterop.UiBlendMin, InkCapsuleInterop.UiBlendMax);

        _fork.AnchorGamma = chosen.CoverageGammaThousandths;
        _fork.AnchorBlend = chosen.TargetBlendAlpha;

        if (!_branchHasPseudo)
            return;

        Narrow(ref _fork.PseudoRadiusMin, ref _fork.PseudoRadiusMax, chosen.PseudoGlowRadiusThousandths,
            InkCapsuleInterop.UiPseudoRadiusMinThousandths, InkCapsuleInterop.UiPseudoRadiusMaxThousandths);
        Narrow(ref _fork.PseudoIntensityMin, ref _fork.PseudoIntensityMax, chosen.PseudoGlowIntensity,
            0u, InkCapsuleInterop.UiPseudoIntensityMax);
        _fork.AnchorPseudoR = chosen.PseudoGlowRadiusThousandths;
        _fork.AnchorPseudoI = chosen.PseudoGlowIntensity;
    }

    private static void Narrow(
        ref uint lo,
        ref uint hi,
        uint value,
        uint minLimit,
        uint maxLimit)
    {
        var span = Math.Max(hi - lo, 1u) / 2u;
        lo = ClampU(value > span ? value - span : minLimit, minLimit, maxLimit);
        hi = ClampU(value + span, minLimit, maxLimit);
    }

    private uint GammaForWizardColumn(int column, bool narrowGamma)
    {
        if (!narrowGamma)
            return WizardGammaCols[column];
        return LerpBracket(GammaRefineMin(), GammaRefineMax(), column, 2);
    }

    private uint GammaRefineMin()
    {
        var cols = WizardGammaCols;
        var coarse = Math.Clamp(_refineGammaCoarseColumn, 0, PairedTopRowBlocks - 1);
        var anchor = _fork.AnchorGamma;
        if (coarse == 0)
            return _fork.GammaMin;
        if (coarse == 2)
            return cols[1];
        var stepLo = cols[1] > cols[0] ? cols[1] - cols[0] : 1u;
        return ClampU(
            anchor > stepLo / 2u ? anchor - stepLo / 2u : _fork.GammaMin,
            InkCapsuleInterop.UiGammaMinThousandths,
            InkCapsuleInterop.UiGammaMaxThousandths);
    }

    private uint GammaRefineMax()
    {
        var cols = WizardGammaCols;
        var coarse = Math.Clamp(_refineGammaCoarseColumn, 0, PairedTopRowBlocks - 1);
        var anchor = _fork.AnchorGamma;
        if (coarse == 0)
            return cols[0];
        if (coarse == 2)
            return cols[2];
        var stepHi = cols[2] > cols[1] ? cols[2] - cols[1] : 1u;
        return ClampU(
            anchor + stepHi / 2u,
            InkCapsuleInterop.UiGammaMinThousandths,
            InkCapsuleInterop.UiGammaMaxThousandths);
    }

    private static uint RadiusPresetForWizardColumn(int column, uint anchorR) =>
        column switch
        {
            0 => WizardRadiusRefineCols[0],
            2 => WizardRadiusRefineCols[2],
            _ => ClampU(anchorR, 450u, 550u),
        };

    private uint ContourIntensityForWizardColumn(int column)
    {
        if (_fork.AnchorPseudoR > 0u)
            return EquivalentContourIntensity(
                _fork.AnchorPseudoI,
                _fork.AnchorPseudoR,
                WizardContourRadiusCols[column]);
        return WizardContourIntensityCols[column];
    }

    private static uint EquivalentContourIntensity(uint anchorI, uint anchorR, uint radius)
    {
        if (radius == 0 || anchorR == 0)
            return 0;
        var scaled = (anchorI * anchorR + radius / 2u) / radius;
        return ClampU(scaled, 0u, InkCapsuleInterop.UiPseudoIntensityMax);
    }

    private static uint LerpBracket(uint lo, uint hi, int index, int steps)
    {
        if (steps <= 0)
            return lo;
        return lo + (uint)((hi - lo) * (uint)index / (uint)steps);
    }

    private static uint ClampU(uint value, uint lo, uint hi) =>
        Math.Min(Math.Max(value, lo), hi);

    private static void ApplyPseudoOff(ref GlassInkProfileModel profile)
    {
        profile.PseudoGlowEnabled = 0;
        profile.PseudoGlowRadiusThousandths = 0;
        profile.PseudoGlowIntensity = 0;
    }

    private static void ApplyPseudoOn(ref GlassInkProfileModel profile, uint radius, uint intensity)
    {
        profile.PseudoGlowEnabled = 1;
        profile.PseudoGlowRadiusThousandths = ClampU(
            radius,
            InkCapsuleInterop.UiPseudoRadiusMinThousandths,
            InkCapsuleInterop.UiPseudoRadiusMaxThousandths);
        profile.PseudoGlowIntensity = ClampU(
            intensity,
            0u,
            InkCapsuleInterop.UiPseudoIntensityMax);
    }

    private static void StampNeutralSlots(WizardPageState page, int blockCount)
    {
        for (var i = 0; i < blockCount; i++)
        {
            var cfg = page.Configs[i];
            var profile = cfg.Profile;
            profile.WriteNeutralDeprecatedSlots();
            page.Configs[i] = new WizardPreviewBlock(cfg.RenderMode, profile);
        }
    }

    private sealed class WizardPageState
    {
        public int BlockCount;
        public readonly WizardPreviewBlock[] Configs = new WizardPreviewBlock[PairedPageBlockCount];
    }

    private sealed class ForkBracket
    {
        public uint GammaMin = InkCapsuleInterop.UiGammaMinThousandths;
        public uint GammaMax = InkCapsuleInterop.UiGammaMaxThousandths;
        public uint BlendMin = InkCapsuleInterop.UiBlendMin;
        public uint BlendMax = InkCapsuleInterop.UiBlendMax;
        public uint AnchorGamma = 506u;
        public uint AnchorBlend = 194u;
        public uint AnchorPseudoR = 610u;
        public uint AnchorPseudoI = 113u;
        public uint PseudoRadiusMin = InkCapsuleInterop.UiPseudoRadiusMinThousandths;
        public uint PseudoRadiusMax = InkCapsuleInterop.UiPseudoRadiusMaxThousandths;
        public uint PseudoIntensityMin;
        public uint PseudoIntensityMax = InkCapsuleInterop.UiPseudoIntensityMax;
    }
}

public enum WizardSelectionRole
{
    FlatBlock,
    TopRow,
    BottomRow,
}

public readonly record struct WizardSelectionPath(
    int Row,
    int Column,
    int PresetIndex,
    WizardSelectionRole Role);

public readonly record struct WizardPreviewBlock(uint RenderMode, GlassInkProfileModel Profile);

public sealed class WizardPageSnapshot
{
    public WizardPageSnapshot(IReadOnlyList<WizardPreviewBlock> blocks, bool isPaired)
    {
        Blocks = blocks;
        IsPaired = isPaired;
    }

    public IReadOnlyList<WizardPreviewBlock> Blocks { get; }

    public bool IsPaired { get; }
}
