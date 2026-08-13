using System.ComponentModel;
using System.Runtime.CompilerServices;
using Microsoft.UI;
using Microsoft.UI.Xaml.Media;
using Windows.UI;

namespace GlassSettings.Services;

public sealed class InjectRuntimeRowModel : INotifyPropertyChanged
{
    private string _exeName = string.Empty;
    private string _detail = string.Empty;
    private Brush _beaconBrush = new SolidColorBrush(Colors.Gray);

    public event PropertyChangedEventHandler? PropertyChanged;

    public uint Pid { get; init; }

    public string ExeName
    {
        get => _exeName;
        set => SetField(ref _exeName, value);
    }

    public string Detail
    {
        get => _detail;
        set => SetField(ref _detail, value);
    }

    public Brush BeaconBrush
    {
        get => _beaconBrush;
        set => SetField(ref _beaconBrush, value);
    }

    public static Brush BrushFor(InjectRuntimeBeacon beacon) =>
        beacon switch
        {
            InjectRuntimeBeacon.Active => new SolidColorBrush(Color.FromArgb(255, 16, 124, 16)),
            InjectRuntimeBeacon.Rejected => new SolidColorBrush(Color.FromArgb(255, 196, 145, 0)),
            InjectRuntimeBeacon.Residual => new SolidColorBrush(Color.FromArgb(255, 196, 43, 28)),
            _ => new SolidColorBrush(Colors.Gray),
        };

    public void Apply(InjectRuntimeEntry entry)
    {
        ExeName = entry.ExeName;
        Detail = $"PID {entry.Pid} · {entry.ModulesText}";
        BeaconBrush = BrushFor(entry.Beacon);
    }

    private void SetField<T>(ref T field, T value, [CallerMemberName] string? name = null)
    {
        if (Equals(field, value))
            return;
        field = value;
        PropertyChanged?.Invoke(this, new PropertyChangedEventArgs(name));
    }
}
