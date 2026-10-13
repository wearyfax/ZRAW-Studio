// Preview window for one ZRAW clip: a frame at half resolution in the Player tab's look, with a scrub bar.
// Frames come from zrawcore.dll on a background thread; while you drag, only the latest position is
// rendered, so the bar never queues up work.
using System;
using System.Drawing;
using System.Globalization;
using System.IO;
using System.Threading.Tasks;
using System.Windows.Forms;
using ReaLTaiizor.Controls;
using ReaLTaiizor.Forms;
using Panel = System.Windows.Forms.Panel;

public class PreviewForm : MetroForm
{
    readonly string path;
    readonly int frames;
    readonly double fps;
    readonly PictureBox pic = new PictureBox { Dock = DockStyle.Fill, SizeMode = PictureBoxSizeMode.Zoom, BackColor = Z.Ink };
    readonly MetroTrackBar bar;
    readonly Label pos = Z.Text1("", Z.Text, 230, 22);
    readonly object gate = new object();
    int want, shown = -1;
    bool busy;

    // Double-click on a clip. The Player tab picks ZRAW Studio's preview or mpv; RAW clips fall back to
    // the preview when mpv lacks the ZRAW plugin, other files to the usual Windows player.
    public static void Open(string path, IWin32Window owner, Action<string> log)
    {
        try
        {
            var info = MovInfo.Read(path);
            bool mpv = Settings.Get("player", "studio") == "mpv" && Mpv.Find() != null;
            if (info.Hevc)
            {
                // HEVC ZRAW plays in mpv through the ZRAW plugin (descrambled on the fly); there is no Studio preview for it
                if (!info.Convertible) { log(L.F("{0} is an HEVC ZRAW variant this version does not recognize.", Path.GetFileName(path))); return; }
                if (Mpv.Find() != null && Mpv.PluginState() != null && Mpv.Play(new[] { path }, log)) return;
                log(L.F("{0} is HEVC ZRAW: install mpv and the ZRAW plugin (Player tab) to play it.", Path.GetFileName(path)));
                return;
            }
            if (mpv && (!info.TrueRaw || Mpv.PluginState() != null) && Mpv.Play(new[] { path }, log)) return;
            if (mpv && info.TrueRaw) log(L.T("The ZRAW plugin for mpv is not installed (Player tab), so this RAW clip opens in ZRAW Studio's preview."));
            if (info.TrueRaw) new PreviewForm(path, info.Frames, info.Fps).Show(owner);
            else System.Diagnostics.Process.Start(path);
        }
        catch (Exception ex) { log(L.F("Could not open {0}: {1}", path, ex.Message)); }
    }

    public PreviewForm(string path, int frames, double fps)
    {
        this.path = path;
        this.frames = Math.Max(1, frames);
        this.fps = fps > 0 ? fps : 30;
        Text = Path.GetFileName(path);
        Size = new Size(1280, 800);
        MinimumSize = new Size(720, 480);
        StartPosition = FormStartPosition.CenterParent;
        Z.Dress(this);

        bar = Z.Slider(0, Math.Max(1, this.frames - 1), 0);
        bar.Dock = DockStyle.Fill;
        bar.Margin = new Padding(0);
        pos.Dock = DockStyle.Right;
        pos.TextAlign = ContentAlignment.MiddleRight;
        var bottom = new Panel { Dock = DockStyle.Bottom, Height = 50, BackColor = Z.Panel, Padding = new Padding(12, 14, 12, 14) };
        bottom.Controls.Add(bar);
        bottom.Controls.Add(pos);
        Controls.Add(pic);
        Controls.Add(bottom);

        bar.Scroll += s => Request(bar.Value);
        // Metro's slider only drags from its small handle; click anywhere on the bar to jump, then drag
        bool dragging = false;
        Action<int> seekTo = x =>
        {
            int f = (int)Math.Round((double)Math.Max(0, Math.Min(bar.Width, x)) / Math.Max(1, bar.Width) * (this.frames - 1));
            if (f != bar.Value) bar.Value = f;
            Request(f);
        };
        bar.MouseDown += (s, e) => { if (e.Button != MouseButtons.Left) return; dragging = true; seekTo(e.X); };
        bar.MouseMove += (s, e) => { if (dragging) seekTo(e.X); };
        bar.MouseUp += (s, e) => dragging = false;
        KeyPreview = true;
        KeyDown += (s, e) =>
        {
            int step = e.Shift ? 10 : 1, f = bar.Value;
            switch (e.KeyCode)
            {
                case Keys.Left: f -= step; break;
                case Keys.Right: f += step; break;
                case Keys.Home: f = 0; break;
                case Keys.End: f = this.frames - 1; break;
                case Keys.Escape: Close(); return;
                default: return;
            }
            e.Handled = true;
            bar.Value = Math.Max(0, Math.Min(this.frames - 1, f));
            Request(bar.Value);
        };
        pic.MouseWheel += (s, e) => { bar.Value = Math.Max(0, Math.Min(this.frames - 1, bar.Value - Math.Sign(e.Delta))); Request(bar.Value); };
        Shown += (s, e) => { pic.Focus(); Request(0); };
        FormClosed += (s, e) => { if (pic.Image != null) pic.Image.Dispose(); };
    }

    void Request(int f)
    {
        var t = TimeSpan.FromSeconds(f / fps);
        pos.Text = L.F("Frame {0} of {1}     {2}:{3:00}.{4:00}", f + 1, frames, (int)t.TotalMinutes, t.Seconds, f % (int)Math.Round(fps));
        lock (gate)
        {
            want = f;
            if (busy) return;
            busy = true;
        }
        Task.Run(() => Worker());
    }

    void Worker()
    {
        while (true)
        {
            int f;
            lock (gate)
            {
                if (want == shown || IsDisposed) { busy = false; return; }
                f = want;
                shown = f;
            }
            Bitmap b = null;
            string err = null;
            try { b = ZrawCore.Render(path, f); } catch (Exception ex) { err = ex.Message; }
            try
            {
                BeginInvoke(new Action(() =>
                {
                    if (err != null) { pos.Text = L.F("Cannot show this frame: {0}", err); return; }
                    var old = pic.Image;
                    pic.Image = b;
                    if (old != null) old.Dispose();
                }));
            }
            catch { if (b != null) b.Dispose(); lock (gate) busy = false; return; }   // window closed meanwhile
        }
    }
}
