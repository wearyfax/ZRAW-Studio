// Thumbnail view shared by the Convert and Tagger tabs. It draws the rows of the page's grid as
// tiles: names, status text, colors and selection all come from the grid, so the list and the
// thumbnails can never disagree. Each row's Tag holds its Thumb (picture, progress, dimmed or not).
using System;
using System.Collections.Concurrent;
using System.Collections.Generic;
using System.Drawing;
using System.IO;
using System.Linq;
using System.Threading;
using System.Windows.Forms;
using ReaLTaiizor.Controls;

public class Thumb
{
    public string Path;
    public bool Raw;              // ZRAW (RAW or HEVC): rendered by zrawcore; anything else asks Windows
    public bool Dim;              // drawn darker: not a RAW clip
    public double Progress = -1;  // 0..1 while converting
    public Bitmap Image;
    public bool Asked, Failed;
    public string Note;           // shown instead of a picture; no picture is made
}

public class ThumbView : ScrollableControl
{
    const int MinW = 240, Pad = 12, TextH = 48, CacheW = 384, CacheH = 216;
    // One shared cache that never evicts (~330 KB a clip); add eviction if people load thousands of clips
    static readonly Dictionary<string, Bitmap> cache = new Dictionary<string, Bitmap>(StringComparer.OrdinalIgnoreCase);
    static readonly List<ThumbView> views = new List<ThumbView>();

    // The Player tab changed the preview look: remake the ZRAW thumbnails with it. Call on the UI thread.
    public static void LookChanged()
    {
        foreach (var v in views)
            foreach (DataGridViewRow r in v.grid.Rows)
            {
                var t = r.Tag as Thumb;
                if (t == null || !t.Raw) continue;
                lock (cache) cache.Remove(t.Path);
                t.Asked = false;
                t.Image = null;
            }
        foreach (var v in views) v.Refill();
    }

    readonly DataGridView grid;
    readonly int statusCol;
    readonly int[] rightCols;     // shown at the right of the status line
    public string EmptyText = L.T("Drag and drop video files here");   // faint, in the middle of an empty view
    public Action<string> Open;   // double-click
    public Action Remove;         // Delete key
    int anchor = -1, hover = -1;
    bool showing;
    readonly ToolTip tip = new ToolTip { AutoPopDelay = 30000, InitialDelay = 600, ReshowDelay = 300 };   // the row's hover card (its first cell's tooltip)

    public ThumbView(DataGridView grid, int statusCol, params int[] rightCols)
    {
        this.grid = grid;
        this.statusCol = statusCol;
        this.rightCols = rightCols;
        views.Add(this);
        Dock = DockStyle.Fill;
        BackColor = Z.Panel;
        DoubleBuffered = true;
        ResizeRedraw = true;
        AutoScroll = true;
        TabStop = true;
        Visible = false;
        SetStyle(ControlStyles.Selectable, true);
        HandleCreated += (s, e) => Z.DarkScrollbars(this);
        grid.CellDoubleClick += (s, e) => { if (e.RowIndex >= 0 && Open != null) Open(((Thumb)grid.Rows[e.RowIndex].Tag).Path); };
        grid.KeyDown += (s, e) => { Key(e); };
    }

    // The List / Thumbnails pair for a toolbar; remembers the choice under `setting`
    public FlowLayoutPanel Switcher(string setting)
    {
        var list = Z.Button(L.T("List"), false);
        var tiles = Z.Button(L.T("Thumbnails"), false);
        var bar = Z.Bar(DockStyle.Right, list, tiles);
        list.Margin = new Padding(0);   // the pair touches, like a segmented switch
        Action<bool> show = on =>
        {
            grid.Visible = !on;
            Visible = showing = on;
            Z.Active(list, !on);
            Z.Active(tiles, on);
            Settings.Set(setting, on ? "thumbs" : "list");
            Refill();
        };
        list.Click += (s, e) => show(false);
        tiles.Click += (s, e) => show(true);
        show(Settings.Get(setting, "list") == "thumbs");
        return bar;
    }

