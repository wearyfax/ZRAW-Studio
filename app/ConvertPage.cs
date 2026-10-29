// Convert tab: a list (or thumbnails) of clips, output options with the lossy compression slider, and
// the zrawdng engine running as a child process. The engine reports through tab-separated lines on
// stderr (zrawdng convert --progress): @clip <path> <frames> <dir>, @progress <done> <total> <fps>,
// @skip <path> <reason>, @done <path> <ok> <failed frames> <seconds> <fps>; anything else is log text.
using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.Drawing;
using System.Drawing.Drawing2D;
using System.Drawing.Imaging;
using System.Globalization;
using System.Runtime.InteropServices;
using System.IO;
using System.Linq;
using System.Text;
using System.Text.RegularExpressions;
using System.Threading.Tasks;
using System.Windows.Forms;
using ReaLTaiizor.Controls;
using Panel = System.Windows.Forms.Panel;

public class ConvertPage : UserControl
{
    class Clip
    {
        public string Path;
        public MovInfo Info;
        public DataGridViewRow Row;   // the grid row holds the status text and selection for both views
        public Thumb T;               // also the row's Tag
        public int Done;
        public bool Finished;
        public string OutDir;          // from the engine's @clip line
        public byte[] FolderIco;       // the folder icon, rendered when the run starts (FolderIcon)
        public bool Ok, IconDone;
    }

    const int OW = 296, LW = 92;   // options column width and its label column

    readonly List<Clip> clips = new List<Clip>();
    readonly DataGridView grid = Z.Grid();
    readonly ThumbView thumbs;
    readonly RichTextBox log = Z.Log();
    readonly MetroButton add = Z.Button(L.T("Add clips..."), false), addFolder = Z.Button(L.T("Add folder..."), false), remove = Z.Button(L.T("Remove"), false), clear = Z.Button(L.T("Clear"), false);
    readonly MetroButton start = Z.Button(L.T("Start"), true), pause = Z.Button(L.T("Pause"), false), stop = Z.Button(L.T("Stop"), false), browse = Z.Button(L.T("Browse..."), false), openOut = Z.Button(L.T("Open"), false);
    readonly MetroComboBox format = Z.Combo(L.T("Lossy CinemaDNG (Resolve)"), L.T("Lossless CinemaDNG"), L.T("Uncompressed"), L.T("Uncompressed, packed"));
    readonly MetroComboBox scale = Z.Combo(L.T("Full resolution"), L.T("Half resolution"), L.T("Quarter resolution"));
    readonly MetroComboBox denoise = Z.Combo(L.T("Off"), L.T("Light"), L.T("Strong"));   // engine --denoise [light|strong]: temporal raw denoise on the GPU
    readonly MetroComboBox colorMatrix = Z.Combo(Look.Names);   // DNG color matrices (engine default / --zcam-dng-color / --camera-color / --videosuite-color)
    readonly MetroTrackBar ratio = Z.Slider(15, 120, 30);   // tenths: 1.5:1 to 12:1
    readonly MetroComboBox rateMode = Z.Combo(L.T("Fixed ratio"), L.T("Fixed quality per shot"));
    readonly MetroTrackBar level = Z.Slider(0, 7, 2);        // fixed quality: index into the Level tables below
    readonly MetroLabel ratioText = Z.Label("3.0 : 1", Color.White, Z.BigFont);
    readonly MetroLabel equivText = Z.Label(L.T("equivalent"), Z.Dim, Z.UiFont);   // after a fixed-quality level name
    readonly Label sizeText = Z.Text1("", Z.Text, OW - LW, 22), errorText = Z.Text1("", Z.Text, OW - LW, 22), gradeText = Z.Text1("", Z.Text, OW - LW, 22), proresText = Z.Text1("", Z.Text, OW - LW, 22);
    readonly MetroSwitch gpu = Z.Switch();
    // One checkbox per GPU (gpuinfo order, fastest first); several can share the work
    readonly FlowLayoutPanel gpuList = new FlowLayoutPanel { FlowDirection = FlowDirection.TopDown, WrapContents = false, AutoSize = true, BackColor = Z.Panel, Margin = new Padding(LW, 0, 0, 6) };
    readonly List<ZCheck> gpuBoxes = new List<ZCheck>();
    readonly ZNumeric threads = Z.Numeric(1, Environment.ProcessorCount, Environment.ProcessorCount);   // up to the logical CPU count
    readonly MetroTextBox output = Z.TextBox();
    // Off at every start on purpose: deleting footage should be a choice made for this batch
    readonly ZCheck deleteSource = Z.Check(L.T("Delete source clips after converting"));
    readonly ZCheck folderIcons = Z.Check(L.T("Show a frame of each clip as its folder icon"));
    readonly ZCheck skipConverted = Z.Check(L.T("Skip already converted ZRAW clips"));
    readonly MetroProgressBar overall = Z.Progress();
    readonly Label status = Z.Text1("", Z.Dim, 520, 22);
    readonly Label eta = Z.Text1("", Z.Text, 100, Z.RowHeight);   // queue time left, right-aligned on the run row
    readonly Queue<KeyValuePair<DateTime, int>> speed = new Queue<KeyValuePair<DateTime, int>>();   // (time, frames done), last 20 s

    readonly ToolTip toolTip = new ToolTip { AutoPopDelay = 32767 };   // the longest Windows allows (about 33 s); the default 5 s is too short to read long tooltips
    Process proc;
    Queue<List<string>> batches;
    Clip current;
    bool stopping, paused;
    DateTime pausedAt;
    TimeSpan pausedFor;   // left out of the run's time, so the saved speed is the real one
    [DllImport("ntdll.dll")] static extern int NtSuspendProcess(IntPtr process);   // freezes every thread of the engine; it keeps its memory
    [DllImport("ntdll.dll")] static extern int NtResumeProcess(IntPtr process);
    int totalFrames;
    List<Clip> run = new List<Clip>();   // clips of the current or last conversion
    DateTime startedAt;

