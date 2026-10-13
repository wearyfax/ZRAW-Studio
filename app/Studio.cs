// ZRAW Studio: the Z CAM ZRAW converter (zrawdng engine), tagger and player in one window.
// ReaLTaiizor (Metro controls, MIT) themed in Z CAM colors. Everything ships inside the exe:
// ReaLTaiizor.dll as a gzip resource, plus the zrawdng.exe engine and the Explorer add-on DLL, which
// are written to %LOCALAPPDATA% under a content hash the first time they are needed.
using System;
using System.Collections.Generic;
using System.Drawing;
using System.Drawing.Drawing2D;
using System.Drawing.Imaging;
using System.Globalization;
using System.IO;
using System.IO.Compression;
using System.Linq;
using System.Reflection;
using System.Runtime.CompilerServices;
using System.Runtime.InteropServices;
using System.Security.Cryptography;
using System.Text;
using System.Windows.Forms;
using ReaLTaiizor.Controls;
using Panel = System.Windows.Forms.Panel;
using TabPage = System.Windows.Forms.TabPage;
using ReaLTaiizor.Enum.Metro;
using ReaLTaiizor.Forms;
using ReaLTaiizor.Child.Metro;
using Microsoft.Win32;

// The version shown on the About tab and compared with GitHub release tags (v1.0.0)
[assembly: AssemblyTitle("ZRAW Studio")]
[assembly: AssemblyProduct("ZRAW Studio")]
[assembly: AssemblyVersion("1.0.0.0")]
[assembly: AssemblyFileVersion("1.0.0.0")]
[assembly: AssemblyInformationalVersion("1.0.0")]

// Z CAM palette
static class Z
{
    public static readonly Color Red = Color.FromArgb(0xFF, 0x00, 0x04);
    public static readonly Color RedHover = Color.FromArgb(0xFF, 0x3D, 0x40);
    public static readonly Color RedPress = Color.FromArgb(0xC4, 0x00, 0x03);
    public static readonly Color Ink = Color.FromArgb(0x10, 0x10, 0x15);       // Z CAM "woodsmoke"
    public static readonly Color Panel = Color.FromArgb(0x18, 0x18, 0x1F);
    public static readonly Color Row = Color.FromArgb(0x1E, 0x1E, 0x26);
    public static readonly Color RowAlt = Color.FromArgb(0x23, 0x23, 0x2C);
    public static readonly Color Line = Color.FromArgb(0x30, 0x30, 0x3A);
    public static readonly Color Text = Color.FromArgb(0xE8, 0xE8, 0xEC);
    public static readonly Color Dim = Color.FromArgb(0x8A, 0x8A, 0x96);
    public static readonly Color Faint = Color.FromArgb(0x2E, 0x2E, 0x38);   // just above the panel color: hints you only notice when you look for them
    public static readonly Color ZrawRow = Color.FromArgb(0x3A, 0x0E, 0x12);
    public static readonly Color Good = Color.FromArgb(0x5C, 0xC8, 0x7A);
    public static readonly Font UiFont = new Font("Segoe UI", 9.5f);
    public static readonly Font HeadFont = new Font("Segoe UI Semibold", 9f);
    public static readonly Font BigFont = new Font("Segoe UI Semibold", 20f);
    public static readonly Font MonoFont = new Font("Consolas", 9.5f);
    public static readonly Font HintFont = new Font("Segoe UI", 13f);

    // The faint "drag and drop" line in the middle of an empty list
    public static void Hint(DataGridView g, string text)
    {
        g.Paint += (s, e) =>
        {
            if (g.Rows.Count > 0) return;
            var r = new Rectangle(0, g.ColumnHeadersHeight, g.Width, g.Height - g.ColumnHeadersHeight);
            TextRenderer.DrawText(e.Graphics, text, HintFont, r, Faint, TextFormatFlags.HorizontalCenter | TextFormatFlags.VerticalCenter);
        };
        g.Resize += (s, e) => { if (g.Rows.Count == 0) g.Invalidate(); };
    }

    // Primary (red) or secondary (dark) button
    public static MetroButton Button(string text, bool primary)
    {
        var b = new MetroButton { Text = text, Font = UiFont, Size = new Size(TextRenderer.MeasureText(text, UiFont).Width + 36, 32), Margin = new Padding(4), Cursor = Cursors.Hand };
        b.IsDerivedStyle = false;
        b.Style = Style.Custom;
        if (primary)
        {
            b.NormalColor = b.NormalBorderColor = Red;
            b.HoverColor = b.HoverBorderColor = RedHover;
            b.PressColor = b.PressBorderColor = RedPress;
            b.NormalTextColor = b.HoverTextColor = b.PressTextColor = Color.White;
        }
        else
        {
            b.NormalColor = Row; b.NormalBorderColor = Line; b.NormalTextColor = Text;
            b.HoverColor = RowAlt; b.HoverBorderColor = Red; b.HoverTextColor = Color.White;
            b.PressColor = Line; b.PressBorderColor = RedPress; b.PressTextColor = Color.White;
        }
        b.DisabledBackColor = Row; b.DisabledBorderColor = Line; b.DisabledForeColor = Dim;
        return b;
    }

    public static MetroLabel Label(string text, Color color, Font font)
    {
        var l = new MetroLabel { Text = text, Font = font ?? UiFont, AutoSize = true, Margin = new Padding(3, 6, 3, 2) };
        l.IsDerivedStyle = false;
        l.Style = Style.Custom;
        l.ForeColor = color;
        l.BackColor = Panel;
        return l;
    }

    public static MetroComboBox Combo(params string[] items)
    {
        var c = new MetroComboBox { Font = UiFont, Width = 260, Margin = new Padding(3, 3, 3, 6) };
        c.IsDerivedStyle = false;
        c.Style = Style.Custom;
        c.BackgroundColor = Row; c.BorderColor = Line; c.ForeColor = Text; c.ArrowColor = Red;
        c.SelectedItemBackColor = RedPress; c.SelectedItemForeColor = Color.White;
        c.DisabledBackColor = Row; c.DisabledBorderColor = Line; c.DisabledForeColor = Dim;
        c.Items.AddRange(items);
        c.SelectedIndex = 0;
        return c;
    }

