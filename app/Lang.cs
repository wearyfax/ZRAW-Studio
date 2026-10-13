// The UI language: Russian when Windows is set to Russian, else English, unless the About tab says
// otherwise. Text is looked up by its English wording (the table is in Russian.cs), so the code still
// reads in English and a string missing from the table shows in English.
using System;
using System.Globalization;

static partial class L
{
    public static readonly string Code = Pick();
    public static readonly bool Ru = Code == "ru";
    // Numbers on screen: "3,0" and "1 234" in Russian. Engine arguments and settings stay invariant.
    public static readonly CultureInfo Culture = Ru ? new CultureInfo("ru-RU") : CultureInfo.InvariantCulture;

    static string Pick()
    {
        string s = Settings.Get("language", "");
        if (s == "en" || s == "ru") return s;
        return CultureInfo.CurrentUICulture.TwoLetterISOLanguageName == "ru" ? "ru" : "en";
    }

    public static string T(string en) { string s; return Ru && Russian.TryGetValue(en, out s) ? s : en; }

    // T, then string.Format. {0:clip|clips} picks a word by the number: two forms in English (1, other),
    // three in Russian (1 клип, 2 клипа, 5 клипов).
    public static string F(string en, params object[] args) { return string.Format(Plural.Instance, T(en), args); }

    class Plural : IFormatProvider, ICustomFormatter
    {
        public static readonly Plural Instance = new Plural();
        public object GetFormat(Type t) { return t == typeof(ICustomFormatter) ? this : null; }

        public string Format(string format, object arg, IFormatProvider provider)
        {
            if (format != null && format.IndexOf('|') >= 0)
            {
                var w = format.Split('|');
                long n = Math.Abs(Convert.ToInt64(arg)), d = n % 10, h = n % 100;
                if (w.Length == 2) return w[n == 1 ? 0 : 1];
                return w[d == 1 && h != 11 ? 0 : d >= 2 && d <= 4 && (h < 12 || h > 14) ? 1 : 2];
            }
            var f = arg as IFormattable;
            return f != null ? f.ToString(format, Culture) : arg == null ? "" : arg.ToString();
        }
    }
}
