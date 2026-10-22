namespace Boxify;

internal static class Program
{
    [STAThread]
    private static void Main()
    {
        ApplicationConfiguration.Initialize();

        var manager = new VmManager(AppContext.BaseDirectory);
        using var server = new AppServer(manager);
        server.Start();

        try
        {
            using var form = new MainForm(server.Url);
            Application.Run(form);
        }
        finally
        {
            manager.StopAll();
        }
    }
}
