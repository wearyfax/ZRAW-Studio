// About tab: version, language and update check, the log folder for problem reports, the donation
// address, and credits for the projects ZRAW Studio is built on.
using System;
using System.Diagnostics;
using System.Drawing;
using System.IO;
using System.Net;
using System.Reflection;
using System.Text.RegularExpressions;
using System.Threading;
using System.Windows.Forms;
using ReaLTaiizor.Controls;
using Panel = System.Windows.Forms.Panel;

// The latest release on GitHub, from its tag ("v1.2.0")
static class Updates
{
    public const string Repo = "https://github.com/wearyfax/ZRAW-Studio";
    // From this assembly, not Application.ProductVersion, which reads whichever exe started the process
    public static readonly string Version = typeof(Updates).Assembly.GetCustomAttribute<AssemblyInformationalVersionAttribute>().InformationalVersion;

    public static Version Latest()
    {
        ServicePointManager.SecurityProtocol |= SecurityProtocolType.Tls12;   // GitHub refuses older TLS
        try
        {
            using (var w = new WebClient())
            {
                w.Headers[HttpRequestHeader.UserAgent] = "ZRAW-Studio/" + Version;
                w.Headers[HttpRequestHeader.Accept] = "application/vnd.github+json";
                var m = Regex.Match(w.DownloadString("https://api.github.com/repos/wearyfax/ZRAW-Studio/releases/latest"), "\"tag_name\"\\s*:\\s*\"v?([0-9]+(\\.[0-9]+){1,3})\"");
                if (!m.Success) throw new InvalidDataException(L.T("GitHub's reply has no version number"));
                return new Version(m.Groups[1].Value);
            }
        }
        catch (WebException ex)
        {
            var r = ex.Response as HttpWebResponse;
            if (r != null && r.StatusCode == HttpStatusCode.NotFound) throw new InvalidDataException(L.T("no release is published yet"));
            throw;
        }
    }

    // Missing parts count as 0: Version alone ranks 1.0.0.0 above 1.0.0, so the tag v1.0.0.0 looked newer
    static Version Full(Version v) { return new Version(v.Major, v.Minor, Math.Max(0, v.Build), Math.Max(0, v.Revision)); }
    public static bool IsNewer(Version v) { return Full(v) > Full(new Version(Version)); }
}

public class AboutPage : UserControl
{
    const int LeftW = 700, CreditW = 400;
    const string Monero = "839eRTjQkKyLsephY5jtxdU1NzRk1sGtw7vts9mbyBmvY8sPvA6LugijW7fj4YirRQXA7212LG2E74qNKYrhgs5CAV1ZMt3";

    readonly Label updateState = Z.Text1("", Z.Dim, LeftW, 22), problemState = Z.Text1("", Z.Dim, LeftW, 22), walletState = Z.Text1("", Z.Dim, LeftW, 22);
    readonly MetroButton check = Z.Button(L.T("Check for updates"), false), download = Z.Button(L.T("Download"), true);
    readonly ZCheck autoCheck = Z.Check(L.T("Check for updates when ZRAW Studio starts"));
    readonly MetroButton logs = Z.Button(L.T("Open log folder"), false), report = Z.Button(L.T("Report a problem"), false);
    readonly MetroButton copy = Z.Button(L.T("Copy address"), false), wallet = Z.Button(L.T("Open in wallet"), false);
    readonly MetroButton english = Z.Button("English", false), russian = Z.Button("Русский", false);   // each in its own language
    int checking;

