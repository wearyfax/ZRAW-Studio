// Finds Z CAM ZRAW clips among H.265 .MOV files and prefixes them with ZRAW_.
// A clip is ZRAW when its video track's sample description fourcc is 'zraw' (H.265 clips use 'hvc1'),
// the same check zraw-parser makes. Z CAM writes moov after the video data; the scan seeks
// past mdat and reads only moov, so a folder of clips takes milliseconds.
using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using System.Text;

public class Clip { public string Path, Name, Kind, Status; }  // Name: path relative to the scanned folder

public static class Tagger
{
    public const string Prefix = "ZRAW_";
    // Clip.Status values, in English; the Tagger tab shows them translated
    public const string Done = "tagged", ToRename = "to rename", NotZraw = "WARNING: has a ZRAW_ name but is not ZRAW";

    public static bool Tagged(string name) { return name.StartsWith(Prefix, StringComparison.OrdinalIgnoreCase); }

    public static List<Clip> Scan(string dir, bool subfolders)
    {
        var clips = new List<Clip>();
        var files = new List<string>();
        Collect(dir, subfolders, files);
        string root = dir.TrimEnd('\\') + "\\";
        foreach (var p in files.OrderBy(p => p, StringComparer.OrdinalIgnoreCase))
        {
            var c = new Clip { Path = p, Name = p.Substring(root.Length), Kind = Classify(p) };
            bool z = c.Kind == "ZRAW", t = Tagged(Path.GetFileName(p));
            c.Status = z ? (t ? Done : ToRename) : (t ? NotZraw : "");
            clips.Add(c);
        }
        return clips;
    }

    // Skips folders it may not read (e.g. System Volume Information when a whole drive is dropped).
    static void Collect(string dir, bool subfolders, List<string> files)
    {
        try
        {
            files.AddRange(Directory.GetFiles(dir).Where(p => Path.GetExtension(p).Equals(".mov", StringComparison.OrdinalIgnoreCase)));
            if (subfolders) foreach (var d in Directory.GetDirectories(dir)) Collect(d, true, files);
        }
        catch (UnauthorizedAccessException) { }
    }

    // Renames happen in each file's own folder.
    public static int Rename(string dir, bool subfolders, Action<string> log)
    {
        int n = 0;
        foreach (var c in Scan(dir, subfolders).Where(c => c.Kind == "ZRAW" && !Tagged(Path.GetFileName(c.Path))))
            if (Move(c.Path, Path.Combine(Path.GetDirectoryName(c.Path), Prefix + Path.GetFileName(c.Path)), log)) n++;
        return n;
    }

    // Only strips the prefix from files whose contents are ZRAW, so a hand-named ZRAW_ file is left alone.
    public static int Undo(string dir, bool subfolders, Action<string> log)
    {
        int n = 0;
        foreach (var c in Scan(dir, subfolders).Where(c => c.Kind == "ZRAW" && Tagged(Path.GetFileName(c.Path))))
            if (Move(c.Path, Path.Combine(Path.GetDirectoryName(c.Path), Path.GetFileName(c.Path).Substring(Prefix.Length)), log)) n++;
        return n;
    }

    static bool Move(string from, string to, Action<string> log)
    {
        string a = Path.GetFileName(from), b = Path.GetFileName(to);
        if (File.Exists(to)) { log(L.F("SKIP {0}: {1} already exists", a, b)); return false; }
        try { File.Move(from, to); log(a + "  ->  " + b); return true; }
        catch (Exception e) { log(L.F("FAILED {0}: {1}", a, e.Message)); return false; }
    }

    // "ZRAW", "H.265" and "H.264" stay as they are; the rest is shown as is, so it is translated here
    public static string Classify(string path)
    {
        try
        {
            var moov = ReadMoov(path);
            if (moov == null) return L.T("unreadable");
            var codecs = new List<string>();
            Walk(moov, 0, moov.Length, codecs);
            if (codecs.Contains("zraw")) return "ZRAW";
            if (codecs.Contains("hvc1") || codecs.Contains("hev1")) return "H.265";
            if (codecs.Contains("avc1")) return "H.264";
            return L.F("other ({0})", string.Join(",", codecs));
        }
        catch (Exception e) { return L.F("unreadable ({0})", e.Message); }
    }

    // Walks top-level atoms by seeking (no data read) until moov, then loads just moov.
    public static byte[] ReadMoov(string path)
    {
        using (var f = new FileStream(path, FileMode.Open, FileAccess.Read, FileShare.ReadWrite, 4096))
        {
            var h = new byte[16];
            long pos = 0, end = f.Length;
            while (pos + 8 <= end)
            {
                f.Position = pos;
                if (f.Read(h, 0, 16) < 8) return null;
                long size = BE(h, 0, 4); int hdr = 8;
                if (size == 1) { size = BE(h, 8, 8); hdr = 16; }
                else if (size == 0) size = end - pos;
                if (size < hdr) return null;
                if (Encoding.ASCII.GetString(h, 4, 4) == "moov")
                {
                    if (size > (64 << 20) || pos + size > end) return null; // 64 MB cap; raise it if hour-long clips hit it
                    var b = new byte[size - hdr];
                    f.Position = pos + hdr;
                    int got = 0, r;
                    while (got < b.Length && (r = f.Read(b, got, b.Length - got)) > 0) got += r;
                    return got == b.Length ? b : null;
                }
                pos += size;
            }
        }
        return null;
    }

    // moov > trak > mdia > minf > stbl > stsd; records the first sample entry's fourcc per track.
    static void Walk(byte[] b, int start, int end, List<string> codecs)
    {
        int p = start;
        while (p + 8 <= end)
        {
            long size = BE(b, p, 4); int hdr = 8;
            if (size == 1) { size = BE(b, p + 8, 8); hdr = 16; }
            else if (size == 0) size = end - p;
            if (size < hdr || p + size > end) return;
            string type = Encoding.ASCII.GetString(b, p + 4, 4);
            if (type == "trak" || type == "mdia" || type == "minf" || type == "stbl") Walk(b, p + hdr, (int)(p + size), codecs);
            else if (type == "stsd" && size >= 24) codecs.Add(Encoding.ASCII.GetString(b, p + 20, 4));
            p += (int)size;
        }
    }

    static long BE(byte[] b, int o, int n) { long v = 0; for (int i = 0; i < n; i++) v = (v << 8) | b[o + i]; return v; }
}
