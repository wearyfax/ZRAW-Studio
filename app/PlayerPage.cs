// Player tab: the player that double-click opens clips in, a drop box that plays files in mpv, and the ZRAW
// plugin for mpv (zraw.dll from engine\, which lets a normal mpv play ZRAW clips).
using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.Drawing;
using System.Drawing.Drawing2D;
using System.IO;
using System.Linq;
using System.Windows.Forms;
using Microsoft.Win32;
using ReaLTaiizor.Controls;
using Panel = System.Windows.Forms.Panel;

// Finding mpv, playing files in it, and its ZRAW plugin
static class Mpv
{
    public const string Download = "https://mpv.io/installation/";

    // Chosen by hand, else registered (App Paths, as mpv's installer does), else on PATH or in the usual places
    public static string Find()
    {
        string s = Settings.Get("mpvPath", "");
        if (s.Length > 0 && File.Exists(s)) return s;
        foreach (var root in new[] { Registry.CurrentUser, Registry.LocalMachine })
            try
            {
                using (var k = root.OpenSubKey(@"Software\Microsoft\Windows\CurrentVersion\App Paths\mpv.exe"))
                {
                    var v = k == null ? null : (k.GetValue("") as string ?? "").Trim('"');
                    if (!string.IsNullOrEmpty(v) && File.Exists(v)) return v;
                }
            }
            catch { }
        var places = (Environment.GetEnvironmentVariable("PATH") ?? "").Split(';').Where(d => d.Trim().Length > 0).Select(d => d.Trim())
            .Concat(new[] { Environment.GetFolderPath(Environment.SpecialFolder.ProgramFiles) + @"\mpv",
                            Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData) + @"\Programs\mpv",
                            Environment.GetFolderPath(Environment.SpecialFolder.UserProfile) + @"\scoop\apps\mpv\current" });
        foreach (var d in places)
            try { var f = Path.Combine(d, "mpv.exe"); if (File.Exists(f)) return f; } catch { }
        return null;
    }

    // mpv reads scripts from portable_config next to mpv.exe when that folder exists, else from %APPDATA%\mpv
    public static string ScriptsDir()
    {
        string exe = Find();
        if (exe != null)
        {
            string portable = Path.Combine(Path.GetDirectoryName(exe), "portable_config");
            if (Directory.Exists(portable)) return Path.Combine(portable, "scripts");
        }
        return Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.ApplicationData), "mpv", "scripts");
    }

    public static string PluginPath { get { return Path.Combine(ScriptsDir(), "zraw.dll"); } }

    // null: not installed; true: the same as the one in this exe; false: a different (older or newer) copy
    public static bool? PluginState()
    {
        try
        {
            if (!File.Exists(PluginPath)) return null;
            return File.ReadAllBytes(PluginPath).SequenceEqual(Embedded.Bytes("zraw.dll.gz"));
        }
        catch { return false; }
    }

    public static bool Play(IEnumerable<string> files, Action<string> log)
    {
        var list = files.ToList();
        string exe = Find();
        if (exe == null) { log(L.T("mpv is not installed. Click Get mpv on the Player tab.")); return false; }
        foreach (var f in list)
        {
            MovInfo info;
            try { info = MovInfo.Read(f); } catch { continue; }
            if ((info.TrueRaw || info.Hevc) && PluginState() == null) log(L.F("{0} is ZRAW: install the ZRAW plugin on the Player tab so mpv can play it.", Path.GetFileName(f)));
        }
        try
        {
            Process.Start(new ProcessStartInfo(exe, string.Join(" ", list.Select(f => "\"" + f + "\""))) { UseShellExecute = false });
            log(list.Count == 1 ? L.F("Playing {0} in mpv.", Path.GetFileName(list[0])) : L.F("Playing {0} {0:file|files} in mpv.", list.Count));
            return true;
        }
        catch (Exception ex) { log(L.F("Could not start mpv: {0}", ex.Message)); return false; }
    }
}

public class PlayerPage : UserControl
{
    const int OW = 296, LW = 92;

    readonly RichTextBox log = Z.Log();
    readonly MetroButton pick = Z.Button(L.T("Play files..."), false), getMpv = Z.Button(L.T("Get mpv"), false), locate = Z.Button(L.T("Locate mpv.exe..."), false);
    readonly MetroButton install = Z.Button(L.T("Install plugin"), true), uninstall = Z.Button(L.T("Remove plugin"), false);
    readonly MetroComboBox player = Z.Combo(L.T("ZRAW Studio preview"), "mpv");
    readonly MetroComboBox lookColor = Z.Combo(Look.Names);
    readonly MetroComboBox lookExposure = Z.Combo(Look.Exposures.Select(e => e == 0 ? L.T("0 (as shot)") : L.F("{0:+0.0;-0.0} stops", e)).ToArray());
    readonly Label mpvState = Z.Text1("", Z.Text, OW - LW, 22), mpvPath = Z.Text1("", Z.Dim, OW, 20);
    readonly Label pluginState = Z.Text1("", Z.Text, OW - LW, 22), pluginPath = Z.Text1("", Z.Dim, OW, 20);
    readonly ToolTip toolTip = new ToolTip { AutoPopDelay = 32767 };   // the longest Windows allows (about 33 s); the default 5 s is too short to read long tooltips