    public AboutPage()
    {
        BackColor = Z.Panel;
        Font = Z.UiFont;

        // Left: the app, updates, problem reports, donations
        var main = Z.Section(DockStyle.Fill);
        main.Padding = new Padding(24, 20, 0, 14);
        var flow = Column();
        var address = Z.TextBox();
        address.Text = Monero;
        address.ReadOnly = true;
        address.Font = Z.MonoFont;
        address.Width = LeftW;
        autoCheck.Size = new Size(LeftW, 24);
        autoCheck.Margin = new Padding(0, 2, 0, 0);
        var languages = Z.Bar(DockStyle.None, english, russian);
        english.Margin = new Padding(0);   // the pair touches, like the List / Thumbnails switch
        Z.Active(english, !L.Ru);
        Z.Active(russian, L.Ru);
        flow.Controls.AddRange(new Control[] {
            Header(),
            Head(L.T("LANGUAGE")), languages,
            Head(L.T("UPDATES")), updateState, Z.Bar(DockStyle.None, check, download), autoCheck,
            Head(L.T("REPORT A PROBLEM")),
            Line(L.T("ZRAW Studio writes a log of each session. If something goes wrong, open the log folder and")),
            Line(L.T("attach the newest file to your report, together with what you were doing.")),
            Z.Bar(DockStyle.None, logs, report), problemState,
            Head(L.T("DONATE")),
            Line(L.T("ZRAW Studio is free. If it saves you time, you can support its development with Monero (XMR):")),
            address, Z.Bar(DockStyle.None, copy, wallet), walletState });
        main.Controls.Add(flow);

        // Right: credits and license
        var credits = Z.Section(DockStyle.Right);
        credits.Padding = new Padding(20, 20, 0, 14);
        credits.Width = CreditW + 40;
        var list = Column();
        list.Controls.Add(Head(L.T("CREDITS"), true));
        Credit(list, "zraw-parser", "storyboardcreativity, GPL-2.0", "https://github.com/storyboardcreativity/zraw-parser", L.T("ZRAW Studio grew out of this converter."));
        Credit(list, "zraw-decoder-lib", "storyboardcreativity, GPL-2.0", "https://github.com/storyboardcreativity/zraw-decoder-lib", L.T("The engine's ZRAW decoder is ported from this library."));
        Credit(list, "tiny-mov-library", "storyboardcreativity, MIT", "https://github.com/storyboardcreativity/tiny-mov-library", L.T("MOV reading and writing in the engine."));
        Credit(list, "ReaLTaiizor", "Taiizor, MIT", "https://github.com/Taiizor/ReaLTaiizor", L.T("The Metro controls that make up this window."));
        Credit(list, "tinydngloader", "Syoyo Fujita, MIT", "https://github.com/syoyo/tinydngloader", L.T("tiny_dng_writer writes the lossless and uncompressed DNGs."));
        Credit(list, "mpv", L.T("the mpv developers, client API under ISC"), "https://mpv.io", L.T("The ZRAW plugin runs inside mpv."));
        list.Controls.Add(Head(L.T("LICENSE")));
        list.Controls.Add(Note(L.T("ZRAW Studio is free software under the GNU GPL v2.")));
        list.Controls.Add(Link(L.T("Read the license"), Updates.Repo + "/blob/main/LICENSE"));
        credits.Controls.Add(list);

        Controls.Add(main);
        Controls.Add(new Panel { Dock = DockStyle.Right, Width = 10, BackColor = Z.Panel });
        Controls.Add(credits);

        download.Visible = problemState.Visible = walletState.Visible = false;   // status lines show up only when they have news
        updateState.Text = L.T("Not checked yet.");
        autoCheck.Checked = Settings.Get("updateCheck", "1") == "1";
        autoCheck.CheckedChanged += s => { Settings.Set("updateCheck", autoCheck.Checked ? "1" : "0"); Settings.Save(); };
        english.Click += (s, e) => Language("en");
        russian.Click += (s, e) => Language("ru");
        check.Click += (s, e) => Check(null);
        download.Click += (s, e) => Open(Updates.Repo + "/releases/latest", updateState);
        logs.Click += (s, e) =>
        {
            try { Directory.CreateDirectory(LogFile.Dir); Process.Start("explorer.exe", "\"" + LogFile.Dir + "\""); }
            catch (Exception ex) { Say(problemState, L.F("Could not open {0}: {1}", LogFile.Dir, ex.Message), Z.RedHover); }
        };
        report.Click += (s, e) => Open(Updates.Repo + "/issues", problemState);
        copy.Click += (s, e) =>
        {
            try { Clipboard.SetText(Monero); Say(walletState, L.T("Address copied. Thank you!"), Z.Good); }
            catch (Exception ex) { Say(walletState, L.F("Could not copy: {0}", ex.Message), Z.RedHover); }
        };
        wallet.Click += (s, e) =>
        {
            try { Process.Start("monero:" + Monero); }
            catch { Say(walletState, L.T("No Monero wallet opens monero: links on this PC. Copy the address instead."), Z.Dim); }
        };
    }

    // The text is chosen when the window opens, so switching reopens ZRAW Studio. Closing asks first if a
    // conversion is running; if that keeps the window open, the language stays as it is.
    void Language(string code)
    {
        if (code == L.Code) return;
        string before = Settings.Get("language", "");
        Settings.Set("language", code);
        var form = FindForm();
        form.Close();   // saves the settings
        if (form.IsDisposed) Process.Start(Application.ExecutablePath);
        else Settings.Set("language", before);
    }