    public ConvertPage()
    {
        BackColor = Z.Panel;
        Font = Z.UiFont;
        thumbs = new ThumbView(grid, 4, 1, 3) { Remove = RemoveSelected };
        thumbs.Open = p => PreviewForm.Open(p, FindForm(), Write);

        Z.Hint(grid, thumbs.EmptyText);
        grid.Columns.Add("clip", L.T("Clip"));
        grid.Columns.Add("type", L.T("Type"));
        grid.Columns.Add("frames", L.T("Frames"));
        grid.Columns.Add("length", L.T("Length"));
        grid.Columns.Add("status", L.T("Status"));
        grid.Columns.Add("progress", L.T("Progress"));
        grid.Columns[0].AutoSizeMode = DataGridViewAutoSizeColumnMode.Fill;
        grid.Columns[1].Width = 140; grid.Columns[2].Width = 70; grid.Columns[3].Width = 70; grid.Columns[4].Width = 220; grid.Columns[5].Width = 80;
        foreach (DataGridViewColumn col in grid.Columns) col.SortMode = DataGridViewColumnSortMode.NotSortable;   // no sort glyph gap, headers sit over their values
        foreach (int i in new[] { 2, 3, 5 }) grid.Columns[i].DefaultCellStyle.Alignment = grid.Columns[i].HeaderCell.Style.Alignment = DataGridViewContentAlignment.MiddleRight;

        // Left: toolbar + clip list or thumbnails
        var toolbar = new Panel { Dock = DockStyle.Top, Height = Z.RowHeight + 10, BackColor = Z.Panel };
        toolbar.Controls.Add(Z.Bar(DockStyle.Left, add, addFolder, remove, clear));
        toolbar.Controls.Add(thumbs.Switcher("view"));
        var listArea = new Panel { Dock = DockStyle.Fill, BackColor = Z.Panel, Padding = new Padding(0, 0, 10, 0) };
        var gridFrame = Z.Section(DockStyle.Fill);
        gridFrame.Padding = new Padding(1);
        gridFrame.Controls.Add(grid);
        gridFrame.Controls.Add(thumbs);
        listArea.Controls.Add(gridFrame);
        listArea.Controls.Add(toolbar);

        // Right: options, every control on the same left edge and OW wide
        var opts = Z.Section(DockStyle.Right);
        opts.Padding = new Padding(16, 8, 16, 4);
        opts.Width = OW + 34;
        var flow = new FlowLayoutPanel { Dock = DockStyle.Fill, FlowDirection = FlowDirection.TopDown, WrapContents = false, BackColor = Z.Panel };
        flow.HorizontalScroll.Maximum = 0;   // vertical scrolling only, on windows too short for the column
        flow.HorizontalScroll.Visible = false;
        flow.AutoScroll = true;
        // A scrollbar appears when the window is too short for the column; widen the column by its
        // width so the bar doesn't cover the right edge of the controls.
        // Only once the page is on screen: during start-up layout the page is briefly tiny, so the
        // column would flip between wide and narrow, and repaint requests of an invisible page are
        // dropped, which leaves a sliver of the slider's red bar outside the column.
        EventHandler fit = (s, e) =>
        {
            if (!Visible || Width < 400) return;
            int want = OW + 34 + (flow.VerticalScroll.Visible ? SystemInformation.VerticalScrollBarWidth : 0);
            if (opts.Width == want) return;
            opts.Width = want;
            Invalidate(true);   // repaint the strip the column gives back
        };
        flow.Layout += (s, e) => fit(s, e);
        flow.ClientSizeChanged += fit;
        VisibleChanged += fit;
        output.Width = ratio.Width = level.Width = OW;
        format.Width = OW - LW;   // beside the FORMAT heading
        toolTip.SetToolTip(gpu, L.T("Use the GPU for lossy compression. Tick the GPUs to use below."));
        format.Margin = output.Margin = new Padding(0, 0, 0, 4);
        rateMode.Width = OW - LW;
        // The mode choice shares the COMPRESSION heading's row, so it adds no height to the column
        var compressionHead = Head(L.T("COMPRESSION"), rateMode, false);
        // Fixed quality names a ProRes or H.265 level; a small "equivalent" after the name marks the
        // level as a comparison, not the format being written
        var ratioRow = new Panel { Size = new Size(OW, ratioText.PreferredSize.Height), BackColor = Z.Panel, Margin = new Padding(0) };
        ratioText.Location = new Point(0, 0);
        ratioRow.Controls.Add(ratioText);
        ratioRow.Controls.Add(equivText);
        EventHandler placeEquiv = (s, e) => equivText.Location = new Point(ratioText.Right + 4, ratioText.Bottom - equivText.Height - 5);
        ratioText.SizeChanged += placeEquiv;
        equivText.SizeChanged += placeEquiv;
        ratio.Margin = level.Margin = new Padding(0, 2, 0, 7);
        level.Visible = false;
        ratioText.Margin = new Padding(0);
        gpu.Size = new Size(50, 22);
        foreach (var c in new Control[] { scale, denoise, colorMatrix, threads }) c.Width = OW - LW;
        toolTip.SetToolTip(denoise, L.T("Removes noise from the raw data on the GPU before the DNGs are written. Each frame is merged with\n" +
            "its neighbors (two on each side for Light, three for Strong), using the camera's measured noise at the clip's ISO.\n" +
            "Where something moves, the frame keeps its own pixels, so there is no ghosting. Light leaves about 45% of the\n" +
            "noise and does not soften edges. In fixed-quality mode the files also get 20 to 35% smaller.\n" +
            "Conversion runs at about half the speed, and the DNGs no longer contain the camera's original data.\n" +
            "Only whole frames are aligned, so it works best on tripod and gimbal shots."));
        toolTip.SetToolTip(colorMatrix, L.T("Color matrix written into the DNGs.\nZRAW Studio: matrices calibrated by ZRAW Studio for accurate color.\nZCam-DNG (zraw-parser): Z CAM's factory matrices, the same ones the camera firmware's DNG writer and zraw-parser use.\nCamera built-in matrix: the matrices the camera stores in every RAW frame, for each shot's color temperature.\nVideoSuite: Z CAM's ZRAW VideoSuite matrix (the one Z CAM uses for ZLog2 footage)."));
        var outRow = Z.Bar(DockStyle.None, browse, openOut);
        outRow.Size = new Size(OW - LW, browse.Height + browse.Margin.Vertical);   // explicit height; without it the row comes out 100 px tall
        toolTip.SetToolTip(errorText, L.T("Typical error per pixel against the sensor's own noise, measured on Z CAM E2 footage.\nBelow 100% it stays inside the grain the camera records anyway."));
        toolTip.SetToolTip(proresText, L.T("The lowest ProRes level this ratio still matches in the developed log image (UHD 24p, ffmpeg's prores_ks).\n" +
            "Same quality, ProRes bitrate vs raw:\n" +
            "    4444 XQ   1,654 Mb/s   2.6:1, about 910 Mb/s (45% less)\n" +
            "    4444      1,109 Mb/s   3.1:1, about 775 Mb/s (30% less)\n" +
            "    422 HQ      742 Mb/s   4.2:1, about 565 Mb/s (24% less)\n" +
            "    422         495 Mb/s   4.8:1, about 500 Mb/s (about the same)\n" +
            "    LT          347 Mb/s   5.7:1, about 420 Mb/s (20% more)\n" +
            "    Proxy       140 Mb/s   10:1, about 230 Mb/s (65% more)\n" +
            "4:1 gives 422 HQ quality at about three quarters of its bitrate. Past about 6:1 raw needs more bits than\n" +
            "ProRes LT or Proxy for the same look, so treat those ratios as proxy or offline settings, not masters."));
        toolTip.SetToolTip(gradeText, L.T("From about 6:1 up, this shows the E2's own H.265 setting (UHD 24p, 10-bit 4:2:0) that looks about the same\nafter developing to log, matched by eye and by error in the developed image:\nHigh 200 Mb/s is about 7:1, Medium 130 Mb/s about 8 to 9:1, Low 60 Mb/s about 12:1.\nLossy raw needs 1.5 to 3 times the H.265 bitrate for the same look; it keeps full color resolution."));
        flow.Controls.AddRange(new Control[] {
            Head(L.T("FORMAT"), format, true),
            compressionHead, ratioRow, ratio, level, Field(L.T("Size"), sizeText), Field(L.T("Error"), errorText), Field(L.T("Quality"), gradeText), Field(L.T("Matches"), proresText),
            Head(L.T("PROCESSING"), gpu, false), gpuList, Field(L.T("Resolution"), scale), Field(L.T("Denoise"), denoise), Field(L.T("Color"), colorMatrix), Field(L.T("CPU threads"), threads),
            Head(L.T("FOLDER"), outRow, false), output, skipConverted, deleteSource, folderIcons });
        skipConverted.Size = deleteSource.Size = folderIcons.Size = new Size(OW, 21);
        skipConverted.Margin = new Padding(0, 2, 0, 0);
        deleteSource.Margin = folderIcons.Margin = new Padding(0, 1, 0, 0);
        toolTip.SetToolTip(skipConverted, L.T("ZRAW Studio skips clips that already have a conversion in the output folder, without asking.\nUntick this to be asked whether to overwrite them instead."));
        toolTip.SetToolTip(folderIcons, L.T("Each clip's DNG folder shows a frame of the clip in Explorer instead of the plain folder icon,\nso a folder full of conversions is easy to browse.\nTwo hidden files in the folder do it (an .ico file and desktop.ini); nothing else changes."));
        toolTip.SetToolTip(deleteSource, L.T("Each source clip goes to the Recycle Bin once its conversion has finished with no errors\nand the new files check out on disk. Clips on drives without a Recycle Bin are kept."));
        opts.Controls.Add(flow);

        // Bottom: run controls, progress, log
        var bottom = new Panel { Dock = DockStyle.Bottom, Height = 180, BackColor = Z.Panel, Padding = new Padding(0, 10, 0, 0) };
        start.Width = 120;
        pause.Width = Math.Max(pause.Width, TextRenderer.MeasureText(L.T("Resume"), Z.UiFont).Width + 36);   // wide enough for both of its texts
        var runRow = new Panel { Dock = DockStyle.Top, Height = Z.RowHeight + 10, BackColor = Z.Panel };
        eta.Dock = DockStyle.Fill;
        eta.TextAlign = ContentAlignment.MiddleRight;
        eta.Padding = new Padding(0, 0, 0, 10);
        runRow.Controls.Add(eta);
        runRow.Controls.Add(Z.Bar(DockStyle.Left, start, pause, stop, status));
        toolTip.SetToolTip(pause, L.T("Freezes the conversion so the CPU and GPU are free for something else.\nResume continues from the same frame."));
        status.Margin = new Padding(8, (Z.RowHeight - status.Height) / 2, 0, 0);
        gpuList.Controls.Add(Z.Text1(L.T("Checking..."), Z.Dim, OW - LW, 22));
        toolTip.SetToolTip(gpuList, L.T("Tick every GPU that should help. A much slower GPU next to a fast one can slow the whole run down."));
        var barRow = new Panel { Dock = DockStyle.Top, Height = 18, BackColor = Z.Panel, Padding = new Padding(0, 0, 0, 8) };
        overall.Dock = DockStyle.Fill;
        barRow.Controls.Add(overall);
        var logFrame = Z.Section(DockStyle.Fill);
        logFrame.Padding = new Padding(8);
        log.BackColor = Z.Ink;
        logFrame.BackColor = Z.Ink;
        logFrame.Controls.Add(log);
        bottom.Controls.Add(logFrame);
        bottom.Controls.Add(barRow);
        bottom.Controls.Add(runRow);

        Controls.Add(listArea);
        Controls.Add(opts);
        Controls.Add(bottom);

        // Settings
        format.SelectedIndex = Clamp(Int("format", 0), 0, 3);
        ratio.Value = 30;   // 3:1 on every launch; a run uses whatever the slider shows
        level.Value = 2;    // fixed quality starts at ProRes 422 HQ on every launch, like the ratio
        rateMode.SelectedIndex = Settings.Get("rateMode", "0") == "1" ? 1 : 0;
        toolTip.SetToolTip(rateMode, L.T("Fixed ratio: every frame the same size.\n" +
            "Fixed quality: every shot gets at least the chosen quality; the size is set per shot from its ISO and content.\n" +
            "Clean, bright, low-ISO shots get more bits, dark or grainy ones fewer. On average it is about the same size\n" +
            "as the fixed ratio that matches the level, but no shot falls below it, shadows included. The ISO is read from each clip."));
        toolTip.SetToolTip(level, L.T("Each level is the quality a typical shot gets at the fixed ratio that matched that ProRes or\nE2 H.265 setting by eye. Fixed quality gives every shot that quality."));
        scale.SelectedIndex = Clamp(Int("scale", 0), 0, 2);
        denoise.SelectedIndex = Clamp(Int("denoise", 0), 0, 2);
        colorMatrix.SelectedIndex = Clamp(Int("color", 0), 0, 3);
        threads.Value = Clamp(Int("threads", Environment.ProcessorCount), 1, Environment.ProcessorCount);
        gpu.Switched = Settings.Get("gpu", "1") == "1";
        output.Text = Settings.Get("output", "");

        add.Click += (s, e) =>
        {
            using (var d = new OpenFileDialog { RestoreDirectory = true, Multiselect = true, Filter = L.T("Z CAM clips (*.mov, *.zraw)|*.mov;*.zraw|All files|*.*"), Title = L.T("Add ZRAW clips") })
                if (d.ShowDialog(this) == DialogResult.OK) Add(d.FileNames);
        };
        addFolder.Click += (s, e) =>
        {
            string dir = FolderPicker.Pick(this, L.T("Add a folder of ZRAW clips (subfolders included)"), Settings.Get("addFolder", ""));
            if (dir == null) return;
            Settings.Set("addFolder", dir);
            Add(new[] { dir });
        };
        remove.Click += (s, e) => RemoveSelected();
        clear.Click += (s, e) => { clips.Clear(); grid.Rows.Clear(); thumbs.Refill(); Summary(); };
        browse.Click += (s, e) =>
        {
            string dir = FolderPicker.Pick(this, L.T("Choose the output folder"), output.Text);
            if (dir != null) output.Text = dir;
        };
        openOut.Click += (s, e) => { if (Directory.Exists(output.Text)) Process.Start("explorer.exe", "\"" + output.Text + "\""); };
        format.SelectedIndexChanged += (s, e) => { Settings.Set("format", format.SelectedIndex.ToString()); RatioChanged(); };
        ratio.Scroll += (s) => RatioChanged();
        level.Scroll += (s) => RatioChanged();
        rateMode.SelectedIndexChanged += (s, e) =>
        {
            if (rateMode.SelectedIndex == 1 && Settings.Get("qualityNoAsk", "0") != "1")
                using (var f = new ConfirmForm(L.T("Fixed quality and auto ISO"),
                    L.T("Fixed quality reads each clip's ISO and sets the size from the noise at that ISO.\n\n" +
                    "The camera records the ISO once per clip. If a clip was shot with auto ISO and the ISO changed " +
                    "during the shot, the size can be wrong for part of it. Use a fixed ratio for auto ISO clips."),
                    L.T("Use fixed quality")))
                {
                    if (f.ShowDialog(FindForm()) != DialogResult.OK) { rateMode.SelectedIndex = 0; return; }
                    if (f.DontShowAgain) Settings.Set("qualityNoAsk", "1");
                }
            Settings.Set("rateMode", rateMode.SelectedIndex.ToString());
            RatioChanged();
        };
        scale.SelectedIndexChanged += (s, e) => Settings.Set("scale", scale.SelectedIndex.ToString());
        denoise.SelectedIndexChanged += (s, e) => { Settings.Set("denoise", denoise.SelectedIndex.ToString()); Summary(); };
        colorMatrix.SelectedIndexChanged += (s, e) => Settings.Set("color", colorMatrix.SelectedIndex.ToString());
        threads.ValueChanged += s => Settings.Set("threads", threads.Value.ToString());
        gpu.SwitchedChanged += (s) => { Settings.Set("gpu", gpu.Switched ? "1" : "0"); Running(proc != null); Summary(); };
        output.TextChanged += (s, e) => Settings.Set("output", output.Text);
        folderIcons.Checked = Settings.Get("folderIcons", "1") == "1";
        folderIcons.CheckedChanged += s => Settings.Set("folderIcons", folderIcons.Checked ? "1" : "0");
        skipConverted.Checked = Settings.Get("skipConverted", "1") == "1";
        skipConverted.CheckedChanged += s => Settings.Set("skipConverted", skipConverted.Checked ? "1" : "0");
        deleteSource.CheckedChanged += (s) =>
        {
            if (!deleteSource.Checked || Settings.Get("deleteNoAsk", "0") == "1") return;
            using (var f = new ConfirmForm(L.T("Delete source clips?"),
                L.T("After each clip converts with no errors and its new files check out on disk, ZRAW Studio moves the " +
                "original clip to the Recycle Bin, where you can restore it until the bin is emptied.\n\n" +
                "Clips on drives without a Recycle Bin, such as SD cards in a card reader, stay where they are. Windows asks " +
                "first if a clip is too big for the Recycle Bin.\n\n" +
                "Lossy CinemaDNG is not an exact copy of the original. Delete the originals only if you are sure you " +
                "will not need them again."),
                L.T("Delete after converting")))
            {
                if (f.ShowDialog(FindForm()) != DialogResult.OK) deleteSource.Checked = false;
                else if (f.DontShowAgain) { Settings.Set("deleteNoAsk", "1"); Settings.Save(); }
            }
        };
        start.Click += (s, e) => Start();
        stop.Click += (s, e) => Stop();
        pause.Click += (s, e) => Pause();

        RatioChanged();
        Running(false);
        // Posted so it lands after the window's final layout; written earlier, the log opens scrolled down a line
        Load += (s, e) => BeginInvoke(new Action(() => Write(L.T("Ready. Lossy CinemaDNG at 3.0:1 is the default; the slider sets the compression. Double-click a clip to preview it."))));
        CheckGpu();
    }