    public PlayerPage()
    {
        BackColor = Z.Panel;
        Font = Z.UiFont;

        // Left: toolbar + drop box
        var toolbar = new Panel { Dock = DockStyle.Top, Height = Z.RowHeight + 10, BackColor = Z.Panel };
        toolbar.Controls.Add(Z.Bar(DockStyle.Left, pick));
        var drop = new DropBox();
        var left = new Panel { Dock = DockStyle.Fill, BackColor = Z.Panel, Padding = new Padding(0, 0, 10, 0) };
        left.Controls.Add(drop);
        left.Controls.Add(toolbar);

        // Right: settings, same column layout as the Convert tab
        var opts = Z.Section(DockStyle.Right);
        // No right padding: the flow keeps that room itself, so when the column is taller than the
        // window its vertical scroll bar fits without forcing a horizontal one
        opts.Padding = new Padding(16, 14, 0, 14);
        opts.Width = OW + 34;
        var flow = new FlowLayoutPanel { Dock = DockStyle.Fill, FlowDirection = FlowDirection.TopDown, WrapContents = false, BackColor = Z.Panel, AutoScroll = true };
        flow.HandleCreated += (s, e) => Z.DarkScrollbars(flow);
        player.Width = lookColor.Width = lookExposure.Width = OW - LW;
        flow.Controls.AddRange(new Control[] {
            Head(L.T("PLAYBACK"), true), Field(L.T("Double-click"), player),
            Note(L.T("Used when you double-click a clip.")), Note(L.T("Other videos: mpv, or else your usual player.")),
            Head(L.T("PREVIEW LOOK"), false), Field(L.T("Color"), lookColor), Field(L.T("Exposure"), lookExposure),
            Note(L.T("For RAW clips in ZRAW Studio's preview,")), Note(L.T("thumbnails and mpv (from the next clip).")),
            Head("MPV", false), Field(L.T("Status"), mpvState), mpvPath, Z.Bar(DockStyle.None, getMpv, locate),
            Head(L.T("ZRAW PLUGIN FOR MPV"), false), Field(L.T("Status"), pluginState), pluginPath, Z.Bar(DockStyle.None, install, uninstall),
            Note(L.T("Lets mpv play ZRAW clips: RAW at 4K, and HEVC.")), Note(L.T("Ctrl+Up/Down: exposure")), Note(L.T("Ctrl+Left/Right: warmth")),
            Note(L.T("Ctrl+0: default exposure    Ctrl+H: 4K or half")), Note(L.T("4K needs vo=gpu-next in mpv.conf.")),
            Note(L.T("HEVC ZRAW: Ctrl+H switches GPU/CPU decoding.")) });
        opts.Controls.Add(flow);

        var bottom = new Panel { Dock = DockStyle.Bottom, Height = 180, BackColor = Z.Panel, Padding = new Padding(0, 10, 0, 0) };
        var logFrame = Z.Section(DockStyle.Fill);
        logFrame.Padding = new Padding(8);
        logFrame.BackColor = Z.Ink;
        logFrame.Controls.Add(log);
        bottom.Controls.Add(logFrame);

        Controls.Add(left);
        Controls.Add(opts);
        Controls.Add(bottom);

        player.SelectedIndex = Settings.Get("player", "studio") == "mpv" ? 1 : 0;
        player.SelectedIndexChanged += (s, e) => Settings.Set("player", player.SelectedIndex == 1 ? "mpv" : "studio");
        lookColor.SelectedIndex = Look.Matrix;
        lookExposure.SelectedIndex = Math.Max(0, Array.IndexOf(Look.Exposures, Look.Exposure));
        EventHandler lookChanged = (s, e) =>
        {
            if (lookColor.SelectedIndex < 0 || lookExposure.SelectedIndex < 0) return;
            Look.Set(lookColor.SelectedIndex, Look.Exposures[lookExposure.SelectedIndex]);
            ThumbView.LookChanged();
            if (Mpv.PluginState() == null) return;
            try { Look.WriteMpvConf(); }
            catch (Exception ex) { Write(L.F("ERROR: could not write {0} ({1}).", Look.ConfPath, ex.Message)); }
        };
        lookColor.SelectedIndexChanged += lookChanged;
        lookExposure.SelectedIndexChanged += lookChanged;
        pick.Click += (s, e) =>
        {
            using (var d = new OpenFileDialog { RestoreDirectory = true, Multiselect = true, Title = L.T("Play in mpv"), Filter = L.T("Video files|*.mov;*.mp4;*.mkv;*.mxf;*.avi;*.zraw|All files|*.*") })
                if (d.ShowDialog(this) == DialogResult.OK) Play(d.FileNames);
        };
        drop.Click += (s, e) => pick.PerformClick();
        getMpv.Click += (s, e) => { try { Process.Start(Mpv.Download); } catch (Exception ex) { Write(L.F("Could not open {0}: {1}", Mpv.Download, ex.Message)); } };
        locate.Click += (s, e) =>
        {
            using (var d = new OpenFileDialog { RestoreDirectory = true, Title = L.T("Where is mpv.exe?"), Filter = L.T("mpv|mpv.exe|Programs|*.exe") })
                if (d.ShowDialog(this) == DialogResult.OK) { Settings.Set("mpvPath", d.FileName); Settings.Save(); UpdateStatus(); }
        };
        install.Click += (s, e) =>
        {
            try
            {
                Directory.CreateDirectory(Mpv.ScriptsDir());
                File.WriteAllBytes(Mpv.PluginPath, Embedded.Bytes("zraw.dll.gz"));
                Look.WriteMpvConf();
                Write(L.F("Installed the ZRAW plugin: {0}", Mpv.PluginPath));
            }
            catch (IOException ex) { Write(L.F("ERROR: could not write the plugin ({0}). If mpv is open, close it and try again.", ex.Message)); }
            catch (Exception ex) { Write(L.F("ERROR: {0}", ex.Message)); }
            UpdateStatus();
        };
        uninstall.Click += (s, e) =>
        {
            try { File.Delete(Mpv.PluginPath); Write(L.T("Removed the ZRAW plugin.")); }
            catch (Exception ex) { Write(L.F("ERROR: could not remove the plugin ({0}). If mpv is open, close it and try again.", ex.Message)); }
            UpdateStatus();
        };
        VisibleChanged += (s, e) => { if (Visible) UpdateStatus(); };   // mpv may have been installed meanwhile
        UpdateStatus();
        log.HandleCreated += (s, e) => Write(L.T("Drop videos anywhere in this window to play them in mpv."));
    }