    public static MetroSwitch Switch()
    {
        var s = new MetroSwitch { Margin = new Padding(3, 6, 3, 3), Cursor = Cursors.Hand };
        s.IsDerivedStyle = false;
        s.Style = Style.Custom;
        s.BackgroundColor = Row; s.BorderColor = Line; s.CheckColor = Red; s.UnCheckColor = Line; s.SymbolColor = Color.White; s.ForeColor = Text;
        s.DisabledBorderColor = Line; s.DisabledCheckColor = Line; s.DisabledUnCheckColor = Row;
        return s;
    }

    public static ZCheck Check(string text) { return new ZCheck(text); }

    public static MetroTextBox TextBox()
    {
        var t = new MetroTextBox { Font = UiFont, Width = 260, Margin = new Padding(3, 3, 3, 3) };
        t.IsDerivedStyle = false;
        t.Style = Style.Custom;
        t.BackColor = Row; t.BorderColor = Line; t.HoverColor = Red; t.ForeColor = Text;
        t.DisabledBackColor = Row; t.DisabledBorderColor = Line; t.DisabledForeColor = Dim;
        return t;
    }

    public static MetroTrackBar Slider(int min, int max, int value)
    {
        var s = new MetroTrackBar { Minimum = min, Maximum = max, Value = value, Width = 260, Height = 22, Margin = new Padding(3, 6, 3, 6), Cursor = Cursors.Hand };
        s.IsDerivedStyle = false;
        s.Style = Style.Custom;
        s.BackgroundColor = Line; s.ValueColor = Red; s.HandlerColor = Text;
        s.DisabledBackColor = Row; s.DisabledBorderColor = Line; s.DisabledHandlerColor = Dim; s.DisabledValueColor = Line;
        return s;
    }

    public static MetroProgressBar Progress()
    {
        var p = new MetroProgressBar { Minimum = 0, Maximum = 1000, Value = 0, Height = 10 };
        p.IsDerivedStyle = false;
        p.Style = Style.Custom;
        p.BackgroundColor = Ink; p.BorderColor = Line; p.ProgressColor = Red;
        p.DisabledBackColor = Ink; p.DisabledBorderColor = Line; p.DisabledProgressColor = Line;
        return p;
    }

    public static ZNumeric Numeric(int min, int max, int value)
    {
        var n = new ZNumeric { Minimum = min, Maximum = max };
        n.Value = value;
        return n;
    }

    // Plain DataGridView in Z CAM colors (Metro has no grid), with dark scrollbars
    public static DataGridView Grid()
    {
        var g = new DataGridView
        {
            Dock = DockStyle.Fill, BorderStyle = BorderStyle.None, BackgroundColor = Panel, GridColor = Line,
            AllowUserToAddRows = false, AllowUserToResizeRows = false, ReadOnly = true, RowHeadersVisible = false,
            SelectionMode = DataGridViewSelectionMode.FullRowSelect, EnableHeadersVisualStyles = false, Font = UiFont,
            ColumnHeadersHeightSizeMode = DataGridViewColumnHeadersHeightSizeMode.DisableResizing, ColumnHeadersHeight = 30,
            CellBorderStyle = DataGridViewCellBorderStyle.SingleHorizontal, ColumnHeadersBorderStyle = DataGridViewHeaderBorderStyle.None,
        };
        g.RowTemplate.Height = 26;
        g.ColumnHeadersDefaultCellStyle.BackColor = Ink;
        g.ColumnHeadersDefaultCellStyle.ForeColor = Dim;
        g.ColumnHeadersDefaultCellStyle.SelectionBackColor = Ink;
        g.ColumnHeadersDefaultCellStyle.Font = HeadFont;
        g.DefaultCellStyle.BackColor = Row;
        g.DefaultCellStyle.ForeColor = Text;
        g.AlternatingRowsDefaultCellStyle.BackColor = RowAlt;
        g.DefaultCellStyle.SelectionBackColor = Line;   // neutral, so red only ever means ZRAW / problems
        g.DefaultCellStyle.SelectionForeColor = Color.White;
        g.HandleCreated += (s, e) => DarkScrollbars(g);
        return g;
    }

    // Log console: a RichTextBox on Ink, timestamps dim, problems in red
    public static RichTextBox Log()
    {
        var r = new RichTextBox { Dock = DockStyle.Fill, ReadOnly = true, BorderStyle = BorderStyle.None, BackColor = Ink, ForeColor = Text, Font = MonoFont, DetectUrls = false };
        r.HandleCreated += (s, e) => DarkScrollbars(r);
        return r;
    }

    public static void Write(RichTextBox box, string s)
    {
        if (box.InvokeRequired) { box.BeginInvoke(new Action<RichTextBox, string>(Write), box, s); return; }
        bool bad = s.StartsWith("SKIP") || s.StartsWith("FAILED") || s.StartsWith("WARNING") || s.StartsWith("ERROR") || s.StartsWith("frame ");
        box.SelectionStart = box.TextLength;
        box.SelectionColor = Dim;
        box.AppendText(DateTime.Now.ToString("HH:mm:ss  "));
        box.SelectionColor = bad ? RedHover : Text;
        box.AppendText(s + "\n");
        box.ScrollToCaret();
        LogFile.Append(s);
    }

    [DllImport("uxtheme.dll", CharSet = CharSet.Unicode)] static extern int SetWindowTheme(IntPtr hwnd, string app, string idList);
    [DllImport("user32.dll")] static extern bool ReleaseCapture();
    [DllImport("user32.dll")] static extern IntPtr SendMessage(IntPtr h, int msg, IntPtr w, IntPtr l);
    public static void DarkScrollbars(Control c) { try { SetWindowTheme(c.Handle, "DarkMode_Explorer", null); } catch { } }

    // Bordered dark panel used for the option and log sections
    public static Panel Section(DockStyle dock)
    {
        var p = new Panel { Dock = dock, BackColor = Panel, Padding = new Padding(12) };
        p.Paint += (s, e) => { using (var pen = new Pen(Line)) e.Graphics.DrawRectangle(pen, 0, 0, p.Width - 1, p.Height - 1); };
        return p;
    }

    // A row of controls in one line, vertically centered on the 32 px buttons, 8 px apart, flush with
    // the edge it docks to so it lines up with the panels below it. 10 px gap underneath.
    public const int RowHeight = 32, Gap = 8;
    public static FlowLayoutPanel Bar(DockStyle dock, params Control[] items)
    {
        var f = new FlowLayoutPanel { Dock = dock, AutoSize = true, WrapContents = false, BackColor = Panel, Margin = new Padding(0), Padding = new Padding(0, 0, 0, 10) };
        for (int i = 0; i < items.Length; i++)
        {
            var c = items[i];
            int h = c is Label ? TextRenderer.MeasureText("Ag", c.Font).Height : c.Height;
            c.Margin = new Padding(0, Math.Max(0, (RowHeight - h) / 2), i == items.Length - 1 ? 0 : Gap, 0);
        }
        f.Controls.AddRange(items);
        return f;
    }