    // A section heading on the left of the section's first row, so headings take no rows of their own
    // (the column has to fit the window at its opening size without scrolling)
    static Control Head(string text, Control first, bool top)
    {
        var p = Field(text, first);
        p.Controls[0].Font = Z.HeadFont;
        p.Margin = new Padding(0, top ? 0 : 6, 0, 4);
        return p;
    }

    // "Label   control" on one line, labels in a column LW wide so the controls line up
    static Control Field(string label, Control c)
    {
        var p = new Panel { Size = new Size(OW, c.Height), BackColor = Z.Panel, Margin = new Padding(0, 0, 0, 3) };
        var l = Z.Text1(label, Z.Dim, LW, c.Height);
        c.Location = new Point(LW, 0);
        c.Margin = new Padding(0);
        p.Controls.Add(l);
        p.Controls.Add(c);
        return p;
    }

    static int Int(string key, int fallback) { int v; return int.TryParse(Settings.Get(key, ""), out v) ? v : fallback; }
    static int Clamp(int v, int lo, int hi) { return Math.Max(lo, Math.Min(hi, v)); }
    double Ratio { get { return ratio.Value / 10.0; } }
    string RatioString() { return Ratio.ToString("0.0", L.Culture) + ":1"; }
    public void Write(string s) { Z.Write(log, s); }

    // Error relative to sensor noise, measured on Z CAM E2 UHD footage
    static readonly double[,] Quality = { { 1.5, 0.03 }, { 2, 0.063 }, { 2.5, 0.101 }, { 3, 0.157 }, { 4, 0.325 }, { 5, 0.552 }, { 7, 0.865 }, { 10, 1.3 } };

    static double ErrorAt(double r)
    {
        for (int i = 1; i < Quality.GetLength(0); i++)
            if (r <= Quality[i, 0])
            {
                double t = (r - Quality[i - 1, 0]) / (Quality[i, 0] - Quality[i - 1, 0]);
                return Quality[i - 1, 1] + t * (Quality[i, 1] - Quality[i - 1, 1]);
            }
        return Quality[Quality.GetLength(0) - 1, 1];
    }

    // Past the noise grades: the E2's own H.265 setting (UHD 24p, 10-bit 4:2:0) that looks about the
    // same, matched by eye on Z CAM E2 footage: High ~7:1, Medium ~8 to 9:1, Low ~12:1
    static string H265Grade(double r)
    {
        if (r < 6.95) return L.T("Better than E2 H.265 High");
        if (r >= 9.45 && r < 11.45) return L.T("E2 H.265, 60 to 130 Mb/s");
        return L.F("Like E2 H.265 at {0} Mb/s", r < 7.95 ? 200 : r < 9.45 ? 130 : 60);
    }