    // Call after rows were added or removed
    public void Refill()
    {
        Relayout();
        if (!showing) return;   // thumbnails are only made once someone looks at them
        var todo = new ConcurrentQueue<Thumb>();
        foreach (DataGridViewRow r in grid.Rows)
        {
            var t = r.Tag as Thumb;
            if (t == null || t.Asked || t.Note != null) continue;
            t.Asked = true;
            Bitmap b;
            lock (cache) cache.TryGetValue(t.Path, out b);
            if (b != null) t.Image = b; else todo.Enqueue(t);
        }
        // STA threads, because Windows' thumbnail handlers are COM objects
        for (int k = 0; k < Math.Min(3, todo.Count); k++)
        {
            var th = new Thread(() => { Thumb t; while (todo.TryDequeue(out t)) Make(t); }) { IsBackground = true };
            th.SetApartmentState(ApartmentState.STA);
            th.Start();
        }
    }

    void Make(Thumb t)
    {
        Bitmap small = null;
        try
        {
            Bitmap big = null;
            if (t.Raw)
                try { var mi = MovInfo.Read(t.Path); if (mi.TrueRaw || mi.Hevc && mi.Convertible) big = ZrawCore.Render(t.Path, mi.Frames / 10); } catch { }   // frame 10% in, like Explorer; HEVC ZRAW decoded by Windows
            if (big == null) big = ZrawCore.ShellThumb(t.Path, CacheW);
            using (big) small = ZrawCore.Shrink(big, CacheW, CacheH);
            lock (cache) cache[t.Path] = small;
        }
        catch { }
        t.Image = small;   // a reference write, safe to read from OnPaint
        t.Failed = small == null;
        try { if (IsHandleCreated) BeginInvoke(new Action(Invalidate)); } catch { }
    }

    int Cols { get { return Math.Max(1, (ClientSize.Width - Pad) / (MinW + Pad)); } }
    int TW { get { return (ClientSize.Width - Pad) / Cols - Pad; } }   // tiles stretch so the grid fills the width
    int TH { get { return TW * 9 / 16; } }

    void Relayout()
    {
        int rows = (grid.Rows.Count + Cols - 1) / Cols;
        AutoScrollMinSize = new Size(0, rows == 0 ? 0 : Pad + rows * (TH + TextH + Pad));
        Invalidate();
    }

    protected override void OnResize(EventArgs e) { base.OnResize(e); Relayout(); }
    protected override void OnScroll(ScrollEventArgs se) { base.OnScroll(se); Invalidate(); }
    protected override void OnMouseWheel(MouseEventArgs e) { base.OnMouseWheel(e); Invalidate(); }

    Rectangle Tile(int i)
    {
        return new Rectangle(Pad + i % Cols * (TW + Pad), Pad + i / Cols * (TH + TextH + Pad) + AutoScrollPosition.Y, TW, TH + TextH);
    }

    int Hit(Point p)
    {
        for (int i = 0; i < grid.Rows.Count; i++) if (Tile(i).Contains(p)) return i;
        return -1;
    }