    // Plain label for fixed layouts: no wrapping, "..." when it does not fit
    public static Label Text1(string text, Color color, int width, int height)
    {
        return new Label { Text = text, ForeColor = color, BackColor = Panel, Font = UiFont, AutoSize = false, Size = new Size(width, height), TextAlign = ContentAlignment.MiddleLeft, AutoEllipsis = true, UseMnemonic = false, Margin = new Padding(0) };
    }

    // A path shortened in the middle to fit the label, so the drive and the file name both stay readable
    public static string ShortPath(string path, Label l)
    {
        string s = string.Copy(path);
        TextRenderer.MeasureText(s, l.Font, new Size(l.Width, l.Height), TextFormatFlags.PathEllipsis | TextFormatFlags.ModifyString | TextFormatFlags.SingleLine);
        int end = s.IndexOf('\0');
        return end >= 0 ? s.Substring(0, end) : s;
    }

    // Selected look for the List / Thumbnails pair
    public static void Active(MetroButton b, bool on)
    {
        b.NormalColor = on ? Line : Row;
        b.NormalTextColor = on ? Color.White : Dim;
        b.Invalidate();
    }

    // Z CAM colors and the window buttons for a Metro window
    public static void Dress(MetroForm f, bool dialog = false)
    {
        f.Font = UiFont;
        try { f.Icon = Icon.ExtractAssociatedIcon(Application.ExecutablePath); } catch { }
        f.Style = Style.Custom;
        f.BackgroundColor = Panel; f.HeaderColor = Ink; f.BorderColor = Red; f.TextColor = Text; f.ForeColor = Text;
        f.SmallLineColor1 = Red; f.SmallLineColor2 = RedPress;
        f.ShowHeader = true;
        // Icon and title drawn here: Metro's own title has no icon. Dragging them moves the window.
        f.ShowTitle = false;
        var logo = new PictureBox { Size = new Size(24, 24), Location = new Point(10, 8), SizeMode = PictureBoxSizeMode.CenterImage, BackColor = Ink };
        try { using (var s = typeof(Z).Assembly.GetManifestResourceStream("icon.ico")) logo.Image = new Icon(s, 24, 24).ToBitmap(); } catch { }
        var title = new Label { Text = f.Text.ToUpperInvariant(), Font = UiFont, ForeColor = Text, BackColor = Ink, AutoSize = true, Location = new Point(40, 11), UseMnemonic = false };
        f.TextChanged += (s, e) => title.Text = f.Text.ToUpperInvariant();
        MouseEventHandler drag = (s, e) => { if (e.Button == MouseButtons.Left) { ReleaseCapture(); SendMessage(f.Handle, 0xA1 /* WM_NCLBUTTONDOWN */, (IntPtr)2 /* HTCAPTION */, IntPtr.Zero); } };
        logo.MouseDown += drag;
        title.MouseDown += drag;
        f.Controls.Add(logo);
        f.Controls.Add(title);
        f.AllowResize = true;
        f.Padding = new Padding(2, 40, 2, 2);

        Control box;
        if (dialog)   // just a close button: Metro's control box always draws minimize and maximize too
        {
            var x = new Label { Text = "✕", Font = new Font("Segoe UI", 10f), ForeColor = Text, BackColor = Ink, Size = new Size(36, 28), TextAlign = ContentAlignment.MiddleCenter, Cursor = Cursors.Hand };
            x.MouseEnter += (s, e) => { x.BackColor = Red; x.ForeColor = Color.White; };
            x.MouseLeave += (s, e) => { x.BackColor = Ink; x.ForeColor = Text; };
            x.Click += (s, e) => { f.DialogResult = DialogResult.Cancel; f.Close(); };
            box = x;
        }
        else
        {
            var m = new MetroControlBox { MaximizeBox = true, MinimizeBox = true, Size = new Size(100, 25) };
            m.IsDerivedStyle = false;
            m.Style = Style.Custom;
            m.CloseHoverBackColor = Red; m.CloseHoverForeColor = Color.White; m.CloseNormalForeColor = Text;
            m.MaximizeHoverBackColor = Line; m.MaximizeHoverForeColor = Color.White; m.MaximizeNormalForeColor = Text;
            m.MinimizeHoverBackColor = Line; m.MinimizeHoverForeColor = Color.White; m.MinimizeNormalForeColor = Text;
            m.DisabledForeColor = Dim;
            box = m;
        }
        f.Controls.Add(box);
        // Keep the window buttons in the top-right corner whatever size Windows gives the form
        // (MetroForm shifts loose header controls around while it lays out, so these are re-placed too)
        EventHandler place = (s, e) =>
        {
            box.Location = new Point(f.ClientSize.Width - box.Width - 2, 2);
            logo.Location = new Point(10, (40 - logo.Height) / 2);
            title.Location = new Point(logo.Right + 8, (40 - title.Height) / 2);
            box.BringToFront(); logo.BringToFront(); title.BringToFront();
        };
        f.Load += place;
        f.Resize += place;
    }

    public static IEnumerable<Control> All(Control root)
    {
        yield return root;
        foreach (Control c in root.Controls) foreach (var d in All(c)) yield return d;
    }
}

// Checkbox drawn here, box and text. MetroCheckBox cut off descenders (the g in "converting") and
// painted its box white while disabled, with no color to change that.
// Same members the pages use: Checked, CheckedChanged(sender), Text, Enabled, Tag.
class ZCheck : Panel
{
    const int Box = 17;
    bool isChecked;
    public event Action<object> CheckedChanged;

    public ZCheck(string text)
    {
        Text = text;
        Font = Z.UiFont;
        Size = new Size(260, 24);
        Margin = new Padding(3, 4, 3, 4);
        BackColor = Z.Panel;
        Cursor = Cursors.Hand;
        DoubleBuffered = true;
        Click += (s, e) => { if (Enabled) Checked = !Checked; };   // the text toggles too, like a normal checkbox
        TextChanged += (s, e) => Invalidate();
        EnabledChanged += (s, e) => Invalidate();
    }

