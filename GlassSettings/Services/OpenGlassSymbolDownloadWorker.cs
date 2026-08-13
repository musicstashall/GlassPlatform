using System.Net;
using System.Net.Http;
using System.Net.Sockets;

namespace GlassSettings.Services;

internal sealed class OpenGlassSymbolDownloadWorker
{
    private const int SymbolDownloadTimeoutSeconds = 5;
    private const int ReadBufferSize = 128 * 1024;

    private static readonly HttpClient Http = CreateHttpClient();

    private static readonly (string Name, int BasePercent)[] Modules =
    [
        ("uDWM.dll", 0),
        ("dwmcore.dll", 50),
    ];

    public async Task<OpenGlassSymbolDownloadCompletion> DownloadAsync(
        string cacheRoot,
        OpenGlassSymbolCacheLayoutKind layout,
        IProgress<SymbolDownloadProgress> progress,
        CancellationToken cancellationToken)
    {
        try
        {
            var prepareResult = PrepareCacheDirectory(cacheRoot);
            if (prepareResult.HasValue)
                return prepareResult.Value;

            foreach (var module in Modules)
            {
                cancellationToken.ThrowIfCancellationRequested();

                var modulePath = GetSystemModulePath(module.Name);
                if (modulePath is null)
                {
                    return Failed(
                        unchecked((int)0x80070002),
                        $"Failed to locate {module.Name} in the system directory.");
                }

                var info = PeSymbolModuleInfoReader.TryBuild(modulePath, module.Name);
                if (info is null)
                {
                    return Failed(
                        unchecked((int)0x80070002),
                        $"Failed to prepare symbol download for {module.Name}.\nModule: {modulePath}");
                }

                progress.Report(new SymbolDownloadProgress(module.BasePercent, info.Value.PdbFileName));

                var destinationPath = BuildDestinationPath(cacheRoot, layout, info.Value);
                Directory.CreateDirectory(Path.GetDirectoryName(destinationPath)!);

                var downloadResult = await DownloadFileAsync(
                    info.Value.Url,
                    destinationPath,
                    info.Value.PdbFileName,
                    module.BasePercent,
                    progress,
                    cancellationToken).ConfigureAwait(false);

                if (downloadResult.HasValue)
                    return downloadResult.Value;

                progress.Report(new SymbolDownloadProgress(module.BasePercent + 50, info.Value.PdbFileName));
            }

            progress.Report(new SymbolDownloadProgress(100, null));
            return new OpenGlassSymbolDownloadCompletion(
                OpenGlassSymbolDownloadOutcomeKind.Success,
                0,
                "Required symbols downloaded successfully.");
        }
        catch (OperationCanceledException)
        {
            return new OpenGlassSymbolDownloadCompletion(
                OpenGlassSymbolDownloadOutcomeKind.Cancelled,
                unchecked((int)0x800704C7),
                "Symbol download cancelled.");
        }
        catch (HttpRequestException ex)
        {
            return FailedFromHttp(ex);
        }
        catch (Exception ex)
        {
            return Failed(
                unchecked((int)0x80004005),
                ex.Message);
        }
    }

    private static async Task<OpenGlassSymbolDownloadCompletion?> DownloadFileAsync(
        string url,
        string destinationPath,
        string pdbFileName,
        int basePercent,
        IProgress<SymbolDownloadProgress> progress,
        CancellationToken cancellationToken)
    {
        var completed = false;

        try
        {
            RemoveDestinationPath(destinationPath);

            using var response = await WithOperationTimeoutAsync(
                token => Http.GetAsync(url, HttpCompletionOption.ResponseHeadersRead, token),
                cancellationToken).ConfigureAwait(false);

            response.EnsureSuccessStatusCode();

            var totalBytes = response.Content.Headers.ContentLength;
            await using var stream = await WithOperationTimeoutAsync(
                token => response.Content.ReadAsStreamAsync(token),
                cancellationToken).ConfigureAwait(false);
            await using var file = new FileStream(
                destinationPath,
                FileMode.Create,
                FileAccess.Write,
                FileShare.None,
                ReadBufferSize,
                useAsync: true);

            var buffer = new byte[ReadBufferSize];
            long downloaded = 0;

            while (true)
            {
                cancellationToken.ThrowIfCancellationRequested();

                var read = await WithOperationTimeoutAsync(
                    async token => await stream.ReadAsync(buffer.AsMemory(0, buffer.Length), token)
                        .ConfigureAwait(false),
                    cancellationToken).ConfigureAwait(false);
                if (read == 0)
                    break;

                await file.WriteAsync(buffer.AsMemory(0, read), cancellationToken).ConfigureAwait(false);
                downloaded += read;

                int percent;
                if (totalBytes is > 0)
                {
                    var filePercent = (int)(downloaded * 100 / totalBytes.Value);
                    percent = Math.Clamp(basePercent + filePercent / 2, 0, 100);
                }
                else
                {
                    percent = basePercent;
                }

                progress.Report(new SymbolDownloadProgress(percent, pdbFileName));
            }

            completed = true;
            return null;
        }
        catch (OperationCanceledException) when (!cancellationToken.IsCancellationRequested)
        {
            var failureKind = ClassifyOperationTimeoutFailure();
            var message = failureKind == OpenGlassSymbolDownloadFailureKind.TimedOut
                ? $"Timed out after {SymbolDownloadTimeoutSeconds} seconds while downloading {pdbFileName}."
                : string.Empty;

            return Failed(unchecked((int)0x800705B4), message, failureKind);
        }
        catch (OperationCanceledException)
        {
            throw;
        }
        catch (HttpRequestException ex)
        {
            return FailedFromHttp(ex);
        }
        finally
        {
            if (!completed)
                RemoveDestinationPath(destinationPath);
        }
    }