    // The lowest ProRes level a ratio still matches, matched by eye on Z CAM E2 footage:
    // 4444 XQ 2.6:1, 4444 3.1:1, 422 HQ 4.2:1, 422 4.8:1, LT 5.7:1, Proxy 10:1
    static string ProResGrade(double r)
    {
        return r <= 10.05 ? L.F("At least {0}", r <= 2.65 ? "ProRes 4444 XQ" : r <= 3.15 ? "ProRes 4444" : r <= 4.25 ? "ProRes 422 HQ" :
                                r <= 4.85 ? "ProRes 422" : r <= 5.75 ? "ProRes 422 LT" : "ProRes Proxy") : L.T("Below ProRes Proxy");
    }

    // Fixed quality levels, measured over 36 E2 clips: each target (error x sensor noise) is the
    // typical clip's frame-wide quality at the fixed ratio that matched the level by eye. The engine
    // holds it in every brightness band, shadows included. The average and range are the ratios the
    // clips got in fixed-quality mode.
    static readonly string[] LevelShort = { "ProRes 4444 XQ", "ProRes 4444", "ProRes 422 HQ", "ProRes 422", "ProRes 422 LT", "H.265 High", "H.265 Medium", "H.265 Low" };
    static readonly string[] LevelLong = { "ProRes 4444 XQ", "ProRes 4444", "ProRes 422 HQ", "ProRes 422", "ProRes 422 LT",
                                           L.T("E2 H.265 High, 200 Mb/s"), L.T("E2 H.265 Medium, 130 Mb/s"), L.T("E2 H.265 Low, 60 Mb/s") };
    static readonly double[] LevelTarget = { 0.13, 0.20, 0.42, 0.58, 0.78, 0.97, 1.13, 1.55 };
    static readonly int[] LevelMaxRatio = { 8, 8, 8, 8, 8, 20, 20, 20 };   // H.265 levels may go past 8:1
    static readonly double[] LevelAverage = { 2.55, 2.98, 3.99, 4.58, 5.33, 6.39, 7.75, 10.76 };
    static readonly double[] LevelLow = { 2.0, 2.1, 2.7, 3.0, 3.4, 3.8, 4.1, 4.8 }, LevelHigh = { 3.2, 3.8, 5.1, 5.9, 6.9, 8.1, 10.8, 14.4 };
    bool FixedQuality { get { return format.SelectedIndex == 0 && rateMode.SelectedIndex == 1; } }
    double AverageRatio { get { return FixedQuality ? LevelAverage[level.Value] : Ratio; } }   // for size estimates

    void RatioChanged()
    {
        int f = format.SelectedIndex;
        bool lossy = f == 0;
        ratio.Enabled = level.Enabled = rateMode.Enabled = lossy && proc == null;
        // One layout for the swap: shown before the other is hidden, both sliders overflow the column
        // for a moment and its scroll bar flashes
        var column = ratio.Parent;
        if (column != null) column.SuspendLayout();
        ratio.Visible = !FixedQuality;
        level.Visible = equivText.Visible = FixedQuality;
        if (column != null) column.ResumeLayout();
        ratioText.ForeColor = lossy ? Color.White : Z.Dim;
        if (FixedQuality)
        {
            int i = level.Value;
            ratioText.Text = LevelShort[i];
            sizeText.Text = L.F("{0:0.0}:1 average, {1:0.0} MB/frame", LevelAverage[i], 3840 * 2160 * 1.5 / LevelAverage[i] / 1e6);
            errorText.Text = L.F("At most ±{0:0}% of noise", LevelTarget[i] * 100);
            gradeText.Text = L.F(i == 2 ? "{0:0.0} to {1:0.0}:1 per shot (default)" : "{0:0.0} to {1:0.0}:1 per shot", LevelLow[i], LevelHigh[i]);
            proresText.Text = i < 5 ? L.F("At least {0}", LevelLong[i]) : L.F("Like E2 H.265 at {0} Mb/s", new[] { 200, 130, 60 }[i - 5]);
        }
        else if (lossy)
        {
            double e = ErrorAt(Ratio);
            ratioText.Text = Ratio.ToString("0.0", L.Culture) + " : 1";
            sizeText.Text = L.F("{0:0.0} MB per UHD frame", 3840 * 2160 * 1.5 / Ratio / 1e6);
            errorText.Text = L.F(Ratio > 10 ? "Over ±{0:0}% of sensor noise" : "±{0:0}% of sensor noise", e * 100);   // measured up to 10:1
            proresText.Text = ProResGrade(Ratio);
            string grade = e <= 0.12 ? L.T("Near lossless") : e <= 0.2 ? L.T("Visually lossless") : e <= 0.4 ? L.T("Very high") : e <= 0.65 ? L.T("High, light grading") : H265Grade(Ratio);
            gradeText.Text = Math.Abs(Ratio - 3) < 0.05 ? L.F("{0} (default)", grade) : grade;
        }
        else
        {
            ratioText.Text = L.T(f == 1 ? "Lossless" : "Uncompressed");
            sizeText.Text = L.T(f == 1 ? "About 6.9 MB per UHD frame" : f == 2 ? "16.6 MB per UHD frame" : "12.4 MB per UHD frame");
            errorText.Text = L.T("None, every pixel exact");
            proresText.Text = L.T("Beyond ProRes 4444 XQ");
            gradeText.Text = L.T(f == 1 ? "Lossless" : f == 2 ? "zraw-parser's padded layout" : "Packed 12-bit, no padding");
        }
        Summary();
    }

    // zrawdng gpuinfo: "@gpu <number> <name> <why not usable>" per GPU, fastest first
    void CheckGpu()
    {
        Task.Run(() =>
        {
            var found = new List<string[]>();
            string error = null;
            try
            {
                var p = Process.Start(new ProcessStartInfo(Embedded.Engine(), "gpuinfo") { UseShellExecute = false, RedirectStandardError = true, CreateNoWindow = true });
                foreach (var line in p.StandardError.ReadToEnd().Split('\n'))
                {
                    var f = line.TrimEnd('\r').Split('\t');
                    if (f.Length >= 4 && f[0] == "@gpu" && f[1] != "-1") found.Add(new[] { f[1], f[2], f[3] });
                }
                p.WaitForExit();
            }
            catch (Exception ex) { error = L.F("Engine error: {0}", ex.Message); }
            BeginInvoke(new Action(() => ShowGpus(found, error)));
        });
    }

    void ShowGpus(List<string[]> found, string error)
    {
        gpuList.Controls.Clear();
        gpuBoxes.Clear();
        if (found.Count == 0)
        {
            var l = Z.Text1(error ?? L.T("None found, CPU only"), Z.Dim, OW - LW, 22);
            toolTip.SetToolTip(l, l.Text);
            gpuList.Controls.Add(l);
            return;
        }
        // Remembered by name, so the choice survives a GPU being added; the first time, the fastest one
        var chosen = Settings.Get("gpus", "").Split('|');
        foreach (var g in found)
        {
            var b = Z.Check(g[1]);
            b.Size = new Size(OW - LW, 24);
            b.Margin = new Padding(0, 0, 0, 4);
            b.Tag = g[2].Length == 0 ? g[0] : null;   // the engine's number, null when unusable
            b.Checked = b.Tag != null && (chosen.Contains(g[1]) || chosen[0].Length == 0 && g == found[0]);
            toolTip.SetToolTip(b, b.Tag != null ? g[1] : L.F("{0} cannot be used: {1}", g[1], g[2]));
            b.CheckedChanged += s => { var on = gpuBoxes.Where(x => x.Checked).Select(x => x.Text).ToList(); Settings.Set("gpus", on.Count > 0 ? string.Join("|", on) : "-"); };
            gpuBoxes.Add(b);
            gpuList.Controls.Add(b);
        }
        Running(proc != null);
        Summary();
    }

    static string Hms(double seconds) { return TimeSpan.FromSeconds(Math.Max(0, seconds)).ToString(@"h\:mm\:ss"); }
    string FpsKey() { return "fps" + format.SelectedIndex + (FixedQuality ? "q" : "") + (denoise.SelectedIndex > 0 ? "d" + denoise.SelectedIndex : "") + (gpu.Switched ? "g" : "c"); }   // the GPU list may still be loading
    double LastFps() { double v; return double.TryParse(Settings.Get(FpsKey(), ""), NumberStyles.Float, CultureInfo.InvariantCulture, out v) ? v : 0; }
    static long FileSize(string path) { try { return new FileInfo(path).Length; } catch { return 0; } }

    void RemoveSelected()
    {
        if (proc != null) return;
        foreach (var c in clips.Where(c => c.Row.Selected).ToList())
        {
            clips.Remove(c);
            grid.Rows.Remove(c.Row);
        }
        thumbs.Refill();
        Summary();
    }