    protected override void OnPaint(PaintEventArgs e)
    {
        var g = e.Graphics;
        if (grid.Rows.Count == 0)
        {
            TextRenderer.DrawText(g, EmptyText, Z.HintFont, ClientRectangle, Z.Faint, TextFormatFlags.HorizontalCenter | TextFormatFlags.VerticalCenter);
            return;
        }
        const TextFormatFlags one = TextFormatFlags.VerticalCenter | TextFormatFlags.EndEllipsis | TextFormatFlags.NoPrefix | TextFormatFlags.SingleLine;
        g.InterpolationMode = System.Drawing.Drawing2D.InterpolationMode.HighQualityBilinear;
        int tw = TW, th = TH;
        for (int i = 0; i < grid.Rows.Count; i++)
        {
            var r = Tile(i);
            if (!r.IntersectsWith(e.ClipRectangle)) continue;
            var row = grid.Rows[i];
            var t = (Thumb)row.Tag;
            var back = row.DefaultCellStyle.BackColor;
            using (var b = new SolidBrush(row.Selected ? Z.Line : i == hover ? Z.RowAlt : back.IsEmpty ? Z.Row : back)) g.FillRectangle(b, r);
            var pic = new Rectangle(r.X, r.Y, tw, th);
            using (var b = new SolidBrush(Z.Ink)) g.FillRectangle(b, pic);
            if (t.Image != null)
            {
                double k = Math.Min((double)tw / t.Image.Width, (double)th / t.Image.Height);
                int w = (int)(t.Image.Width * k), h = (int)(t.Image.Height * k);
                g.DrawImage(t.Image, pic.X + (tw - w) / 2, pic.Y + (th - h) / 2, w, h);
                if (t.Dim) using (var b = new SolidBrush(Color.FromArgb(110, Z.Ink))) g.FillRectangle(b, pic);
            }
            else TextRenderer.DrawText(g, t.Note ?? L.T(t.Failed ? "No preview" : "Loading..."), Z.UiFont, pic, Z.Dim, TextFormatFlags.HorizontalCenter | TextFormatFlags.VerticalCenter);
            if (t.Progress >= 0)   // converting: progress along the bottom of the picture
                using (var b = new SolidBrush(Z.Red)) g.FillRectangle(b, pic.X, pic.Bottom - 4, (int)(tw * Math.Min(1, t.Progress)), 4);

            var fore = row.DefaultCellStyle.ForeColor;
            var name = new Rectangle(r.X + 10, pic.Bottom + 4, tw - 20, 20);
            string file = Convert.ToString(row.Cells[0].Value);
            if (file.EndsWith(".mov", StringComparison.OrdinalIgnoreCase)) file = file.Substring(0, file.Length - 4);   // so the clip number fits
            TextRenderer.DrawText(g, file, Z.UiFont, name, t.Dim ? Z.Dim : fore.IsEmpty ? Z.Text : fore, one);
            string right = string.Join("   ", rightCols.Select(k => Convert.ToString(row.Cells[k].Value)).Where(v => v.Length > 0));
            int rw = right.Length > 0 ? TextRenderer.MeasureText(right, Z.UiFont).Width : 0;
            var line2 = new Rectangle(r.X + 10, name.Bottom, tw - 20, 20);
            TextRenderer.DrawText(g, right, Z.UiFont, line2, Z.Dim, one | TextFormatFlags.Right);
            var st = row.Cells[statusCol].Style.ForeColor;
            TextRenderer.DrawText(g, Convert.ToString(row.Cells[statusCol].Value), Z.UiFont, new Rectangle(line2.X, line2.Y, line2.Width - rw - 6, line2.Height),
                                  !st.IsEmpty ? st : t.Dim ? Z.Dim : fore.IsEmpty ? Z.Dim : fore, one);
            if (row.Selected) using (var pen = new Pen(Color.White)) g.DrawRectangle(pen, r.X, r.Y, r.Width - 1, r.Height - 1);
        }
    }

    protected override void OnMouseDown(MouseEventArgs e)
    {
        base.OnMouseDown(e);
        Focus();
        int i = Hit(e.Location);
        if (i >= 0 && (ModifierKeys & Keys.Control) != 0) { grid.Rows[i].Selected = !grid.Rows[i].Selected; anchor = i; }
        else if (i >= 0 && (ModifierKeys & Keys.Shift) != 0 && anchor >= 0)
        {
            grid.ClearSelection();
            for (int k = Math.Min(anchor, i); k <= Math.Max(anchor, i); k++) grid.Rows[k].Selected = true;
        }
        else
        {
            grid.ClearSelection();
            if (i >= 0) grid.Rows[i].Selected = true;
            anchor = i;
        }
        Invalidate();
    }

    protected override void OnMouseDoubleClick(MouseEventArgs e)
    {
        base.OnMouseDoubleClick(e);
        int i = Hit(e.Location);
        if (i >= 0 && Open != null) Open(((Thumb)grid.Rows[i].Tag).Path);
    }

    protected override void OnMouseMove(MouseEventArgs e)
    {
        base.OnMouseMove(e);
        int i = Hit(e.Location);
        if (i != hover)
        {
            hover = i;
            Invalidate();
            tip.Hide(this);
            tip.SetToolTip(this, i >= 0 ? grid.Rows[i].Cells[0].ToolTipText ?? "" : "");
        }
    }

    protected override void OnMouseLeave(EventArgs e) { base.OnMouseLeave(e); hover = -1; Invalidate(); tip.SetToolTip(this, ""); }

    protected override void OnKeyDown(KeyEventArgs e) { base.OnKeyDown(e); Key(e); }

    // Same keys in both views: Delete removes, Enter previews, Ctrl+A selects all
    void Key(KeyEventArgs e)
    {
        if (e.KeyCode == Keys.Delete && Remove != null) Remove();
        if (e.KeyCode == Keys.Enter && Open != null && grid.SelectedRows.Count > 0) { e.Handled = e.SuppressKeyPress = true; Open(((Thumb)grid.SelectedRows[0].Tag).Path); }
        if (e.KeyCode == Keys.A && e.Control) { grid.SelectAll(); Invalidate(); }
    }
}