    public bool Checked
    {
        get { return isChecked; }
        set
        {
            if (isChecked == value) return;
            isChecked = value;
            Invalidate();
            if (CheckedChanged != null) CheckedChanged(this);
        }
    }

    protected override void OnPaint(PaintEventArgs e)
    {
        base.OnPaint(e);
        var g = e.Graphics;
        int top = (Height - Box) / 2;
        using (var fill = new SolidBrush(Z.Row)) g.FillRectangle(fill, 0, top, Box, Box);
        using (var border = new Pen(Z.Line)) g.DrawRectangle(border, 0, top, Box - 1, Box - 1);
        if (isChecked)   // red tick, gray while disabled
        {
            g.SmoothingMode = SmoothingMode.AntiAlias;
            using (var tick = new Pen(Enabled ? Z.Red : Z.Dim, 2.2f) { StartCap = LineCap.Round, EndCap = LineCap.Round, LineJoin = LineJoin.Round })
                g.DrawLines(tick, new[] { new PointF(3.6f, top + 8.4f), new PointF(7.3f, top + 11.4f), new PointF(12.6f, top + 4.6f) });
            g.SmoothingMode = SmoothingMode.Default;
        }
        TextRenderer.DrawText(g, Text, Font, new Rectangle(Box + 7, 0, Width - Box - 7, Height), Enabled ? Z.Text : Z.Dim,
                              TextFormatFlags.Left | TextFormatFlags.VerticalCenter | TextFormatFlags.EndEllipsis | TextFormatFlags.NoPrefix | TextFormatFlags.SingleLine);
    }
}

// Number box with red + and - on the right, in the MetroNumeric look. Used instead of MetroNumeric,
// which steps by more than one per click and lets the value pass its maximum. One click = one step;
// the mouse wheel and the arrow keys step too. Same members the pages use: Value, Minimum, Maximum.
class ZNumeric : Control
{
    int value;
    public int Minimum = 1, Maximum = 100;
    public event Action<object> ValueChanged;

    public ZNumeric()
    {
        Font = Z.UiFont;
        Size = new Size(120, 30);
        Margin = new Padding(3, 3, 3, 6);
        DoubleBuffered = true;
        SetStyle(ControlStyles.Selectable, true);
        Cursor = Cursors.Hand;
        EnabledChanged += (s, e) => Invalidate();
    }

    public int Value
    {
        get { return value; }
        set
        {
            int v = Math.Max(Minimum, Math.Min(Maximum, value));
            if (v == this.value) return;
            this.value = v;
            Invalidate();
            if (ValueChanged != null) ValueChanged(this);
        }
    }

    Rectangle Plus { get { return new Rectangle(Width - 54, 0, 26, Height); } }
    Rectangle Minus { get { return new Rectangle(Width - 28, 0, 26, Height); } }

    protected override void OnMouseDown(MouseEventArgs e)
    {
        base.OnMouseDown(e);
        Focus();
        if (!Enabled || e.Button != MouseButtons.Left) return;
        if (Plus.Contains(e.Location)) Value++;
        else if (Minus.Contains(e.Location)) Value--;
    }

    protected override void OnMouseWheel(MouseEventArgs e) { base.OnMouseWheel(e); if (Enabled) Value += Math.Sign(e.Delta); }

    protected override bool IsInputKey(Keys k) { return k == Keys.Up || k == Keys.Down || base.IsInputKey(k); }
    protected override void OnKeyDown(KeyEventArgs e)
    {
        base.OnKeyDown(e);
        if (!Enabled) return;
        if (e.KeyCode == Keys.Up) Value++;
        if (e.KeyCode == Keys.Down) Value--;
    }

    protected override void OnPaint(PaintEventArgs e)
    {
        var g = e.Graphics;
        g.Clear(Z.Row);
        using (var pen = new Pen(Z.Line)) g.DrawRectangle(pen, 0, 0, Width - 1, Height - 1);
        var flags = TextFormatFlags.VerticalCenter | TextFormatFlags.NoPrefix | TextFormatFlags.SingleLine;
        TextRenderer.DrawText(g, value.ToString(CultureInfo.InvariantCulture), Font, new Rectangle(8, 0, Width - 64, Height), Enabled ? Z.Text : Z.Dim, flags | TextFormatFlags.Right);
        Color sym = Enabled ? Z.Red : Z.Dim;
        TextRenderer.DrawText(g, "+", Z.HeadFont, Plus, Enabled && value < Maximum ? sym : Z.Dim, flags | TextFormatFlags.HorizontalCenter);
        TextRenderer.DrawText(g, "−", Z.HeadFont, Minus, Enabled && value > Minimum ? sym : Z.Dim, flags | TextFormatFlags.HorizontalCenter);
    }
}

// The Explorer-style folder picker (the same window as Open File, in folder mode), used instead of
// the tree view of .NET Framework's FolderBrowserDialog. Null when canceled.
static class FolderPicker
{
    [ComImport, Guid("DC1C5A9C-E88A-4dde-A5A1-60F82A20AEF7")] class FileOpenDialog { }

