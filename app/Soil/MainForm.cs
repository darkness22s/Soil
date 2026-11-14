using Microsoft.Web.WebView2.Core;
using Microsoft.Web.WebView2.WinForms;

namespace Soil;

public sealed class MainForm : Form
{
    private readonly WebView2 m_web = new() { Dock = DockStyle.Fill };
    private readonly string m_url;
    private readonly SoilPaths m_paths;
    private readonly VmManager m_vmManager;
    private bool m_minimized;

    public MainForm(string url, SoilPaths paths, VmManager vmManager)
    {
        m_url = url;
        m_paths = paths;
        m_vmManager = vmManager;
        Text = "Soil";
        StartPosition = FormStartPosition.CenterScreen;
        ClientSize = new Size(1440, 900);
        MinimumSize = new Size(1024, 700);
        BackColor = Color.FromArgb(20, 24, 31);
        m_web.AllowExternalDrop = false;
        Controls.Add(m_web);
        Load += OnLoaded;
        Resize += OnResized;
    }

    // Park running guests into snapshots while the app is minimized, then
    // restore their saved state when the window is shown again.
    private void OnResized(object? sender, EventArgs e)
    {
        var minimized = WindowState == FormWindowState.Minimized;
        if (minimized == m_minimized)
            return;
        m_minimized = minimized;
        try
        {
            if (minimized)
                m_vmManager.IdleAll();
            else
                m_vmManager.ResumeAll();
        }
        catch
        {
        }
    }

    private async void OnLoaded(object? sender, EventArgs e)
    {
        try
        {
            Directory.CreateDirectory(m_paths.WebViewDir);
            var env = await CoreWebView2Environment.CreateAsync(null, m_paths.WebViewDir);
            await m_web.EnsureCoreWebView2Async(env);

            m_web.CoreWebView2.Settings.AreDevToolsEnabled = false;
            m_web.CoreWebView2.Settings.IsStatusBarEnabled = false;
            m_web.CoreWebView2.Settings.AreDefaultContextMenusEnabled = false;
            m_web.CoreWebView2.Settings.IsZoomControlEnabled = false;
            var allowedRoot = m_url.EndsWith('/') ? m_url : m_url + "/";
            m_web.CoreWebView2.NavigationStarting += (_, args) =>
            {
                if (!args.Uri.StartsWith(allowedRoot, StringComparison.OrdinalIgnoreCase))
                    args.Cancel = true;
            };
            m_web.CoreWebView2.NewWindowRequested += (_, args) => args.Handled = true;
            m_web.CoreWebView2.Navigate(m_url);
        }
        catch (Exception ex)
        {
            MessageBox.Show(
                "Soil needs the Microsoft Edge WebView2 runtime.\r\n\r\n" + ex.Message,
                "Soil", MessageBoxButtons.OK, MessageBoxIcon.Error);
            Close();
        }
    }
}