    private static OpenGlassSymbolDownloadCompletion? PrepareCacheDirectory(string cacheRoot)
    {
        try
        {
            if (Directory.Exists(cacheRoot))
                Directory.Delete(cacheRoot, recursive: true);

            Directory.CreateDirectory(cacheRoot);
            return null;
        }
        catch (IOException ex)
        {
            return Failed(
                unchecked((int)0x80070020),
                $"Failed to clear symbol cache directory.\n{cacheRoot}\n{ex.Message}");
        }
        catch (UnauthorizedAccessException ex)
        {
            return Failed(
                unchecked((int)0x80070005),
                $"Access denied while clearing symbol cache directory.\n{cacheRoot}\n{ex.Message}");
        }
    }

    private static void RemoveDestinationPath(string destinationPath)
    {
        try
        {
            if (Directory.Exists(destinationPath))
                Directory.Delete(destinationPath, recursive: true);
            else if (File.Exists(destinationPath))
                File.Delete(destinationPath);
        }
        catch
        {
            /* best effort */
        }
    }

    private static async Task<T> WithOperationTimeoutAsync<T>(
        Func<CancellationToken, Task<T>> operation,
        CancellationToken cancellationToken)
    {
        using var linkedCts = CancellationTokenSource.CreateLinkedTokenSource(cancellationToken);
        linkedCts.CancelAfter(TimeSpan.FromSeconds(SymbolDownloadTimeoutSeconds));
        return await operation(linkedCts.Token).ConfigureAwait(false);
    }

    private static string BuildDestinationPath(
        string cacheRoot,
        OpenGlassSymbolCacheLayoutKind layout,
        PeSymbolModuleInfoReader.ModuleDownloadInfo info)
    {
        if (layout == OpenGlassSymbolCacheLayoutKind.SymSrvNested)
            return Path.Combine(cacheRoot, info.PdbFileName, info.Identifier, info.PdbFileName);

        return Path.Combine(cacheRoot, info.PdbFileName);
    }

    private static string? GetSystemModulePath(string moduleName)
    {
        var systemDir = Environment.GetFolderPath(Environment.SpecialFolder.System);
        if (string.IsNullOrWhiteSpace(systemDir))
            return null;

        var path = Path.Combine(systemDir, moduleName);
        return File.Exists(path) ? path : null;
    }

    private static OpenGlassSymbolDownloadCompletion FailedFromHttp(HttpRequestException ex)
    {
        var failureKind = ClassifyHttpFailure(ex);
        return Failed(unchecked((int)0x80072EFD), DescribeHttpFailure(ex), failureKind);
    }

    private static OpenGlassSymbolDownloadCompletion Failed(
        int hResult,
        string message,
        OpenGlassSymbolDownloadFailureKind failureKind = OpenGlassSymbolDownloadFailureKind.Other) =>
        new(OpenGlassSymbolDownloadOutcomeKind.Failed, hResult, message, failureKind);

    private static OpenGlassSymbolDownloadFailureKind ClassifyOperationTimeoutFailure()
    {
        if (!NetworkConnectivityService.IsInternetAvailable())
            return OpenGlassSymbolDownloadFailureKind.ConnectionLost;

        return OpenGlassSymbolDownloadFailureKind.TimedOut;
    }

    private static OpenGlassSymbolDownloadFailureKind ClassifyHttpFailure(HttpRequestException ex)
    {
        if (!NetworkConnectivityService.IsInternetAvailable())
            return OpenGlassSymbolDownloadFailureKind.ConnectionLost;

        if (IsLikelyConnectionHttpFailure(ex))
            return OpenGlassSymbolDownloadFailureKind.ConnectionLost;

        return OpenGlassSymbolDownloadFailureKind.Other;
    }

    private static bool IsLikelyConnectionHttpFailure(HttpRequestException ex)
    {
        if (ex.StatusCode == HttpStatusCode.NotFound)
            return false;

        if (ex.HttpRequestError is HttpRequestError.ConnectionError
            or HttpRequestError.NameResolutionError
            or HttpRequestError.SecureConnectionError)
        {
            return true;
        }

        for (var inner = ex.InnerException; inner is not null; inner = inner.InnerException)
        {
            if (inner is SocketException or IOException)
                return true;
        }

        return ex.StatusCode is null;
    }

    private static string DescribeHttpFailure(HttpRequestException ex)
    {
        if (ex.StatusCode == HttpStatusCode.NotFound)
            return "Symbol file was not found on Microsoft Symbol Server for the current Windows build.";

        return string.IsNullOrWhiteSpace(ex.Message)
            ? "Failed to connect to Microsoft Symbol Server."
            : ex.Message;
    }

    private static HttpClient CreateHttpClient()
    {
        var client = new HttpClient
        {
            Timeout = Timeout.InfiniteTimeSpan
        };
        client.DefaultRequestHeaders.UserAgent.ParseAdd("GlassSettings/1.0");
        return client;
    }
}
