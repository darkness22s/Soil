namespace Soil;

internal static class Program
{
    [STAThread]
    private static void Main(string[] args)
    {
        var paths = SoilPaths.CreateDefault();
        paths.Ensure();

        if (args.Any(arg => string.Equals(arg, "--bootstrap", StringComparison.OrdinalIgnoreCase)))
            return;

        ApplicationConfiguration.Initialize();

        var manager = new VmManager(AppContext.BaseDirectory, paths);
        using var server = new AppServer(manager, paths);
        server.Start();

        try
        {
            using var form = new MainForm(server.Url, paths);
            Application.Run(form);
        }
        finally
        {
            manager.StopAll();
        }
    }
}