    [ComImport, Guid("42f85136-db7e-439c-85f1-e4075d135fc8"), InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
    interface IFileOpenDialog
    {
        [PreserveSig] int Show(IntPtr parent);
        void SetFileTypes(uint count, IntPtr specs);
        void SetFileTypeIndex(uint index);
        void GetFileTypeIndex(out uint index);
        void Advise(IntPtr sink, out uint cookie);
        void Unadvise(uint cookie);
        void SetOptions(uint options);
        void GetOptions(out uint options);
        void SetDefaultFolder(IShellItem item);
        void SetFolder(IShellItem item);
        void GetFolder(out IShellItem item);
        void GetCurrentSelection(out IShellItem item);
        void SetFileName([MarshalAs(UnmanagedType.LPWStr)] string name);
        void GetFileName([MarshalAs(UnmanagedType.LPWStr)] out string name);
        void SetTitle([MarshalAs(UnmanagedType.LPWStr)] string title);
        void SetOkButtonLabel([MarshalAs(UnmanagedType.LPWStr)] string text);
        void SetFileNameLabel([MarshalAs(UnmanagedType.LPWStr)] string text);
        void GetResult(out IShellItem item);
        // (the rest of the interface isn't needed)
    }

    [ComImport, Guid("43826D1E-E718-42EE-BC55-A1E261C37BFE"), InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
    interface IShellItem
    {
        void BindToHandler(IntPtr bc, ref Guid bhid, ref Guid riid, out IntPtr ppv);
        void GetParent(out IShellItem parent);
        void GetDisplayName(uint sigdn, [MarshalAs(UnmanagedType.LPWStr)] out string name);
        void GetAttributes(uint mask, out uint attributes);
        void Compare(IShellItem other, uint hint, out int order);
    }

    [DllImport("shell32.dll", CharSet = CharSet.Unicode, PreserveSig = false)]
    static extern void SHCreateItemFromParsingName(string path, IntPtr bc, [In] ref Guid riid, [MarshalAs(UnmanagedType.Interface)] out IShellItem item);

    const uint FOS_PICKFOLDERS = 0x20, FOS_FORCEFILESYSTEM = 0x40, FOS_PATHMUSTEXIST = 0x800;
    const uint SIGDN_FILESYSPATH = 0x80058000;

    public static string Pick(IWin32Window owner, string title, string start)
    {
        var d = (IFileOpenDialog)new FileOpenDialog();
        try
        {
            d.SetOptions(FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST);
            d.SetTitle(title);
            if (!string.IsNullOrEmpty(start) && Directory.Exists(start))
            {
                try { var iid = typeof(IShellItem).GUID; IShellItem s; SHCreateItemFromParsingName(start, IntPtr.Zero, ref iid, out s); d.SetFolder(s); } catch { }
            }
            if (d.Show(owner == null ? IntPtr.Zero : owner.Handle) != 0) return null;   // canceled
            IShellItem result; string path;
            d.GetResult(out result);
            result.GetDisplayName(SIGDN_FILESYSPATH, out path);
            return path;
        }
        finally { Marshal.ReleaseComObject(d); }
    }
}

// Simple key=value settings in %LOCALAPPDATA%\ZRAW Studio\settings.txt
static class Settings
{
    public static readonly string Dir = Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData), "ZRAW Studio");
    static readonly string FilePath = Path.Combine(Dir, "settings.txt");
    static Dictionary<string, string> values;

    static Dictionary<string, string> Load()
    {
        if (values != null) return values;
        values = new Dictionary<string, string>();
        try
        {
            if (File.Exists(FilePath))
                foreach (var line in File.ReadAllLines(FilePath))
                {
                    int i = line.IndexOf('=');
                    if (i > 0) values[line.Substring(0, i)] = line.Substring(i + 1);
                }
        }
        catch { }
        return values;
    }

    public static string Get(string key, string fallback) { string v; return Load().TryGetValue(key, out v) ? v : fallback; }
    public static void Set(string key, string value) { Load()[key] = value; }
    public static void Save()
    {
        try
        {
            Directory.CreateDirectory(Dir);
            var sb = new StringBuilder();
            foreach (var kv in Load()) sb.Append(kv.Key).Append('=').Append(kv.Value).Append("\r\n");
            File.WriteAllText(FilePath, sb.ToString());
        }
        catch { }
    }
}

// Every tab's log lines, one file per session in %LOCALAPPDATA%\ZRAW Studio\logs, for problem
// reports. The newest 20 sessions are kept.
static class LogFile
{
    public static readonly string Dir = Path.Combine(Settings.Dir, "logs");
    static readonly object gate = new object();
    static string path;

    public static void Append(string line)
    {
        lock (gate)
            try
            {
                if (path == null)
                {
                    Directory.CreateDirectory(Dir);
                    foreach (var old in Directory.GetFiles(Dir, "*.log").OrderByDescending(f => f).Skip(19)) File.Delete(old);
                    path = Path.Combine(Dir, DateTime.Now.ToString("yyyy-MM-dd HH-mm-ss", CultureInfo.InvariantCulture) + ".log");
                    File.AppendAllText(path, "ZRAW Studio " + Updates.Version + " on " + Windows() + ", " + Environment.ProcessorCount + " logical CPUs\r\n");
                }
                File.AppendAllText(path, DateTime.Now.ToString("HH:mm:ss  ", CultureInfo.InvariantCulture) + line + "\r\n");
            }
            catch { }   // a full or read-only drive must not stop the app
    }

    // Environment.OSVersion reports Windows 8 to apps without a compatibility manifest
    static string Windows()
    {
        try
        {
            using (var k = Registry.LocalMachine.OpenSubKey(@"SOFTWARE\Microsoft\Windows NT\CurrentVersion"))
                return k.GetValue("ProductName") + " " + k.GetValue("DisplayVersion") + ", build " + k.GetValue("CurrentBuild");
        }
        catch { return Environment.OSVersion.VersionString; }
    }
}

// Files carried inside the exe that must exist on disk to run (the engine, the Explorer add-on).
static class Embedded
{
    public static byte[] Bytes(string resource)
    {
        using (var rs = typeof(Embedded).Assembly.GetManifestResourceStream(resource))
        {
            if (rs == null) return null;
            Stream s = resource.EndsWith(".gz") ? (Stream)new GZipStream(rs, CompressionMode.Decompress) : rs;
            using (s)
            using (var ms = new MemoryStream()) { s.CopyTo(ms); return ms.ToArray(); }
        }
    }

    // Writes the resource to dir\<name>-<hash><ext> unless it is already there; returns the path.
    // The hash in the name means a newer build never overwrites a copy that is still running.
    public static string Extract(string resource, string dir, string name, string ext, bool hashFolder = false)
    {
        byte[] data = Bytes(resource);
        if (data == null) throw new FileNotFoundException("resource missing from the exe: " + resource);
        string hash;
        using (var sha = SHA1.Create()) hash = BitConverter.ToString(sha.ComputeHash(data), 0, 4).Replace("-", "");
        string path = hashFolder ? Path.Combine(dir, name + "-" + hash, name + ext) : Path.Combine(dir, name + "-" + hash + ext);
        if (!File.Exists(path))
        {
            Directory.CreateDirectory(Path.GetDirectoryName(path));
            string tmp = path + ".tmp";
            File.WriteAllBytes(tmp, data);
            File.Move(tmp, path);
        }
        return path;
    }

    public static string Engine() { return Extract("zrawdng.exe.gz", Path.Combine(Settings.Dir, "engine"), "zrawdng", ".exe"); }
}

