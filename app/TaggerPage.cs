// Tagger tab: finds the ZRAW clips among the .MOV files of a folder and prefixes them with ZRAW_
// (Tagger.cs), and installs or removes the Explorer add-on (IconHandler.cs).
using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.Drawing;
using System.IO;
using System.Linq;
using System.Threading.Tasks;
using System.Windows.Forms;
using ReaLTaiizor.Controls;
using Panel = System.Windows.Forms.Panel;
using TabPage = System.Windows.Forms.TabPage;

public class TaggerPage : UserControl
{
    readonly DataGridView grid = Z.Grid();
    readonly RichTextBox log = Z.Log();
    readonly MetroButton rescan = Z.Button(L.T("Rescan"), false), rename = Z.Button(L.T("Rename ZRAW"), true), undo = Z.Button(L.T("Undo"), false), icon = Z.Button(L.T("Install thumbnail engine"), false), pick = Z.Button(L.T("Choose folder..."), false);
    readonly ZCheck sub = Z.Check(L.T("Include subfolders"));
    readonly Label folder = Z.Text1(L.T("Drop a folder anywhere in this window"), Z.Dim, 100, Z.RowHeight);
    readonly ToolTip toolTip = new ToolTip { AutoPopDelay = 32767 };   // the longest Windows allows (about 33 s); the default 5 s is too short to read long tooltips
    readonly ThumbView thumbs;
    string dir;

    public TaggerPage()
    {
        BackColor = Z.Panel;
        Font = Z.UiFont;
        grid.Columns.Add("file", L.T("File"));
        grid.Columns.Add("type", L.T("Type"));
        grid.Columns.Add("status", L.T("Status"));
        grid.Columns[0].AutoSizeMode = DataGridViewAutoSizeColumnMode.Fill;
        grid.Columns[1].Width = 120; grid.Columns[2].Width = 320;
        thumbs = new ThumbView(grid, 2, 1) { EmptyText = L.T("Drag and drop a folder of video files here") };
        Z.Hint(grid, thumbs.EmptyText);
        thumbs.Open = p => PreviewForm.Open(p, FindForm(), Write);

        // Buttons and the checkbox on the left; the folder path takes the rest and shortens with "..."
        var toolbar = new Panel { Dock = DockStyle.Top, Height = Z.RowHeight + 10, BackColor = Z.Panel };
        sub.Width = TextRenderer.MeasureText(sub.Text, sub.Font).Width + 30;   // box, gap, text
        folder.Dock = DockStyle.Fill;
        folder.Padding = new Padding(8, 0, 0, 10);
        folder.AutoEllipsis = true;
        toolbar.Controls.Add(folder);
        toolbar.Controls.Add(thumbs.Switcher("taggerView"));
        toolbar.Controls.Add(Z.Bar(DockStyle.Left, pick, rescan, rename, undo, icon, sub));

        var gridFrame = Z.Section(DockStyle.Fill);
        gridFrame.Padding = new Padding(1);
        gridFrame.Controls.Add(grid);
        gridFrame.Controls.Add(thumbs);

        var bottom = new Panel { Dock = DockStyle.Bottom, Height = 180, BackColor = Z.Panel, Padding = new Padding(0, 10, 0, 0) };
        var logFrame = Z.Section(DockStyle.Fill);
        logFrame.Padding = new Padding(8);
        logFrame.BackColor = Z.Ink;
        logFrame.Controls.Add(log);
        bottom.Controls.Add(logFrame);

        Controls.Add(gridFrame);
        Controls.Add(bottom);
        Controls.Add(toolbar);

        pick.Click += (s, e) =>
        {
            string picked = FolderPicker.Pick(this, L.T("Folder with Z CAM clips"), dir);
            if (picked != null) Open(picked);
        };
        rescan.Click += (s, e) => Run(null, null, null);
        sub.CheckedChanged += (s) => { if (dir != null) Run(null, null, null); };
        rename.Click += (s, e) => Run(L.T("Renaming..."), L.T("Renamed {0} {0:file|files}."), Tagger.Rename);
        undo.Click += (s, e) => Run(L.T("Undoing the renames..."), L.T("Undo done: {0} {0:file|files} renamed back."), Tagger.Undo);
        icon.Click += (s, e) =>
        {
            try { if (IconHandler.Installed) IconHandler.Uninstall(Write); else IconHandler.Install(Write); }
            catch (Exception ex) { Write(L.F("ERROR: {0}", ex.Message)); }
            IconText();
        };
        IconText();
        log.HandleCreated += (s, e) => Write(L.T("Finds the ZRAW clips among your H.265 clips by looking inside each file, and adds the ZRAW_ prefix to their names. Drop a folder of .MOV clips here."));
    }

