// Explorer add-on installer: registers ZrawIcon.dll (an embedded resource of this exe) for the
// current user.
using System;
using System.IO;
using System.Runtime.InteropServices;
using System.Security.Cryptography;
using Microsoft.Win32;

// Per-user Explorer icon, thumbnail and preview handlers (ZrawIcon.dll, embedded in this exe). No
// admin needed: everything lives under HKCU and %LOCALAPPDATA%\ZrawTagger. The DLL file name carries
// a content hash, so a newer build never has to overwrite a copy Explorer still has loaded.
static class IconHandler
{
    // The CLSIDs are also in ZrawIcon.cpp. Each handler is (class, registry slot, name). Icons work from
    // SystemFileAssociations, but Windows only looks for thumbnail and preview handlers on the extension key.
    static readonly string[,] Handlers =
    {
        { "{CDB31174-80BF-4A2F-9BCB-BA05B0F22683}", @"Software\Classes\SystemFileAssociations\.mov\ShellEx\IconHandler", "ZRAW Studio icon handler" },
        { "{D2B1645E-FE53-419C-A57A-CB81336F8A26}", @"Software\Classes\.mov\ShellEx\{e357fccd-a995-4576-b01f-234630154e96}", "ZRAW Studio thumbnail handler" },
        { "{0C3C2943-9325-417B-BB2A-C8500E5E0DA3}", @"Software\Classes\.mov\ShellEx\{8895b1c6-b41f-4c1c-a562-0d564250836f}", "ZRAW Studio preview handler" },
        // HEVC ZRAW clips from E2 firmware 1.0.9 are named .ZRAW
        { "{CDB31174-80BF-4A2F-9BCB-BA05B0F22683}", @"Software\Classes\SystemFileAssociations\.zraw\ShellEx\IconHandler", "ZRAW Studio icon handler" },
        { "{D2B1645E-FE53-419C-A57A-CB81336F8A26}", @"Software\Classes\.zraw\ShellEx\{e357fccd-a995-4576-b01f-234630154e96}", "ZRAW Studio thumbnail handler" },
        { "{0C3C2943-9325-417B-BB2A-C8500E5E0DA3}", @"Software\Classes\.zraw\ShellEx\{8895b1c6-b41f-4c1c-a562-0d564250836f}", "ZRAW Studio preview handler" },
    };
    const int Preview = 2;
    const string PrevHostAppId = "{6d2b5079-2f0b-48dd-ab7f-97cec514d30b}";   // Windows' prevhost.exe surrogate
    const string PreviewList = @"Software\Microsoft\Windows\CurrentVersion\PreviewHandlers";
    const string OldThumbSlot = @"Software\Classes\SystemFileAssociations\.mov\ShellEx\{e357fccd-a995-4576-b01f-234630154e96}"; // the thumbnail slot older builds used
    static readonly string Dir = Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData), "ZrawTagger");

    [DllImport("shell32.dll")] static extern void SHChangeNotify(int eventId, uint flags, IntPtr a, IntPtr b);
    static void Refresh() { SHChangeNotify(0x08000000, 0x1000, IntPtr.Zero, IntPtr.Zero); } // SHCNE_ASSOCCHANGED, SHCNF_FLUSH

    static string Slot(int i)
    {
        using (var k = Registry.CurrentUser.OpenSubKey(Handlers[i, 1])) return k == null ? null : k.GetValue("") as string;
    }
    static bool Ours(int i) { return Handlers[i, 0].Equals(Slot(i), StringComparison.OrdinalIgnoreCase); }

    static void RemoveOldThumbSlot()
    {
        bool ours;
        using (var k = Registry.CurrentUser.OpenSubKey(OldThumbSlot)) ours = k != null && Handlers[1, 0].Equals(k.GetValue("") as string, StringComparison.OrdinalIgnoreCase);
        if (ours) Registry.CurrentUser.DeleteSubKeyTree(OldThumbSlot, false);
    }

    public static bool Installed { get { for (int i = 0; i < Handlers.GetLength(0); i++) if (!Ours(i)) return false; return true; } }

    public static void Install(Action<string> log)
    {
        for (int i = 0; i < Handlers.GetLength(0); i++)
            if (!string.IsNullOrEmpty(Slot(i)) && !Ours(i)) { log("ERROR: another handler (" + Slot(i) + ") is already registered at HKCU\\" + Handlers[i, 1] + ". ZRAW Studio does not replace it, so the thumbnail engine was not installed."); return; }
        byte[] dll;
        using (var rs = typeof(IconHandler).Assembly.GetManifestResourceStream("ZrawIcon.dll"))
        using (var ms = new MemoryStream()) { rs.CopyTo(ms); dll = ms.ToArray(); }
        string hash;
        using (var sha = SHA1.Create()) hash = BitConverter.ToString(sha.ComputeHash(dll), 0, 4).Replace("-", "");
        string path = Path.Combine(Dir, "ZrawIcon-" + hash + ".dll");
        Directory.CreateDirectory(Dir);
        if (!File.Exists(path)) File.WriteAllBytes(path, dll);
        for (int i = 0; i < Handlers.GetLength(0); i++)
        {
            string cls = @"Software\Classes\CLSID\" + Handlers[i, 0];
            using (var k = Registry.CurrentUser.CreateSubKey(cls))
            {
                k.SetValue("", Handlers[i, 2]);
                if (i == Preview) { k.SetValue("AppID", PrevHostAppId); k.SetValue("DisplayName", Handlers[i, 2]); }
            }
            using (var k = Registry.CurrentUser.CreateSubKey(cls + @"\InprocServer32")) { k.SetValue("", path); k.SetValue("ThreadingModel", "Apartment"); }
            using (var k = Registry.CurrentUser.CreateSubKey(Handlers[i, 1])) k.SetValue("", Handlers[i, 0]);
        }
        using (var k = Registry.CurrentUser.CreateSubKey(PreviewList)) k.SetValue(Handlers[Preview, 0], Handlers[Preview, 2]);
        RemoveOldThumbSlot();
        Cleanup(path, log);
        Refresh();
        log("Installed: Explorer shows ZRAW clips with their own icon, thumbnail and Preview pane image (" + path + ")");
        log("If a window still shows old icons, press F5 in it or restart Explorer.");
    }

    public static void Uninstall(Action<string> log)
    {
        for (int i = 0; i < Handlers.GetLength(0); i++)
        {
            if (Ours(i)) Registry.CurrentUser.DeleteSubKeyTree(Handlers[i, 1], false);
            Registry.CurrentUser.DeleteSubKeyTree(@"Software\Classes\CLSID\" + Handlers[i, 0], false);
        }
        using (var k = Registry.CurrentUser.OpenSubKey(PreviewList, true)) if (k != null) k.DeleteValue(Handlers[Preview, 0], false);
        RemoveOldThumbSlot();
        Refresh();
        Cleanup(null, log);
        log("Removed: .MOV files show their normal icon, thumbnail and preview again.");
    }

    static void Cleanup(string keep, Action<string> log)
    {
        if (!Directory.Exists(Dir)) return;
        foreach (var f in Directory.GetFiles(Dir, "ZrawIcon-*.dll"))
        {
            if (f.Equals(keep, StringComparison.OrdinalIgnoreCase)) continue;
            try { File.Delete(f); }
            catch { log("Explorer is still using the old " + Path.GetFileName(f) + ". ZRAW Studio deletes it the next time you install or remove the thumbnail engine."); }
        }
    }
}
