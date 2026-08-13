using Windows.Networking.Connectivity;

namespace GlassSettings.Services;

public static class NetworkConnectivityService
{
    public static bool IsInternetAvailable()
    {
        var profile = NetworkInformation.GetInternetConnectionProfile();
        return profile?.GetNetworkConnectivityLevel() == NetworkConnectivityLevel.InternetAccess;
    }
}