    // Adds the ZRAW clips among the files and folders given, subfolders included: true RAW (to DNG)
    // and HEVC (to an H.265 .mov). The Tagger tab lists every clip. Hidden and system folders are
    // left out, and so are DNG folders (their .mov is the engine's audio sidecar, not a clip).
    public void Add(IEnumerable<string> paths)
    {
        if (proc != null) { Write(L.T("Wait for the current conversion to finish before adding clips.")); return; }
        var input = paths.ToList();
        status.Text = L.T("Looking for clips...");
        Task.Run(() =>
        {
            var files = new List<string>();
            foreach (var p in input)
                if (Directory.Exists(p)) Collect(p, files);
                else if (File.Exists(p)) files.Add(p);
            var infos = files.AsParallel().AsOrdered().Select(f =>
            {
                MovInfo i;
                try { i = MovInfo.Read(f); } catch { return new MovInfo(); }
                try { if (i.TrueRaw) i.Bits = ZrawCore.DataBits(f); else if (i.Hevc) i.Bits = 10; } catch { }   // decodes one frame per RAW clip
                return i;
            }).ToList();
            BeginInvoke(new Action(() => AddClips(files, infos)));
        });
    }

    static void Collect(string dir, List<string> into)
    {
        string[] files, dirs;
        try { files = Directory.GetFiles(dir); dirs = Directory.GetDirectories(dir); } catch { return; }
        if (files.Any(f => f.EndsWith(".dng", StringComparison.OrdinalIgnoreCase))) return;
        into.AddRange(files.Where(f => f.EndsWith(".mov", StringComparison.OrdinalIgnoreCase) || f.EndsWith(".zraw", StringComparison.OrdinalIgnoreCase)).OrderBy(f => f, StringComparer.OrdinalIgnoreCase));
        foreach (var d in dirs.OrderBy(d => d, StringComparer.OrdinalIgnoreCase))
        {
            try { if ((File.GetAttributes(d) & (FileAttributes.Hidden | FileAttributes.System)) != 0) continue; } catch { continue; }
            Collect(d, into);
        }
    }

    // What the clip holds, for the Type column and the line under each thumbnail. A 14-bit file whose
    // pixels only use 12 bits (the E2's 14-bit mode before it was unlocked) shows as 12-bit.
    static string Kind(MovInfo i)
    {
        if (i.Hevc) return L.T("ZRAW H.265 10-bit");
        return i.Bits > 0 ? L.F("ZRAW Bayer {0}-bit", i.Bits) : "ZRAW Bayer";
    }

    // The hover card for a clip, in the list and on its thumbnail: its path, then how it was shot, from
    // the clip's metadata (lens values only with an electronic lens; anything not recorded is left out).
    static string ShotInfo(string path, MovInfo i)
    {
        var m = i.Meta;
        var lines = new List<string>();
        Func<string, string> get = k => { string v; return m.TryGetValue(k, out v) && v.Trim().Length > 0 ? v.Trim() : null; };
        Func<string, double> num = v => { double d; return double.TryParse(new string(v.TakeWhile(ch => char.IsDigit(ch) || ch == '.').ToArray()), NumberStyles.Float, CultureInfo.InvariantCulture, out d) ? d : 0; };
        Action<string, string> add = (label, value) => { if (value != null) lines.Add(L.T(label) + ": " + value); };
        string maker = get("CameraMaker"), model = get("CameraModel");   // "Z CAM" and "E2", "E2-F6", "E2-S6G"
        add("Camera", model != null ? (maker != null ? maker + " " : "") + model : null);
        add("Lens", get("lensType"));
        string focal = get("lensFocalLength");
        add("Focal length", focal != null && num(focal) > 0 ? L.F("{0} mm", num(focal)) : null);
        string f = get("aperture");
        add("Aperture", f != null ? "f/" + f.TrimStart('F', 'f') : null);
        string focus = get("focusDistance");
        if (focus != null && num(focus) > 0) add("Focus distance", num(focus) >= 327675 ? L.T("infinity") : L.F("{0:0.00} m", num(focus) / 1000));
        add("ISO", get("iso"));
        add("Shutter", get("shutterSpeed") ?? get("shutterAngle"));
        string nd = get("nd");   // "clear" on the F6 and S6G; the E2 doesn't record it
        add("ND", nd != null ? char.ToUpper(nd[0]) + nd.Substring(1) : null);
        add("EV", get("ev"));     // exposure compensation, F6 and S6G
        string fps = get("projectFps"), shot = get("shootingRate");
        if (fps != null)
            add("Frame rate", L.F("{0} fps", fps) + (shot != null && shot != fps ? ", " + L.F("shot at {0} fps", shot) : ""));
        string kelvin = get("whiteBalanceKelvin"), tint = get("whiteBalanceTint");
        add("White balance", kelvin != null ? kelvin + (tint != null ? ", " + L.F("tint {0}", tint) : "") : null);
        add("Camera LUT", get("lut"));
        return lines.Count > 0 ? path + "\n\n" + string.Join("\n", lines) : path;
    }

    void AddClips(List<string> files, List<MovInfo> infos)
    {
        var known = new HashSet<string>(clips.Select(c => c.Path), StringComparer.OrdinalIgnoreCase);
        int added = 0, other = 0, unknown = 0, hevc = 0;
        for (int i = 0; i < files.Count; i++)
        {
            if (infos[i].Hevc && !infos[i].Convertible) { unknown++; continue; }
            if (!infos[i].Convertible) { other++; continue; }
            if (!known.Add(files[i])) continue;
            var info = infos[i];
            var c = new Clip { Path = files[i], Info = info, T = new Thumb { Path = files[i], Raw = info.TrueRaw || info.Hevc } };
            string length = info.Fps > 0 ? TimeSpan.FromSeconds(info.Frames / info.Fps).ToString(@"m\:ss") : "";
            c.Row = grid.Rows[grid.Rows.Add(Path.GetFileName(c.Path), Kind(info), info.Frames.ToString(), length, L.T("Ready"), "")];
            if (info.Hevc) hevc++;
            string card = ShotInfo(c.Path, info);   // on every cell, so it shows wherever the row is hovered
            foreach (DataGridViewCell cell in c.Row.Cells) cell.ToolTipText = card;
            c.Row.Tag = c.T;
            clips.Add(c);
            added++;
        }
        grid.ClearSelection();
        thumbs.Refill();
        if (other > 0) Write(L.F("Left out {0} {0:clip|clips} that {0:is|are} not ZRAW (H.265 and others). The Tagger tab shows every clip.", other));
        if (unknown > 0) Write(L.F("Left out {0} HEVC ZRAW {0:clip|clips} of a variant this version does not recognize.", unknown));
        if (added == 0) { if (other + unknown == 0) Write(L.T("No new ZRAW clips found.")); Summary(); return; }
        if (output.Text.Trim().Length == 0) output.Text = Path.Combine(Path.GetPathRoot(clips[clips.Count - added].Path), "zraw-decoded");   // default: a zraw-decoded folder at the root of the clips' drive
        Write(hevc > 0 ? L.F("Added {0} ZRAW {0:clip|clips}: {1} RAW (converted to DNG) and {2} HEVC (copied to an H.265 .mov).", added, added - hevc, hevc)
                       : L.F("Added {0} ZRAW {0:clip|clips}.", added));
        Summary();
    }

    void Summary()
    {
        if (proc != null) return;
        var todo = clips.Where(c => c.Info.Convertible).ToList();
        if (todo.Count == 0) { status.Text = L.T("Drop ZRAW clips or folders anywhere in this window."); eta.Text = ""; return; }
        int frames = todo.Sum(c => c.Info.Frames), hevc = todo.Count(c => c.Info.Hevc);
        double perFrame = format.SelectedIndex == 0 ? 3840 * 2160 * 1.5 / AverageRatio : format.SelectedIndex == 1 ? 6.9e6 : format.SelectedIndex == 2 ? 16.6e6 : 12.4e6;
        double bytes = todo.Where(c => c.Info.TrueRaw).Sum(c => c.Info.Frames * perFrame) + todo.Where(c => c.Info.Hevc).Sum(c => (double)FileSize(c.Path));   // HEVC: about the clip's own size
        status.Text = L.F("{0} {0:clip|clips}{1}, {2:N0} {2:frame|frames}, about {3:0.0} GB of output.",
            todo.Count, hevc > 0 ? " (" + (todo.Count - hevc) + " RAW, " + hevc + " HEVC)" : "", frames, bytes / 1e9);
        double fps = LastFps();
        eta.Text = fps > 0 ? L.F("About {0} at your last speed", Hms(frames / fps)) : "";
        toolTip.SetToolTip(eta, fps > 0 ? L.F("Your last full run with these settings averaged {0:0.0} fps", fps) : "");
    }

    void Running(bool on)
    {
        start.Enabled = !on;
        stop.Enabled = pause.Enabled = on;
        if (!on) { paused = false; pause.Text = L.T("Pause"); }
        add.Enabled = addFolder.Enabled = remove.Enabled = clear.Enabled = browse.Enabled = deleteSource.Enabled = folderIcons.Enabled = skipConverted.Enabled = !on;
        format.Enabled = scale.Enabled = denoise.Enabled = colorMatrix.Enabled = threads.Enabled = output.Enabled = gpu.Enabled = !on;
        ratio.Enabled = level.Enabled = rateMode.Enabled = !on && format.SelectedIndex == 0;
        foreach (var b in gpuBoxes) b.Enabled = !on && gpu.Switched && b.Tag != null;
    }

