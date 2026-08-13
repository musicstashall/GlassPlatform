namespace GlassSettings.TextWizard;

/// <summary>GDI preview требует STA — thread pool (MTA) даёт пустые/одинаковые буферы.</summary>
internal static class WizardPreviewRenderScheduler
{
    public static Task<T> RunOnStaThread<T>(Func<T> work)
    {
        var tcs = new TaskCompletionSource<T>(TaskCreationOptions.RunContinuationsAsynchronously);
        var thread = new Thread(() =>
        {
            try
            {
                tcs.SetResult(work());
            }
            catch (Exception ex)
            {
                tcs.SetException(ex);
            }
        })
        {
            IsBackground = true,
            Name = "WizardPreviewSta",
        };
        thread.SetApartmentState(ApartmentState.STA);
        thread.Start();
        return tcs.Task;
    }
}