// The preview look set on the Player tab: the color matrix and the starting exposure for ZRAW Studio's
// previews and thumbnails, and for mpv through the plugin's script-opts\zraw.conf.
static class Look
{
    public static readonly string[] Names = { "ZRAW Studio", "ZCam-DNG (zraw-parser)", "Camera built-in matrix", "VideoSuite" };   // as on the Convert tab
    static readonly string[] ConfNames = { "zrawstudio", "zcamdng", "camera", "videosuite" };   // the engine's kPreviewMatrixNames
    public static readonly double[] Exposures = { -2, -1.5, -1, -0.5, 0, 0.5, 1, 1.5, 2, 2.5, 3, 3.5, 4 };

    public static int Matrix
    {
        get { int i; return int.TryParse(Settings.Get("lookColor", "0"), out i) && i >= 0 && i < Names.Length ? i : 0; }
    }
    public static double Exposure
    {
        get { double e; return double.TryParse(Settings.Get("lookExposure", "0"), NumberStyles.Float, CultureInfo.InvariantCulture, out e) ? e : 0; }
    }

    public static void Set(int matrix, double exposure)
    {
        Settings.Set("lookColor", matrix.ToString(CultureInfo.InvariantCulture));
        Settings.Set("lookExposure", exposure.ToString(CultureInfo.InvariantCulture));
        Settings.Save();
    }

    public static string ConfPath { get { return Path.Combine(Path.GetDirectoryName(Mpv.ScriptsDir()), "script-opts", "zraw.conf"); } }

    // mpv picks it up when the next clip opens
    public static void WriteMpvConf()
    {
        Directory.CreateDirectory(Path.GetDirectoryName(ConfPath));
        File.WriteAllText(ConfPath, "# ZRAW preview defaults, written by ZRAW Studio (Player tab)\nmatrix=" + ConfNames[Matrix] +
                                    "\nexposure=" + Exposure.ToString(CultureInfo.InvariantCulture) + "\n");
    }
}

// Preview frames from zrawcore.dll (engine\, the player's C API), which works without libmpv.
static class ZrawCore
{
    [StructLayout(LayoutKind.Sequential)] struct Info { public int Frames; public double Fps; public int Width, Height, Bits; }
    [DllImport("zrawcore.dll", CallingConvention = CallingConvention.Cdecl, CharSet = CharSet.Unicode)] static extern int zr_clip_info_get(string path, out Info info);
    [StructLayout(LayoutKind.Sequential)] struct PreviewSettings { public double Exposure, WbRed, WbBlue; public int FullRes, Matrix; }   // zr_preview_settings
    [DllImport("zrawcore.dll", CallingConvention = CallingConvention.Cdecl, CharSet = CharSet.Unicode)] static extern int zr_render_frame(string path, int frame, ref PreviewSettings settings, byte[] rgb, long size);
    [DllImport("zrawcore.dll", CallingConvention = CallingConvention.Cdecl)] static extern IntPtr zr_last_error();
    [DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true)] static extern IntPtr LoadLibrary(string path);
    static readonly object gate = new object();
    static bool loaded;

    // Loading the extracted copy first makes the DllImports above bind to it
    static void Load()
    {
        lock (gate)
        {
            if (loaded) return;
            string dll = Embedded.Extract("zrawcore.dll.gz", Path.Combine(Settings.Dir, "engine"), "zrawcore", ".dll", true);
            if (LoadLibrary(dll) == IntPtr.Zero) throw new IOException("cannot load " + dll + " (error " + Marshal.GetLastWin32Error() + ")");
            loaded = true;
        }
    }

    static string Error() { return Marshal.PtrToStringAnsi(zr_last_error()) ?? "unknown error"; }

    // One frame at half resolution in the preview look chosen on the Player tab (camera white balance,
    // that color matrix and exposure, sRGB)
    public static Bitmap Render(string path, int frame)
    {
        Load();
        Info info;
        if (zr_clip_info_get(path, out info) != 0) throw new IOException(Error());
        int w = info.Width / 2, h = info.Height / 2;
        var rgb = new byte[(long)w * h * 3];
        var look = new PreviewSettings { Exposure = Look.Exposure, WbRed = 1, WbBlue = 1, Matrix = Look.Matrix };
        if (zr_render_frame(path, Math.Max(0, Math.Min(info.Frames - 1, frame)), ref look, rgb, rgb.Length) != 0) throw new IOException(Error());
        for (int i = 0; i < rgb.Length; i += 3) { byte t = rgb[i]; rgb[i] = rgb[i + 2]; rgb[i + 2] = t; }   // GDI wants BGR
        var bmp = new Bitmap(w, h, PixelFormat.Format24bppRgb);
        var bd = bmp.LockBits(new Rectangle(0, 0, w, h), ImageLockMode.WriteOnly, bmp.PixelFormat);
        for (int y = 0; y < h; y++) Marshal.Copy(rgb, y * w * 3, bd.Scan0 + y * bd.Stride, w * 3);
        bmp.UnlockBits(bd);
        return bmp;
    }

    // Windows' own thumbnail (Media Foundation for H.265/H.264), for the clips zrawcore cannot read
    [ComImport, Guid("bcc18b79-ba16-442f-80c4-8a59c30c463b"), InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
    interface IShellItemImageFactory { [PreserveSig] int GetImage(Size size, int flags, out IntPtr bitmap); }
    [DllImport("shell32.dll", CharSet = CharSet.Unicode, PreserveSig = false)]
    static extern void SHCreateItemFromParsingName(string path, IntPtr bc, [In] ref Guid riid, [MarshalAs(UnmanagedType.Interface)] out IShellItemImageFactory item);
    [DllImport("gdi32.dll")] static extern bool DeleteObject(IntPtr h);

    public static Bitmap ShellThumb(string path, int size)
    {
        var iid = typeof(IShellItemImageFactory).GUID;
        IShellItemImageFactory f;
        SHCreateItemFromParsingName(path, IntPtr.Zero, ref iid, out f);
        IntPtr h;
        int hr = f.GetImage(new Size(size, size), 0x8 /* SIIGBF_THUMBNAILONLY */, out h);
        Marshal.ReleaseComObject(f);
        if (hr != 0) throw new IOException("Windows has no thumbnail for it (0x" + hr.ToString("X8") + ")");
        try { return Image.FromHbitmap(h); } finally { DeleteObject(h); }
    }

    // Scaled to fit w x h, aspect kept
    public static Bitmap Shrink(Bitmap src, int w, int h)
    {
        double k = Math.Min((double)w / src.Width, (double)h / src.Height);
        var b = new Bitmap(Math.Max(1, (int)(src.Width * k)), Math.Max(1, (int)(src.Height * k)));
        using (var g = Graphics.FromImage(b))
        {
            g.InterpolationMode = InterpolationMode.HighQualityBicubic;
            g.PixelOffsetMode = PixelOffsetMode.HighQuality;
            g.DrawImage(src, 0, 0, b.Width, b.Height);
        }
        return b;
    }
}