    void Start()
    {
        var todo = clips.Where(c => c.Info.Convertible).ToList();
        if (todo.Count == 0) { Write(L.T("Nothing to convert: add ZRAW clips first.")); return; }
        string outDir = output.Text.Trim();
        if (outDir.Length == 0) { Write(L.T("ERROR: choose an output folder.")); return; }
        // Absolute, so the engine, the existing-conversion check and Open all mean the same folder whatever the current directory is
        try { outDir = Path.GetFullPath(outDir); Directory.CreateDirectory(outDir); } catch (Exception ex) { Write(L.F("ERROR: cannot create {0}: {1}", outDir, ex.Message)); return; }
        string engine;
        try { engine = Embedded.Engine(); } catch (Exception ex) { Write(L.F("ERROR: {0}", ex.Message)); return; }

        // The engine ends each clip with <output>\<clip name>\<clip name>.mov; that must not be the clip itself
        var self = todo.Where(c =>
        {
            string name = Path.GetFileNameWithoutExtension(c.Path);
            return string.Equals(Path.GetFullPath(Path.Combine(outDir, name, name + ".mov")), Path.GetFullPath(c.Path), StringComparison.OrdinalIgnoreCase);
        }).ToList();
        foreach (var c in self)
        {
            Set(c, L.F("Skipped: {0}", L.T("the output would overwrite the clip")), Z.Dim, "");
            Write(L.F("Skipping {0}: converting it into {1} would overwrite the clip with its own audio file. Choose another output folder.", Path.GetFileName(c.Path), outDir));
        }
        todo = todo.Except(self).ToList();
        if (todo.Count == 0) return;

        // Never replace an earlier conversion without asking: the engine overwrites <output>\<clip name>\
        var existing = todo.Where(c =>
        {
            string name = Path.GetFileNameWithoutExtension(c.Path), d = Path.Combine(outDir, name);
            return Directory.Exists(d) && (File.Exists(Path.Combine(d, name + ".mov")) || Directory.EnumerateFiles(d, "*.dng").Any());
        }).ToList();
        // "Skip already converted" skips only complete conversions: every frame plus the .mov the engine writes
        // last. A stopped or failed run leaves less, and that still goes to the dialog.
        if (skipConverted.Checked)
        {
            var complete = existing.Where(c =>
            {
                string name = Path.GetFileNameWithoutExtension(c.Path), d = Path.Combine(outDir, name);
                return File.Exists(Path.Combine(d, name + ".mov")) && (c.Info.Hevc || Directory.EnumerateFiles(d, "*.dng").Count() >= c.Info.Frames);
            }).ToList();
            foreach (var c in complete) Set(c, L.T("Skipped: already converted"), Z.Dim, "");
            if (complete.Count > 0) Write(L.F("Skipping {0} {0:clip|clips} already converted into {1}.", complete.Count, outDir));
            todo = todo.Except(complete).ToList();
            existing = existing.Except(complete).ToList();
            if (todo.Count == 0) { Write(L.F("Nothing to convert: every clip was already converted into {0}.", outDir)); return; }
        }
        if (existing.Count > 0)
        {
            var answer = MessageBox.Show(this,
                L.F("{0} of these clips {0:was|were} already converted into {1}:\n\n{2}\n\nYes: overwrite them.\nNo: skip those clips and convert the rest.\nCancel: do nothing.",
                    existing.Count, outDir, string.Join("\n", existing.Take(8).Select(c => Path.GetFileNameWithoutExtension(c.Path))) + (existing.Count > 8 ? "\n..." : "")),
                L.T("ZRAW Studio: existing conversions"), MessageBoxButtons.YesNoCancel, MessageBoxIcon.Warning, MessageBoxDefaultButton.Button2);
            if (answer == DialogResult.Cancel) return;
            if (answer == DialogResult.No)
            {
                foreach (var c in existing) Set(c, L.T("Skipped: already converted"), Z.Dim, "");
                todo = todo.Except(existing).ToList();
                if (todo.Count == 0) { Write(L.F("Nothing to convert: every clip was already converted into {0}.", outDir)); return; }
            }
        }

        run = todo;
        foreach (var c in todo) { c.Done = 0; c.Finished = false; c.Ok = c.IconDone = false; c.OutDir = null; c.FolderIco = null; Set(c, L.T("Waiting"), Z.Text, ""); }
        totalFrames = todo.Sum(c => c.Info.Frames);
        if (folderIcons.Checked)   // render every clip's icon now, in conversion order, before the engine can delete a source
        {
            var list = todo.ToList();
            Task.Run(() =>
            {
                foreach (var c in list)
                {
                    try { c.FolderIco = FolderIcon.Make(c.Path, c.Info.Frames); } catch { continue; }   // no frame, no icon
                    var cc = c;
                    BeginInvoke(new Action(() => TryFolderIcon(cc)));
                }
            });
        }
        // Command lines are limited to 32K characters, so long lists run as several engine processes
        batches = new Queue<List<string>>();
        var batch = new List<string>();
        int len = 0;
        foreach (var c in todo)
        {
            if (len + c.Path.Length > 24000 && batch.Count > 0) { batches.Enqueue(batch); batch = new List<string>(); len = 0; }
            batch.Add(c.Path);
            len += c.Path.Length + 3;
        }
        batches.Enqueue(batch);
        stopping = false;
        startedAt = DateTime.Now;
        pausedFor = TimeSpan.Zero;
        overall.Value = 0;
        Running(true);
        speed.Clear();
        // "Converting 3 clips to D:\out as Lossy CinemaDNG (Resolve) 3.0:1, denoise light."
        Write(L.F("Converting {0} {0:clip|clips} to {1} as {2}", todo.Count, outDir, format.Text) +
              (FixedQuality ? L.F(", fixed quality {0}", LevelLong[level.Value]) : format.SelectedIndex == 0 ? " " + RatioString() : "") +
              (denoise.SelectedIndex > 0 ? L.T(denoise.SelectedIndex == 1 ? ", denoise light" : ", denoise strong") : "") + ".");
        if (deleteSource.Checked) Write(L.T("Each source clip goes to the Recycle Bin once it converts with no errors and its new files check out on disk."));
        Settings.Save();
        NextBatch(engine, outDir);
    }

    string Options(string outDir)
    {
        var sb = new StringBuilder();
        sb.Append(" -o ").Append(Quote(outDir)).Append(" --progress -j ").Append(threads.Value);
        switch (format.SelectedIndex)
        {
            case 0:
                if (FixedQuality)
                    sb.Append(string.Format(CultureInfo.InvariantCulture, " --quality {0:0.00} --min-ratio 2 --max-ratio {1}", LevelTarget[level.Value], LevelMaxRatio[level.Value]));
                else
                    sb.Append(" --lossy ").Append(Ratio.ToString("0.0", CultureInfo.InvariantCulture));
                var use = gpuBoxes.Where(b => b.Checked && b.Tag != null).Select(b => (string)b.Tag).ToList();
                if (!gpu.Switched || use.Count == 0) sb.Append(" --cpu"); else sb.Append(" --gpus ").Append(string.Join(",", use));
                break;
            case 1: sb.Append(" --lossless"); break;
            case 2: sb.Append(" --uncompressed"); break;
            case 3: sb.Append(" --uncompressed --pack"); break;
        }
        if (scale.SelectedIndex == 1) sb.Append(" --scale half");
        if (scale.SelectedIndex == 2) sb.Append(" --scale quarter");
        if (denoise.SelectedIndex == 1) sb.Append(" --denoise light");
        if (denoise.SelectedIndex == 2) sb.Append(" --denoise strong");
        if (colorMatrix.SelectedIndex == 1) sb.Append(" --zcam-dng-color");
        if (colorMatrix.SelectedIndex == 2) sb.Append(" --camera-color");
        if (colorMatrix.SelectedIndex == 3) sb.Append(" --videosuite-color");
        if (deleteSource.Checked) sb.Append(" --delete-originals");   // Recycle Bin only; never --permanent from the GUI
        return sb.ToString();
    }

    // Backslashes before the closing quote are doubled, or the command line reads \" as a quote: "D:\\" reaches the
    // engine as D:\ (trimming it gave "D:", a relative path)
    static string Quote(string s) { return "\"" + s + new string('\\', s.Length - s.TrimEnd('\\').Length) + "\""; }