    void Write(string s) { Z.Write(log, s); }

    void IconText()
    {
        icon.Text = L.T(IconHandler.Installed ? "Remove thumbnail engine" : "Install thumbnail engine");
        icon.Width = TextRenderer.MeasureText(icon.Text, Z.UiFont).Width + 36;
    }

    public void Open(string p)
    {
        if (File.Exists(p)) p = Path.GetDirectoryName(p);
        if (!Directory.Exists(p)) { Write(L.F("Not a folder: {0}", p)); return; }
        dir = p;
        folder.Text = p;
        toolTip.SetToolTip(folder, p);
        Write(L.F("Folder: {0}", p));
        Run(null, null, null);
    }

    // Rename and Undo scan before they act, and every action scans again afterward, so the list never goes stale.
    // `busy` and `done` are the log lines before and after the action; `done` gets the number of files renamed.
    async void Run(string busy, string done, Func<string, bool, Action<string>, int> action)
    {
        if (dir == null) { Write(L.T("Drop a folder first.")); return; }
        string d = dir;
        bool deep = sub.Checked;
        List<Clip> clips = null;
        rescan.Enabled = rename.Enabled = undo.Enabled = false;
        try
        {
            await Task.Run(() =>
            {
                if (action != null) { Write(busy); Write(L.F(done, action(d, deep, Write))); }
                var sw = Stopwatch.StartNew();
                clips = Tagger.Scan(d, deep);
                int z = clips.Count(c => c.Kind == "ZRAW"), h = clips.Count(c => c.Kind == "H.265");
                Write(L.F("Scanned {0} .MOV {0:file|files} in {1} ms: {2} ZRAW ({3} to rename, {4} tagged), {5} H.265, {6} other",
                    clips.Count, sw.ElapsedMilliseconds, z, clips.Count(c => c.Status == Tagger.ToRename),
                    clips.Count(c => c.Status == Tagger.Done), h, clips.Count - z - h));
                foreach (var c in clips.Where(c => c.Kind != "ZRAW" && c.Kind != "H.265" || c.Status == Tagger.NotZraw))
                    Write(L.F("WARNING: {0}: {1}", c.Name, c.Kind + (c.Status == Tagger.NotZraw ? ", " + L.T("has a ZRAW_ name but is not ZRAW") : "")));
            });
        }
        catch (Exception e) { Write(L.F("ERROR: {0}", e.Message)); }
        if (clips != null) Fill(clips);
        rescan.Enabled = rename.Enabled = undo.Enabled = true;
    }

    void Fill(List<Clip> clips)
    {
        grid.Rows.Clear();
        foreach (var c in clips)
        {
            var row = grid.Rows[grid.Rows.Add(c.Name, c.Kind, L.T(c.Status))];
            row.Tag = new Thumb { Path = c.Path, Raw = c.Kind == "ZRAW", Dim = c.Kind != "ZRAW" };
            if (c.Kind == "ZRAW") { row.DefaultCellStyle.BackColor = Z.ZrawRow; row.DefaultCellStyle.ForeColor = Color.White; }
            if (c.Status == Tagger.NotZraw || c.Kind.StartsWith(L.T("unreadable"))) row.DefaultCellStyle.ForeColor = Z.RedHover;
        }
        grid.ClearSelection();
        thumbs.Refill();
    }
}