// Reads a .MOV's moov box: whether its video is true-RAW ZRAW, frame count and frame rate.
// Same box walk as Tagger.cs, plus the sample tables. Z CAM writes moov after the video data.
class MovInfo
{
    public string Codec = "";     // sample entry fourcc of the video track: zraw, hvc1, ...
    public uint ZrawVersion;      // 0x12EA78D2 = true RAW, 0x45A32DEF = HEVC (firmware 0.94 and later)
    public uint Scheme;           // HEVC: the 'zraw' box's third word, 1 on the clips we can unscramble
    public int Frames;
    public double Fps;
    public bool TrueRaw { get { return Codec == "zraw" && ZrawVersion == 0x12EA78D2; } }
    public bool Hevc { get { return Codec == "zraw" && ZrawVersion == 0x45A32DEF; } }
    public bool Convertible { get { return TrueRaw || Hevc && Scheme == 1; } }

    static uint BE(byte[] b, int o) { return (uint)(b[o] << 24 | b[o + 1] << 16 | b[o + 2] << 8 | b[o + 3]); }

    public static MovInfo Read(string path)
    {
        var info = new MovInfo();
        byte[] moov = null;
        using (var f = new FileStream(path, FileMode.Open, FileAccess.Read, FileShare.ReadWrite, 4096))
        {
            var h = new byte[16];
            long pos = 0, end = f.Length;
            while (pos + 8 <= end)
            {
                f.Position = pos;
                if (f.Read(h, 0, 16) < 8) break;
                long size = BE(h, 0);
                int hdr = 8;
                if (size == 1) { size = (long)BE(h, 8) << 32 | BE(h, 12); hdr = 16; }
                else if (size == 0) size = end - pos;
                if (size < hdr) break;
                if (Encoding.ASCII.GetString(h, 4, 4) == "moov" && size < (64 << 20))
                {
                    moov = new byte[size - hdr];
                    f.Position = pos + hdr;
                    int got = 0, r;
                    while (got < moov.Length && (r = f.Read(moov, got, moov.Length - got)) > 0) got += r;
                    break;
                }
                pos += size;
            }
        }
        if (moov != null) Walk(moov, 0, moov.Length, info, new Track());
        return info;
    }

    class Track { public string Codec = ""; public uint Version, Scheme, Timescale, Duration; public int Samples; }

    static void Walk(byte[] b, int start, int end, MovInfo info, Track t)
    {
        for (int p = start; p + 8 <= end;)
        {
            long size = BE(b, p);
            if (size < 8 || p + size > end) return;
            string type = Encoding.ASCII.GetString(b, p + 4, 4);
            int body = p + 8, stop = (int)(p + size);
            if (type == "trak")
            {
                var nt = new Track();
                Walk(b, body, stop, info, nt);
                if (nt.Codec == "zraw" || (info.Codec == "" && (nt.Codec == "hvc1" || nt.Codec == "hev1" || nt.Codec == "avc1")))
                {
                    info.Codec = nt.Codec; info.ZrawVersion = nt.Version; info.Scheme = nt.Scheme; info.Frames = nt.Samples;
                    if (nt.Duration > 0) info.Fps = (double)nt.Timescale / nt.Duration;
                }
            }
            else if (type == "mdia" || type == "minf" || type == "stbl") Walk(b, body, stop, info, t);
            else if (type == "mdhd" && size >= 32) t.Timescale = b[body] == 1 ? BE(b, body + 20) : BE(b, body + 12);
            else if (type == "stts" && size >= 24 && BE(b, body + 4) > 0) t.Duration = BE(b, body + 12);   // first entry's sample duration
            else if (type == "stsz" && size >= 20) t.Samples = (int)BE(b, body + 8);
            else if (type == "stsd" && size >= 24)
            {
                t.Codec = Encoding.ASCII.GetString(b, body + 12, 4);
                // Z CAM's 'zraw' sample entry carries a 'zraw' child box whose first field is the version
                int entry = body + 8, entry_end = Math.Min(stop, entry + (int)BE(b, entry));
                for (int q = entry + 8; q + 12 <= entry_end; ++q)
                    if (b[q] == 'z' && b[q + 1] == 'r' && b[q + 2] == 'a' && b[q + 3] == 'w' && q > entry + 8)
                    {
                        t.Version = BE(b, q + 4);
                        if (BE(b, q - 4) > 16 && q + 16 <= entry_end) t.Scheme = BE(b, q + 12);
                        break;
                    }
            }
            p = stop;
        }
    }
}

// Tab strip in Z CAM colors: Metro's tab control only draws its own blue for the selected tab.
class TabStrip : Panel
{
    readonly List<string> titles = new List<string>();
    readonly List<Control> pages = new List<Control>();
    readonly Font font = new Font("Segoe UI Semibold", 10.5f);
    int hover = -1;
    public int Selected { get; private set; }
    public Panel Host = new Panel { Dock = DockStyle.Fill, BackColor = Z.Panel, Padding = new Padding(10, 8, 10, 10) };

    public TabStrip()
    {
        Dock = DockStyle.Top;
        Height = 38;
        BackColor = Z.Panel;
        DoubleBuffered = true;
        Cursor = Cursors.Hand;
        MouseMove += (s, e) => { int h = Hit(e.X); if (h != hover) { hover = h; Invalidate(); } };
        MouseLeave += (s, e) => { hover = -1; Invalidate(); };
        MouseDown += (s, e) => { int h = Hit(e.X); if (h >= 0) Select(h); };
    }

    public void Add(string title, Control page)
    {
        titles.Add(title);
        pages.Add(page);
        page.Dock = DockStyle.Fill;
        page.Visible = pages.Count == 1;
        Host.Controls.Add(page);
        Invalidate();
    }

    public void Select(int i)
    {
        Selected = i;
        for (int k = 0; k < pages.Count; k++) pages[k].Visible = k == i;
        Invalidate();
    }

    int TabWidth(int i) { return TextRenderer.MeasureText(titles[i], font).Width + 40; }