    void NextBatch(string engine, string outDir)
    {
        if (stopping || batches.Count == 0) { Finish(); return; }
        var batch = batches.Dequeue();
        var args = "convert " + string.Join(" ", batch.Select(Quote)) + Options(outDir);
        var psi = new ProcessStartInfo(engine, args)
        {
            UseShellExecute = false, CreateNoWindow = true, RedirectStandardError = true, RedirectStandardOutput = true,
            StandardErrorEncoding = Encoding.Default,   // the engine prints paths in the ANSI code page
        };
        try
        {
            proc = new Process { StartInfo = psi, EnableRaisingEvents = true };
            proc.ErrorDataReceived += (s, e) => { if (e.Data != null) BeginInvoke(new Action<string>(Line), e.Data); };
            proc.Exited += (s, e) => BeginInvoke(new Action(() =>
            {
                int code = proc.ExitCode;
                proc.Dispose();
                proc = null;
                if (!stopping && code != 0 && code != 1) Write(L.F("ERROR: the engine stopped with code {0}.", code));
                if (current != null && !current.Finished) { Set(current, L.T(stopping ? "Stopped, folder is incomplete" : "Failed"), Z.RedHover, null); current = null; }
                NextBatch(engine, outDir);
            }));
            proc.Start();
            if (paused) NtSuspendProcess(proc.Handle);   // the previous batch ended just as Pause was clicked
            proc.BeginErrorReadLine();
            proc.BeginOutputReadLine();
        }
        catch (Exception ex) { Write(L.F("ERROR: could not start the engine: {0}", ex.Message)); proc = null; Finish(); }
    }

    Clip Find(string path) { return clips.FirstOrDefault(c => c.Path.Equals(path, StringComparison.OrdinalIgnoreCase)); }

    void Line(string line)
    {
        var f = line.Split('\t');
        switch (f[0])
        {
            case "@clip":
                current = Find(f[1]);
                if (current != null) { Set(current, L.T("Converting"), Color.White, "0%"); if (f.Length > 3) current.OutDir = f[3]; }
                break;
            case "@progress":
                if (current == null) break;
                current.Done = int.Parse(f[1]);
                int total = Math.Max(1, int.Parse(f[2]));
                current.Row.Cells[5].Value = (100 * current.Done / total) + "%";
                current.T.Progress = (double)current.Done / total;
                thumbs.Invalidate();
                Overall(double.Parse(f[3], CultureInfo.InvariantCulture));
                break;
            case "@skip":
                var sk = Find(f[1]);
                // The engine's reasons: "not a ZRAW clip", "unrecognized HEVC ZRAW variant"
                if (sk != null) { sk.Finished = true; Set(sk, L.F("Skipped: {0}", L.T(f[2])), Z.Dim, ""); }
                break;
            case "@done":
                var d = Find(f[1]);
                if (d == null) break;
                d.Finished = true;
                d.Ok = f[2] == "1";
                TryFolderIcon(d);
                if (f[2] == "1") Set(d, L.F("Done: {0:0}s, {1:0} fps", double.Parse(f[4], CultureInfo.InvariantCulture), double.Parse(f[5], CultureInfo.InvariantCulture)), Z.Good, "100%");
                else Set(d, L.F("Failed: {0} {0:frame|frames} did not convert", int.Parse(f[3])), Z.RedHover, null);
                if (d == current) current = null;
                Overall(0);
                break;
            default:
                if (line.Trim().Length > 0) Write(EngineText(line.Trim()));
                break;
        }
    }

    // The engine's routine lines (which GPU compresses, denoise, fixed-quality measurements), translated;
    // errors and anything else show as the engine wrote them. Parts are translated on their own (the
    // mode in "<mode>, DCT and rate control on <GPU>"), and numbers are read back so they get the UI's
    // decimal comma.
    static readonly string[,] EngineLines =
    {
        { @"^(lossy.+), DCT and rate control on (.+)$", L.T("{0}, DCT and rate control on {1}") },
        { @"^(lossy.+) on the CPU$", L.T("{0} on the CPU") },
        { @"^(lossy.+) on the CPU, GPU unavailable: (.+)$", L.T("{0} on the CPU, GPU unavailable: {1}") },
        { @"^lossy ([\d.]+):1$", L.T("lossy {0:0.00}:1") },
        { @"^lossy, fixed quality ([\d.]+) x noise \(([\d.]+) to ([\d.]+):1\)$", L.T("lossy, fixed quality {0:0.00} x noise ({1:0.0} to {2:0.0}:1)") },
        { @"^denoise on (.+)$", L.T("denoise on {0}") },
        { @"^denoise needs a GPU \((.+)\): converting without it$", L.T("denoise needs a GPU ({0}): converting without it") },
        { @"^denoise unavailable \((.+)\): converting this clip without it$", L.T("denoise unavailable ({0}): converting this clip without it") },
        { @"^not enough GPU memory$", L.T("not enough GPU memory") },
        { @"^denoise: (\d+) frames per merge \(temporal ([\d.]+), spatial ([\d.]+)\), ISO (\d+) noise model$", L.T("denoise: {0} {0:frame|frames} per merge (temporal {1:0.0}, spatial {2:0.00}), ISO {3} noise model") },
        { @"^fixed quality ([\d.]+) x noise, ISO (\S+): scale ([\d.]+) from (\d+) frames$", L.T("fixed quality {0:0.00} x noise, ISO {1}: scale {2:0.00} from {3} {3:frame|frames}") },
        { @"^fixed quality ([\d.]+) x noise, ISO (\S+): scale ([\d.]+) from (\d+) denoised frames$", L.T("fixed quality {0:0.00} x noise, ISO {1}: scale {2:0.00} from {3} denoised {3:frame|frames}") },
        { @"^fixed quality: no frame could be measured, using ([\d.]+):1 instead$", L.T("fixed quality: no frame could be measured, using {0:0.0}:1 instead") },
    };

    static string EngineText(string line)
    {
        if (!L.Ru) return line;
        for (int i = 0; i < EngineLines.GetLength(0); i++)
        {
            var m = Regex.Match(line, EngineLines[i, 0]);
            if (!m.Success) continue;
            var args = m.Groups.Cast<Group>().Skip(1).Select(g =>
            {
                double d;
                return double.TryParse(g.Value, NumberStyles.Float, CultureInfo.InvariantCulture, out d) ? (object)d : EngineText(g.Value);
            }).ToArray();
            return L.F(EngineLines[i, 1], args);
        }
        return line;
    }

    // Writes a clip's folder icon once both are in: the rendered icon and a successful conversion
    void TryFolderIcon(Clip c)
    {
        if (c.IconDone || !c.Ok || c.FolderIco == null || c.OutDir == null) return;
        c.IconDone = true;
        try { FolderIcon.Apply(c.OutDir, c.FolderIco); }
        catch (Exception ex) { Write(L.F("WARNING: could not set the folder icon for {0}: {1}", Path.GetFileName(c.OutDir), ex.Message)); }
    }

    void Overall(double fps)
    {
        int done = run.Sum(c => c.Finished ? c.Info.Frames : c.Done);
        overall.Value = totalFrames > 0 ? (int)Math.Min(1000, 1000L * done / totalFrames) : 0;
        if (paused) return;   // lines already in the pipe; keep "Paused" showing
        // Current speed: frames over the last 20 seconds, so the estimate follows the clip being converted
        var now = DateTime.Now;
        speed.Enqueue(new KeyValuePair<DateTime, int>(now, done));
        while (speed.Count > 2 && (now - speed.Peek().Key).TotalSeconds > 20) speed.Dequeue();
        double span = (now - speed.Peek().Key).TotalSeconds;
        double rate = span >= 2 ? (done - speed.Peek().Value) / span : fps;
        int clipNo = run.Count(c => c.Finished) + (current != null ? 1 : 0);
        status.Text = L.F("Clip {0} of {1}     {2:N0} / {3:N0} {3:frame|frames}     {4:0.0} fps",
            clipNo, run.Count, done, totalFrames, rate > 0 ? rate : fps);
        if (rate > 0)
        {
            var left = TimeSpan.FromSeconds((totalFrames - done) / rate);
            eta.Text = L.F("Queue: {0} left, done about {1}", Hms(left.TotalSeconds), (now + left).ToString("t", L.Ru ? L.Culture : CultureInfo.CurrentCulture));   // "0:48" in Russian, Windows' own clock format otherwise
        }
    }

    void Set(Clip c, string text, Color color, string progress)
    {
        c.Row.Cells[4].Value = text;
        c.Row.Cells[4].Style.ForeColor = color;
        if (progress != null) c.Row.Cells[5].Value = progress;
        c.T.Progress = text == L.T("Converting") ? 0 : -1;
        thumbs.Invalidate();
    }

    void Pause()
    {
        if (proc == null) return;
        paused = !paused;
        if (paused)
        {
            NtSuspendProcess(proc.Handle);
            pausedAt = DateTime.Now;
            pause.Text = L.T("Resume");
            status.Text = L.T("Paused. The CPU and GPU are free until you resume.");
            eta.Text = "";
            Write(L.T("Paused."));
        }
        else
        {
            NtResumeProcess(proc.Handle);
            pausedFor += DateTime.Now - pausedAt;
            speed.Clear();   // the speed shown restarts from now, not averaged over the pause
            pause.Text = L.T("Pause");
            status.Text = L.T("Resuming...");
            Write(L.T("Resumed."));
        }
    }