    static MetroLabel Head(string text, bool first) { var l = Z.Label(text, Z.Dim, Z.HeadFont); l.Margin = new Padding(0, first ? 0 : 16, 0, 6); return l; }
    static Label Note(string text) { var l = Z.Text1(text, Z.Dim, OW, 18); l.Margin = new Padding(0, 0, 0, 2); return l; }

    static Control Field(string label, Control c)
    {
        var p = new Panel { Size = new Size(OW, c.Height), BackColor = Z.Panel, Margin = new Padding(0, 0, 0, 6) };
        p.Controls.Add(Z.Text1(label, Z.Dim, LW, c.Height));
        c.Location = new Point(LW, 0);
        c.Margin = new Padding(0);
        p.Controls.Add(c);
        return p;
    }

    void Write(string s) { Z.Write(log, s); }

    void UpdateStatus()
    {
        string exe = Mpv.Find();
        mpvState.Text = L.T(exe != null ? "Found" : "Not installed");
        mpvState.ForeColor = exe != null ? Z.Good : Z.RedHover;
        mpvPath.Text = exe != null ? Z.ShortPath(exe, mpvPath) : L.T("Get it from mpv.io, then come back here.");
        toolTip.SetToolTip(mpvPath, exe ?? Mpv.Download);
        getMpv.Visible = exe == null;

        var state = Mpv.PluginState();
        pluginState.Text = L.T(state == null ? "Not installed" : state.Value ? "Installed" : "Installed, a different version");
        pluginState.ForeColor = state == true ? Z.Good : state == null ? Z.Dim : Z.Text;
        pluginPath.Text = Z.ShortPath(Mpv.PluginPath, pluginPath);
        toolTip.SetToolTip(pluginPath, Mpv.PluginPath);
        install.Text = L.T(state == false ? "Update plugin" : "Install plugin");
        install.Enabled = state != true;
        uninstall.Enabled = state != null;
    }

    public void Play(IEnumerable<string> paths)
    {
        var files = paths.Where(File.Exists).ToList();
        if (files.Count == 0) { Write(L.T("Drop video files, not folders.")); return; }
        Mpv.Play(files, Write);
    }

    // The big target in the middle of the tab: dashed outline, red while something is dragged over it
    class DropBox : Panel
    {
        bool over;
        public DropBox()
        {
            Dock = DockStyle.Fill;
            BackColor = Z.Panel;
            DoubleBuffered = true;
            ResizeRedraw = true;
            Cursor = Cursors.Hand;
            DragEnter += (s, e) => { over = true; Invalidate(); };
            DragLeave += (s, e) => { over = false; Invalidate(); };
            DragDrop += (s, e) => { over = false; Invalidate(); };
        }

        protected override void OnPaint(PaintEventArgs e)
        {
            var g = e.Graphics;
            using (var pen = new Pen(over ? Z.Red : Z.Line, 2) { DashStyle = DashStyle.Dash })
                g.DrawRectangle(pen, 1, 1, Width - 3, Height - 3);
            var mid = Height / 2;
            TextRenderer.DrawText(g, L.T("Drop video files here"), Z.BigFont, new Rectangle(0, mid - 44, Width, 44), over ? Color.White : Z.Text, TextFormatFlags.HorizontalCenter | TextFormatFlags.Bottom);
            TextRenderer.DrawText(g, L.T("They play in mpv. Or click to choose files."), Z.UiFont, new Rectangle(0, mid + 6, Width, 24), Z.Dim, TextFormatFlags.HorizontalCenter | TextFormatFlags.Top);
        }
    }
}