    int Hit(int x)
    {
        for (int i = 0, left = 10; i < titles.Count; left += TabWidth(i), i++)
            if (x >= left && x < left + TabWidth(i)) return i;
        return -1;
    }

    protected override void OnPaint(PaintEventArgs e)
    {
        using (var line = new Pen(Z.Line)) e.Graphics.DrawLine(line, 0, Height - 1, Width, Height - 1);
        for (int i = 0, left = 10; i < titles.Count; left += TabWidth(i), i++)
        {
            var r = new Rectangle(left, 0, TabWidth(i), Height - 1);
            if (i == hover && i != Selected) using (var b = new SolidBrush(Z.Row)) e.Graphics.FillRectangle(b, r);
            TextRenderer.DrawText(e.Graphics, titles[i], font, r, i == Selected ? Color.White : Z.Dim, TextFormatFlags.HorizontalCenter | TextFormatFlags.VerticalCenter);
            if (i == Selected) using (var b = new SolidBrush(Z.Red)) e.Graphics.FillRectangle(b, r.Left + 8, Height - 4, r.Width - 16, 3);
        }
    }
}

// A question in the app's own look: text, a "Don't show this again" box, and two buttons
class ConfirmForm : MetroForm
{
    const int W = 480;
    readonly ZCheck again = Z.Check("Don't show this again");
    public bool DontShowAgain { get { return again.Checked; } }

    public ConfirmForm(string title, string text, string yes)
    {
        Text = title;
        StartPosition = FormStartPosition.CenterParent;
        ShowInTaskbar = false;
        Z.Dress(this, true);
        AllowResize = false;
        var body = new Label { Text = text, ForeColor = Z.Text, BackColor = Z.Panel, Font = Z.UiFont, AutoSize = true, MaximumSize = new Size(W, 0), Margin = new Padding(0, 0, 0, 16), UseMnemonic = false };
        again.Size = new Size(W, 24);
        again.Margin = new Padding(0, 0, 0, 16);
        var ok = Z.Button(yes, true);
        var cancel = Z.Button("Cancel", false);
        ok.Click += (s, e) => { DialogResult = DialogResult.OK; Close(); };
        cancel.Click += (s, e) => { DialogResult = DialogResult.Cancel; Close(); };
        KeyPreview = true;
        KeyDown += (s, e) => { if (e.KeyCode == Keys.Escape) { DialogResult = DialogResult.Cancel; Close(); } };
        var flow = new FlowLayoutPanel { Dock = DockStyle.Fill, FlowDirection = FlowDirection.TopDown, WrapContents = false, BackColor = Z.Panel, Padding = new Padding(20, 12, 20, 12) };
        flow.Controls.Add(body);
        flow.Controls.Add(again);
        flow.Controls.Add(Z.Bar(DockStyle.None, ok, cancel));
        Controls.Add(flow);
        // Sized to the text: header 40, padding, text, checkbox, buttons
        ClientSize = new Size(W + 44, 40 + 12 + body.PreferredSize.Height + 16 + 24 + 16 + Z.RowHeight + 10 + 12 + 2);
    }
}

public class StudioForm : MetroForm
{
    readonly TabStrip tabs = new TabStrip();
    readonly ConvertPage convert = new ConvertPage();
    readonly TaggerPage tagger = new TaggerPage();
    readonly PlayerPage player = new PlayerPage();
    readonly AboutPage about = new AboutPage();

    public StudioForm(string[] args)
    {
        Text = "ZRAW Studio";
        Size = new Size(1280, 860);
        MinimumSize = new Size(1100, 800);
        StartPosition = FormStartPosition.CenterScreen;
        Z.Dress(this);

        tabs.Add("Convert", convert);
        tabs.Add("Tagger", tagger);
        tabs.Add("Player", player);
        tabs.Add("About", about);
        Controls.Add(tabs.Host);
        Controls.Add(tabs);

        // Drops anywhere go to whichever tab is showing
        foreach (var c in Z.All(this))
        {
            c.AllowDrop = true;
            c.DragEnter += (s, e) => e.Effect = e.Data.GetDataPresent(DataFormats.FileDrop) ? DragDropEffects.Copy : DragDropEffects.None;
            c.DragDrop += (s, e) =>
            {
                var paths = (string[])e.Data.GetData(DataFormats.FileDrop);
                if (tabs.Selected == 0) convert.Add(paths); else if (tabs.Selected == 1) tagger.Open(paths[0]); else player.Play(paths);
            };
        }
        // Files or folders dropped on the exe: RAW clips into Convert, their folder into the Tagger
        Shown += (s, e) =>
        {
            if (Settings.Get("updateCheck", "1") == "1") about.Check(convert.Write);
            if (args.Length == 0) return;
            convert.Add(args);
            tagger.Open(args[0]);
        };
        FormClosing += (s, e) => { if (!convert.ConfirmClose()) e.Cancel = true; else Settings.Save(); };
    }
}

static class Program
{
    static readonly Dictionary<string, Assembly> loaded = new Dictionary<string, Assembly>();

    [STAThread]
    static void Main(string[] args)
    {
        // Serve ReaLTaiizor from the gzip resource baked into this exe. Cached so it loads once.
        AppDomain.CurrentDomain.AssemblyResolve += (s, e) =>
        {
            string name = new AssemblyName(e.Name).Name;
            lock (loaded)
            {
                Assembly a;
                if (loaded.TryGetValue(name, out a)) return a;
                byte[] data = Embedded.Bytes(name + ".dll.gz");
                return data == null ? null : loaded[name] = Assembly.Load(data);
            }
        };
        Start(args);
    }

    // Separate method so ReaLTaiizor types are first touched after the resolver is hooked.
    [MethodImpl(MethodImplOptions.NoInlining)]
    static void Start(string[] args)
    {
        Application.EnableVisualStyles();
        Application.SetCompatibleTextRenderingDefault(false);
        // Unexpected errors go into the session log, so a problem report carries them
        Application.ThreadException += (s, e) =>
        {
            LogFile.Append("ERROR (unexpected): " + e.Exception);
            MessageBox.Show("Something went wrong:\n\n" + e.Exception.Message + "\n\nThe details are in the log. To find it, open the About tab and click Open log folder.",
                            "ZRAW Studio", MessageBoxButtons.OK, MessageBoxIcon.Error);
        };
        AppDomain.CurrentDomain.UnhandledException += (s, e) => LogFile.Append("ERROR (fatal): " + e.ExceptionObject);
        Application.Run(new StudioForm(args));
    }
}