    void Stop()
    {
        if (proc == null) return;
        if (paused) pausedFor += DateTime.Now - pausedAt;   // a suspended engine can still be killed
        stopping = true;
        try { proc.Kill(); } catch { }
        Write(L.T("Stopped. The clip that was converting has an incomplete folder."));
    }

    void Finish()
    {
        Running(false);
        var raw = run;
        int ok = raw.Count(c => c.Finished && c.Ok);
        double secs = (DateTime.Now - startedAt - pausedFor).TotalSeconds;
        if (!stopping) overall.Value = 1000;
        int frames = raw.Sum(c => c.Finished ? c.Info.Frames : c.Done);
        if (!stopping && ok == raw.Count && secs > 5) Settings.Set(FpsKey(), (frames / secs).ToString("0.0", CultureInfo.InvariantCulture));
        eta.Text = stopping ? "" : L.F("Took {0}", Hms(secs));
        Write(L.F(stopping ? "Stopped: {0} of {1} {1:clip|clips} converted in {2}." : "Finished: {0} of {1} {1:clip|clips} converted in {2}.", ok, raw.Count, Hms(secs)));
        status.Text = L.T(stopping ? "Stopped." : ok == raw.Count ? "All done. Drop the folders into Resolve." : "Finished with problems. See the list and the log.");
    }

    public bool ConfirmClose()
    {
        if (proc == null) return true;
        if (MessageBox.Show(this, L.T("A conversion is running. Stop it and quit?"), "ZRAW Studio", MessageBoxButtons.YesNo, MessageBoxIcon.Warning) != DialogResult.Yes) return false;
        Stop();
        return true;
    }
}

// A frame of the clip as its output folder's icon in Explorer: an .ico file plus a desktop.ini that
// points at it, both hidden, and the folder marked read-only, which is how Windows knows to read a
// folder's desktop.ini (it doesn't make the files inside read-only).
static class FolderIcon
{
    [DllImport("shell32.dll", CharSet = CharSet.Unicode)] static extern void SHChangeNotify(int eventId, uint flags, string item1, IntPtr item2);
    static readonly int[] Sizes = { 256, 96, 64, 48, 32, 16 };
    static Bitmap badge;

    // The ZRAW clip icon (the one the Explorer add-on gives ZRAW clips), from its 256 px PNG entry;
    // null if missing
    static Bitmap Badge()
    {
        if (badge != null) return badge;
        try
        {
            byte[] ico;
            using (var s = typeof(FolderIcon).Assembly.GetManifestResourceStream("zraw-badge.ico"))
            using (var m = new MemoryStream()) { s.CopyTo(m); ico = m.ToArray(); }
            int count = BitConverter.ToUInt16(ico, 4);
            for (int i = 0; i < count; i++)
            {
                int e = 6 + 16 * i;
                if (ico[e] != 0) continue;   // 0 = 256 px
                int size = BitConverter.ToInt32(ico, e + 8), offset = BitConverter.ToInt32(ico, e + 12);
                using (var png = new MemoryStream(ico, offset, size)) badge = new Bitmap(png);
            }
        }
        catch { }
        return badge;
    }

    // The frame 10% in (as the thumbnails use), fitted across a square with transparent bands above
    // and below, at each icon size; PNG-compressed entries, which Windows reads at every size.
    // The ZRAW badge sits in the frame's bottom-right corner on every size from 32 up, 256 included:
    // Explorer keeps folder icons only at small, medium, large and jumbo (256) sizes, so Large and
    // Extra large icons use the same 256 px picture as the preview pane and need the badge too.
    // 16 px is too small for a readable badge.
    public static byte[] Make(string clip, int frames)
    {
        var mark = Badge();
        using (var frame = ZrawCore.Render(clip, frames / 10))
        using (var ms = new MemoryStream())
        using (var w = new BinaryWriter(ms))
        {
            var pngs = new List<byte[]>();
            foreach (int s in Sizes)
                using (var bmp = new Bitmap(s, s, PixelFormat.Format32bppArgb))
                {
                    using (var g = Graphics.FromImage(bmp))
                    {
                        g.InterpolationMode = InterpolationMode.HighQualityBicubic;
                        g.PixelOffsetMode = PixelOffsetMode.HighQuality;
                        g.Clear(Color.Transparent);
                        int h = Math.Max(1, (int)Math.Round(s * (double)frame.Height / frame.Width)), y = (s - h) / 2;
                        g.DrawImage(frame, new Rectangle(0, y, s, h));
                        if (s >= 32) using (var pen = new Pen(Color.FromArgb(140, 0, 0, 0))) g.DrawRectangle(pen, 0, y, s - 1, h - 1);   // an edge on white backgrounds
                        if (mark != null && s >= 32)
                        {
                            int b = (int)Math.Round(s * 0.30), inset = Math.Max(1, (int)Math.Round(s * 0.025));
                            g.DrawImage(mark, new Rectangle(s - b - inset, y + h - b - inset, b, b));
                        }
                    }
                    using (var png = new MemoryStream()) { bmp.Save(png, ImageFormat.Png); pngs.Add(png.ToArray()); }
                }
            w.Write((short)0); w.Write((short)1); w.Write((short)Sizes.Length);   // ICONDIR
            int offset = 6 + 16 * Sizes.Length;
            for (int i = 0; i < Sizes.Length; i++)   // ICONDIRENTRY: 0 means 256
            {
                w.Write((byte)(Sizes[i] & 0xFF)); w.Write((byte)(Sizes[i] & 0xFF)); w.Write((byte)0); w.Write((byte)0);
                w.Write((short)1); w.Write((short)32); w.Write(pngs[i].Length); w.Write(offset);
                offset += pngs[i].Length;
            }
            foreach (var p in pngs) w.Write(p);
            w.Flush();
            return ms.ToArray();
        }
    }

    [StructLayout(LayoutKind.Sequential, CharSet = CharSet.Unicode)]
    struct SHFOLDERCUSTOMSETTINGS
    {
        public uint dwSize, dwMask; public IntPtr pvid; public string pszWebViewTemplate; public uint cchWebViewTemplate;
        public string pszWebViewTemplateVersion, pszInfoTip; public uint cchInfoTip; public IntPtr pclsid; public uint dwFlags;
        public string pszIconFile; public uint cchIconFile; public int iIconIndex; public string pszLogo; public uint cchLogo;
    }
    [DllImport("shell32.dll", CharSet = CharSet.Unicode)] static extern int SHGetSetFolderCustomSettings(ref SHFOLDERCUSTOMSETTINGS fcs, string path, uint read);

    // Windows' own way to set a folder icon (Properties > Customize > Change Icon): it writes
    // desktop.ini, marks the folder, and tells Explorer the icon changed. Writing desktop.ini by hand
    // leaves Explorer showing the plain folder it already displayed while the clip was converting.
    // The icon file is named after its content (folder-<hash>.ico): Explorer caches icons by file path,
    // so a re-converted folder with a new picture under the old name would keep showing the old one.
    public static void Apply(string dir, byte[] ico)
    {
        string name;
        using (var sha = System.Security.Cryptography.SHA1.Create()) name = "folder-" + BitConverter.ToString(sha.ComputeHash(ico), 0, 4).Replace("-", "").ToLowerInvariant() + ".ico";
        string icoPath = Path.Combine(dir, name), ini = Path.Combine(dir, "desktop.ini");
        foreach (var old in Directory.GetFiles(dir, "folder*.ico"))   // earlier icons of this folder, ours by name
        {
            if (string.Equals(old, icoPath, StringComparison.OrdinalIgnoreCase)) continue;
            try { File.SetAttributes(old, FileAttributes.Normal); File.Delete(old); } catch { }
        }
        foreach (var p in new[] { icoPath, ini }) if (File.Exists(p)) File.SetAttributes(p, FileAttributes.Normal);   // hidden files can't be overwritten
        File.WriteAllBytes(icoPath, ico);
        File.SetAttributes(icoPath, FileAttributes.Hidden | FileAttributes.System);
        var fcs = new SHFOLDERCUSTOMSETTINGS { dwMask = 0x10 /* FCSM_ICONFILE */, pszIconFile = name, iIconIndex = 0 };
        fcs.dwSize = (uint)Marshal.SizeOf(fcs);
        int hr = SHGetSetFolderCustomSettings(ref fcs, dir, 0x2 /* FCS_FORCEWRITE */);
        if (hr != 0) throw new IOException(L.F("Windows refused the folder icon (0x{0})", hr.ToString("X8")));
        const uint pathFlush = 0x0005 /* SHCNF_PATHW */ | 0x1000 /* SHCNF_FLUSH: deliver now, not when Explorer gets around to it */;
        SHChangeNotify(0x00002000 /* SHCNE_UPDATEITEM */, pathFlush, dir, IntPtr.Zero);
        string parent = Path.GetDirectoryName(dir.TrimEnd('\\'));
        if (parent != null) SHChangeNotify(0x00001000 /* SHCNE_UPDATEDIR */, pathFlush, parent, IntPtr.Zero);   // the folder Explorer is showing
    }
}