    // Checks GitHub in the background. With `notify`, a quiet check at start-up: says nothing unless
    // a newer version exists, and then also tells `notify`.
    public void Check(Action<string> notify)
    {
        if (Interlocked.Exchange(ref checking, 1) == 1) return;
        if (!IsHandleCreated) CreateHandle();   // the tab may not have been shown yet; the answer comes back through it
        if (notify == null) Say(updateState, L.T("Checking..."), Z.Dim);
        ThreadPool.QueueUserWorkItem(_ =>
        {
            Version latest = null;
            string error = null;
            try { latest = Updates.Latest(); } catch (Exception ex) { error = ex.Message; }
            if (IsDisposed) return;   // the window closed meanwhile
            BeginInvoke(new Action(() =>
            {
                checking = 0;
                if (latest != null && Updates.IsNewer(latest))
                {
                    Say(updateState, L.F("Version {0} is available. You have {1}.", latest, Updates.Version), Color.White);
                    download.Visible = true;
                    if (notify != null) notify(L.F("ZRAW Studio {0} is available. Download it from the About tab.", latest));
                    LogFile.Append("Update available: " + latest);
                }
                else if (notify != null) return;
                else if (latest != null) Say(updateState, L.T("You have the latest version."), Z.Good);
                else Say(updateState, L.F("Could not check for updates: {0}", error), Z.RedHover);
            }));
        });
    }

    static FlowLayoutPanel Column()
    {
        var f = new FlowLayoutPanel { Dock = DockStyle.Fill, FlowDirection = FlowDirection.TopDown, WrapContents = false, BackColor = Z.Panel, AutoScroll = true };
        f.HandleCreated += (s, e) => Z.DarkScrollbars(f);
        return f;
    }

    // Icon, name, version and the project page
    static Control Header()
    {
        var p = new Panel { Size = new Size(LeftW, 76), BackColor = Z.Panel, Margin = new Padding(0, 0, 0, 4) };
        var logo = new PictureBox { Size = new Size(64, 64), Location = new Point(0, 4), SizeMode = PictureBoxSizeMode.CenterImage, BackColor = Z.Panel };
        try { using (var s = typeof(Z).Assembly.GetManifestResourceStream("icon.ico")) logo.Image = new Icon(s, 64, 64).ToBitmap(); } catch { }
        var name = new Label { Text = "ZRAW Studio", Font = Z.BigFont, ForeColor = Z.Text, BackColor = Z.Panel, AutoSize = true, Location = new Point(78, 0) };
        var version = Z.Text1(L.F("Version {0}", Updates.Version), Z.Dim, 300, 20);
        version.Location = new Point(81, 38);
        var page = Link(Updates.Repo.Substring("https://".Length), Updates.Repo);
        page.Location = new Point(81, 57);
        p.Controls.AddRange(new Control[] { logo, name, version, page });
        return p;
    }

    static MetroLabel Head(string text, bool first = false) { var l = Z.Label(text, Z.Dim, Z.HeadFont); l.Margin = new Padding(0, first ? 0 : 22, 0, 8); return l; }
    static Label Line(string text) { var l = Z.Text1(text, Z.Text, LeftW, 20); l.Margin = new Padding(0, 0, 0, 2); return l; }
    static Label Note(string text) { var l = Z.Text1(text, Z.Dim, CreditW, 20); l.Margin = new Padding(0, 0, 0, 2); return l; }

    static LinkLabel Link(string text, string url)
    {
        var l = new LinkLabel { Text = text, Font = Z.UiFont, AutoSize = true, BackColor = Z.Panel, LinkColor = Z.Text, VisitedLinkColor = Z.Text, ActiveLinkColor = Z.Red,
                                LinkBehavior = LinkBehavior.HoverUnderline, Margin = new Padding(0, 0, 0, 2), Padding = new Padding(0), UseMnemonic = false };
        l.LinkClicked += (s, e) => { try { Process.Start(url); } catch { } };
        return l;
    }

    // Project name as a link with its author and license dimmed beside it, then what ZRAW Studio uses it for
    static void Credit(Control list, string name, string byline, string url, string what)
    {
        var row = new Panel { Size = new Size(CreditW, 22), BackColor = Z.Panel, Margin = new Padding(0, 12, 0, 1) };
        var link = Link(name, url);
        link.Location = new Point(0, 1);
        var by = Z.Text1(byline, Z.Dim, CreditW - link.PreferredWidth - 6, 22);
        by.Location = new Point(link.PreferredWidth + 6, 0);
        row.Controls.AddRange(new Control[] { link, by });
        list.Controls.Add(row);
        list.Controls.Add(Note(what));
    }

    static void Say(Label l, string text, Color color) { l.Text = text; l.ForeColor = color; l.Visible = true; }

    static void Open(string url, Label status)
    {
        try { Process.Start(url); } catch (Exception ex) { Say(status, L.F("Could not open {0}: {1}", url, ex.Message), Z.RedHover); }
    }
}
