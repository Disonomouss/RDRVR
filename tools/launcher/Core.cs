// RDRVR Launcher: settings files, the game folder, starting the game (C# 5, .NET Framework 4.8, the in-box compiler).
//
// The mod reads the game folder's RDRVR.ini, then %LOCALAPPDATA%\RDRVR\RDRVR.user.ini over it (a key in the user file
// wins; both read once at the game's start: src/core/config.cpp). The launcher writes the user file the way the mod's
// own writer does (config::set): only the keys it changes, every other line and comment kept, a missing key inserted
// at its section's end or a new section appended, CRLF, no BOM, through a .tmp file and a replace. It never writes
// while RDR.exe runs (the mod's own writes would race it), except while the mod holds the game for it at its start
// (AtStart), and never touches the game's files.
using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.Globalization;
using System.IO;
using System.Linq;
using System.Text;
using System.Threading;
using Microsoft.Win32;

namespace RdrvrLauncher {

// One ini file as lines. Lookups follow the mod's parser (config.cpp parse_file): a line trimmed of spaces, tabs, CR
// and LF; ';' or '#' at its start a comment; sections and keys case-insensitive; an inline " ;" ends the value; the last
// occurrence of a key wins. The bytes are kept as the file has them: UTF-8, else the ANSI code page (audio.cpp reads a
// path that way), and a BOM stays part of the first line, as the mod reads it (so a "[XR]" behind a BOM is no section
// to either).
public class IniFile {
    public readonly string Path;
    public List<string> Lines = new List<string>();
    public bool Existed;
    public DateTime Stamp;  // the file's last write when read
    Encoding enc = new UTF8Encoding(false);

    public IniFile(string path) {
        Path = path;
        Existed = File.Exists(path);
        if (Existed) {
            Stamp = File.GetLastWriteTimeUtc(path);
            byte[] bytes = File.ReadAllBytes(path);
            string text;
            try {
                text = new UTF8Encoding(false, true).GetString(bytes);
            } catch (DecoderFallbackException) {
                enc = Encoding.Default;
                text = enc.GetString(bytes);
            }
            Lines = text.Replace("\r\n", "\n").Split('\n').ToList();
            if (Lines.Count > 0 && Lines[Lines.Count - 1].Length == 0) Lines.RemoveAt(Lines.Count - 1);
        }
    }

    static readonly char[] Ws = { ' ', '\t', '\r', '\n' };  // the mod's trim

    static string SectionOf(string line) {
        string t = line.Trim(Ws);
        if (t.Length >= 2 && t[0] == '[' && t[t.Length - 1] == ']') return t.Substring(1, t.Length - 2).Trim(Ws);
        return null;
    }

    static bool KeyValue(string line, out string key, out string value) {
        key = null;
        value = null;
        string t = line.Trim(Ws);
        if (t.Length == 0 || t[0] == ';' || t[0] == '#' || SectionOf(t) != null) return false;
        int eq = t.IndexOf('=');
        if (eq < 0) return false;
        key = t.Substring(0, eq).Trim(Ws);
        value = t.Substring(eq + 1).Trim(Ws);
        int c = value.IndexOf(" ;");
        if (c >= 0) value = value.Substring(0, c).Trim(Ws);
        return true;
    }

    public string Get(string section, string key) {
        string cur = null, found = null;
        foreach (string line in Lines) {
            string s = SectionOf(line);
            if (s != null) {
                cur = s;
                continue;
            }
            string k, v;
            if (cur != null && KeyValue(line, out k, out v) && string.Equals(cur, section, StringComparison.OrdinalIgnoreCase) &&
                string.Equals(k, key, StringComparison.OrdinalIgnoreCase))
                found = v;
        }
        return found;
    }

    // config::set's edit: replace the key's line in its section (the last one, as the parser keeps the last), else
    // insert it after the section's last key line, else append the section
    public void Set(string section, string key, string value) {
        string cur = null;
        int lastKey = -1, secHead = -1, secLastLine = -1, hit = -1;
        for (int i = 0; i < Lines.Count; ++i) {
            string s = SectionOf(Lines[i]);
            if (s != null) {
                cur = s;
                if (string.Equals(s, section, StringComparison.OrdinalIgnoreCase)) {
                    secHead = i;
                    secLastLine = i;
                }
                continue;
            }
            if (cur != null && string.Equals(cur, section, StringComparison.OrdinalIgnoreCase)) {
                string k, v;
                if (KeyValue(Lines[i], out k, out v)) {
                    lastKey = i;
                    if (string.Equals(k, key, StringComparison.OrdinalIgnoreCase)) hit = i;
                }
                if (Lines[i].Trim().Length > 0) secLastLine = i;
            }
        }
        string line = key + "=" + value;
        if (hit >= 0) {
            Lines[hit] = line;
        } else if (secHead >= 0) {
            Lines.Insert((lastKey >= 0 ? lastKey : secLastLine) + 1, line);
        } else {
            if (Lines.Count > 0 && Lines[Lines.Count - 1].Trim().Length > 0) Lines.Add("");
            Lines.Add("[" + section + "]");
            Lines.Add(line);
        }
    }

    public void Save() {
        if (Lines.Count == 0 || !Lines[0].TrimStart().StartsWith(";"))
            Lines.Insert(0, "; RDRVR user settings (your own; never replaced by a release or a test).");
        string dir = System.IO.Path.GetDirectoryName(Path);
        if (!Directory.Exists(dir)) Directory.CreateDirectory(dir);
        string tmp = Path + ".tmp";
        File.WriteAllBytes(tmp, enc.GetBytes(string.Join("\r\n", Lines) + "\r\n"));
        if (File.Exists(Path)) File.Replace(tmp, Path, null);
        else File.Move(tmp, Path);
        Existed = true;
        Stamp = File.GetLastWriteTimeUtc(Path);
    }
}

// A value read as the mod reads it (config.cpp get_bool, get_int, get_float)
public static class ModValue {
    // get_bool: 1, true, yes or on, any case
    public static bool Bool(string v) {
        string l = (v ?? "").ToLowerInvariant();
        return l == "1" || l == "true" || l == "yes" || l == "on";
    }

    // get_int: strtol base 0 (0x hex, a leading 0 octal), what parses of the start; nothing parsed: 0
    public static long Int(string v) {
        string s = (v ?? "").TrimStart(' ', '\t');
        int i = 0, sign = 1;
        if (i < s.Length && (s[i] == '+' || s[i] == '-')) sign = s[i++] == '-' ? -1 : 1;
        int b = 10;
        if (i + 1 < s.Length && s[i] == '0' && (s[i + 1] == 'x' || s[i + 1] == 'X') && i + 2 < s.Length && Uri.IsHexDigit(s[i + 2])) {
            b = 16;
            i += 2;
        } else if (i < s.Length && s[i] == '0') {
            b = 8;
        }
        long r = 0;
        for (; i < s.Length; ++i) {
            int d = s[i] >= '0' && s[i] <= '9' ? s[i] - '0' : s[i] >= 'a' && s[i] <= 'f' ? s[i] - 'a' + 10 : s[i] >= 'A' && s[i] <= 'F' ? s[i] - 'A' + 10 : 99;
            if (d >= b) break;
            r = r * b + d;
            if (r > int.MaxValue) return sign * (long)int.MaxValue;
        }
        return sign * r;
    }

    // get_float: strtof, what parses of the start (inf and nan too); nothing parsed: 0
    public static double Float(string v) {
        System.Text.RegularExpressions.Match m = System.Text.RegularExpressions.Regex.Match(v ?? "",
            @"^[ \t]*([+-]?)(inf(inity)?|nan|(\d+\.?\d*|\.\d+)([eE][+-]?\d+)?)", System.Text.RegularExpressions.RegexOptions.IgnoreCase);
        if (!m.Success) return 0;
        string n = m.Groups[2].Value.ToLowerInvariant();
        double r = n.StartsWith("inf") ? double.PositiveInfinity : n == "nan" ? double.NaN : double.Parse(n, NumberStyles.Float, CultureInfo.InvariantCulture);
        return m.Groups[1].Value == "-" ? -r : r;
    }
}

public static class Game {
    public const string SteamAppId = "2668510";

    public static string UserIniPath() {
        return Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData), "RDRVR", "RDRVR.user.ini");
    }

    static bool IsGameDir(string d) {
        return !string.IsNullOrEmpty(d) && File.Exists(Path.Combine(d, "RDR.exe"));
    }

    // The folder above the launcher's own when it sits in the game folder's RDRVR_Launcher subfolder (the installer's
    // place for it), or its own folder (an older layout), else Steam's libraries (libraryfolders.vdf), else the Rockstar
    // Games Launcher's install key: as tools/installer/install.ps1 finds it
    public static string FindGameDir(out bool steam) {
        steam = false;
        string own = AppDomain.CurrentDomain.BaseDirectory.TrimEnd('\\');
        string parent = Path.GetDirectoryName(own);
        if (!IsGameDir(own) && IsGameDir(parent)) own = parent;
        string steamPath = null;
        try {
            using (RegistryKey k = Registry.CurrentUser.OpenSubKey(@"Software\Valve\Steam")) {
                if (k != null) steamPath = k.GetValue("SteamPath") as string;
            }
        } catch (Exception) {
        }
        List<string> libs = new List<string>();
        if (!string.IsNullOrEmpty(steamPath)) {
            libs.Add(steamPath.Replace('/', '\\'));
            string vdf = Path.Combine(steamPath, "steamapps", "libraryfolders.vdf");
            try {
                if (File.Exists(vdf))
                    foreach (string l in File.ReadAllLines(vdf)) {
                        string t = l.Trim();
                        if (!t.StartsWith("\"path\"")) continue;
                        string[] parts = t.Split('"');
                        if (parts.Length >= 4) libs.Add(parts[3].Replace("\\\\", "\\"));
                    }
            } catch (Exception) {
            }
        }
        if (IsGameDir(own)) {
            foreach (string lib in libs)
                if (own.StartsWith(lib.TrimEnd('\\') + "\\", StringComparison.OrdinalIgnoreCase)) steam = true;
            return own;
        }
        foreach (string lib in libs) {
            string d = Path.Combine(lib, "steamapps", "common", "Red Dead Redemption");
            if (IsGameDir(d)) {
                steam = true;
                return d;
            }
        }
        foreach (string key in new[] { @"SOFTWARE\WOW6432Node\Rockstar Games\Red Dead Redemption", @"SOFTWARE\Rockstar Games\Red Dead Redemption" }) {
            try {
                using (RegistryKey k = Registry.LocalMachine.OpenSubKey(key)) {
                    string d = k != null ? k.GetValue("InstallFolder") as string : null;
                    if (IsGameDir(d)) return d.TrimEnd('\\');
                }
            } catch (Exception) {
            }
        }
        return null;
    }

    public static bool Running() {
        return Process.GetProcessesByName("RDR").Length > 0 || Process.GetProcessesByName("PlayRDR").Length > 0;
    }

    // The note the mod reads at the game's start (src/core/launcher_start.cpp handed_off): "started <UTC unix seconds>
    // pid <n>", written just before this launcher starts the game itself, so the mod does not open a second launcher
    // (Steam and PlayRDR.exe pass nothing on to RDR.exe). The mod deletes it; it counts for 300 s
    public static string NotePath(string userIni = null) {
        return Path.Combine(Path.GetDirectoryName(userIni ?? UserIniPath()), "RDRVR.launcher_started");
    }

    public static void WriteNote(string path) {
        Directory.CreateDirectory(Path.GetDirectoryName(path));
        long t = (long)(DateTime.UtcNow - new DateTime(1970, 1, 1, 0, 0, 0, DateTimeKind.Utc)).TotalSeconds;
        File.WriteAllBytes(path, Encoding.ASCII.GetBytes("started " + t.ToString(CultureInfo.InvariantCulture) + " pid " +
                                                         Process.GetCurrentProcess().Id.ToString(CultureInfo.InvariantCulture) + "\r\n"));
    }

    // Steam's own start (as the harness does), or the Rockstar Games Launcher's bootstrap in the game folder. Never
    // -width / -height: the mod's render resolution refuses to arm with them. The note first (removed if no start)
    public static string Start(string gameDir, bool steam) {
        string note = NotePath();
        try {
            WriteNote(note);
        } catch (Exception) {
        }
        try {
            if (steam) {
                Process.Start("steam://rungameid/" + SteamAppId);
                return "started through Steam";
            }
            string play = gameDir != null ? Path.Combine(gameDir, "PlayRDR.exe") : null;
            if (play != null && File.Exists(play)) {
                Process.Start(new ProcessStartInfo(play) { WorkingDirectory = gameDir, UseShellExecute = true });
                return "started through the Rockstar Games Launcher";
            }
            throw new InvalidOperationException("the game was not found (no Steam library with it, no Rockstar install key)");
        } catch (Exception) {
            try {
                File.Delete(note);
            } catch (Exception) {
            }
            throw;
        }
    }
}

// The at-start mode (research/run30/launcher.md): the mod opened this launcher from RDR.exe ("--at-start <pid>
// <timeout>") and holds the game at its device init until this process exits; then it reads the user file again. Exit
// codes the mod logs: 0 Play (saved), 1 closed (an older launcher's, or ended in Task Manager: the game goes on), 2 the
// countdown ran out, 3 the game had gone on, 4 the game was gone, 6 closed by the user (2026-11-02: the start cancelled).
// The mod's events: Up (set here at once: an older launcher never sets it), Hold (set here when the user stops the
// countdown: the mod then waits up to 5 minutes) and Gone
// (set by the mod when it went on: from then on this is the normal launcher, saving refused while the game runs)
public static class AtStart {
    public static int Pid;           // the held game's process id; 0: not at the start (or the game went on)
    public static int Timeout = 30;  // the countdown in seconds; 0: none
    public static int ExitCode = 1;
    public static bool Linked;       // the mod's events were found
    static EventWaitHandle hold, gone;

    public static bool Active {
        get { return Pid != 0; }
    }

    public static void Open(int pid, int timeout) {
        Pid = pid;
        Timeout = timeout <= 0 ? 0 : Math.Max(5, Math.Min(300, timeout));
        try {
            hold = EventWaitHandle.OpenExisting(@"Local\RDRVR_LauncherHold_" + pid.ToString(CultureInfo.InvariantCulture));
            gone = EventWaitHandle.OpenExisting(@"Local\RDRVR_LauncherGone_" + pid.ToString(CultureInfo.InvariantCulture));
            using (EventWaitHandle up = EventWaitHandle.OpenExisting(@"Local\RDRVR_LauncherUp_" + pid.ToString(CultureInfo.InvariantCulture))) up.Set();
            Linked = true;
        } catch (Exception) {
            hold = gone = null;
            Linked = false;
        }
    }

    public static bool Gone() {
        try {
            return gone != null && gone.WaitOne(0);
        } catch (Exception) {
            return false;
        }
    }

    // by the process list (no handle on the game is opened)
    public static bool GameAlive() {
        if (Pid == 0) return false;
        try {
            using (Process p = Process.GetProcessById(Pid)) return true;
        } catch (ArgumentException) {
            return false;
        } catch (InvalidOperationException) {
            return false;
        }
    }

    // saving is safe while the game is held: the mod reads the user file again only once this window has closed
    public static bool Holding() {
        return Active && Linked && !Gone() && GameAlive();
    }

    public static void Hold() {
        try {
            if (hold != null) hold.Set();
        } catch (Exception) {
        }
    }

    public static void Leave() {
        Pid = 0;
    }
}

// The settings as the game will see them: the user file's value, else the game folder's RDRVR.ini, else the
// schema's default; and the launcher's edits, written to the user file on Save
public class Settings {
    public IniFile User, Shipped;
    public readonly Dictionary<string, string> Edits = new Dictionary<string, string>(StringComparer.OrdinalIgnoreCase);
    public event Action<string, string> Changed;  // (section/key, value): every control bound to it refreshes

    public static string Id(string section, string key) {
        return section + "/" + key;
    }

    // config.cpp's settings reset: a shipped [Compat] SettingsReset tag that the user file does not carry means the
    // mod moves that file aside at the next start and begins from the release's defaults. The launcher shows those
    // defaults (the user file's values would not apply) and, on Save, does the same move first
    public string ResetTag;
    public bool ResetPending;
    public string GameDir, UserPath;

    public void Load(string gameDir, string userPath = null) {  // userPath: the self-test's copy
        GameDir = gameDir;
        UserPath = userPath ?? Game.UserIniPath();
        Edits.Clear();
        Reload();
    }

    // the files as they are now (the game may have written the user file since, or a release the game folder's);
    // the edits are kept
    public void Reload() {
        User = new IniFile(UserPath);
        Shipped = GameDir != null ? new IniFile(Path.Combine(GameDir, "RDRVR.ini")) : null;
        ResetTag = Shipped != null ? Shipped.Get("Compat", "SettingsReset") : null;
        ResetPending = !string.IsNullOrEmpty(ResetTag) && User.Existed && User.Get("Compat", "SettingsReset") != ResetTag;
    }

    // true when the user file or the game folder's changed on disk since they were read
    public bool ChangedOnDisk() {
        Func<IniFile, bool> moved = f => f != null && (File.Exists(f.Path) != f.Existed || (f.Existed && File.GetLastWriteTimeUtc(f.Path) != f.Stamp));
        return moved(User) || moved(Shipped) || (Shipped == null && GameDir != null);
    }

    // the value without the launcher's edit
    public string Saved(string section, string key) {
        string v = User != null && !ResetPending ? User.Get(section, key) : null;
        if (v == null && Shipped != null) v = Shipped.Get(section, key);
        return v;
    }

    public string Get(string section, string key, string fallback) {
        string v;
        if (Edits.TryGetValue(Id(section, key), out v)) return v;
        return Saved(section, key) ?? fallback;
    }

    // an edit back to the saved value is no edit
    public void Set(string section, string key, string value) {
        string id = Id(section, key);
        if (Get(section, key, null) == value) return;
        if (Saved(section, key) == value) Edits.Remove(id);
        else Edits[id] = value;
        if (Changed != null) Changed(id, value);
    }

    public int Pending {
        get { return Edits.Count; }
    }

    // The settings reset (config.cpp): a pending one is done here as the mod would do it at the start (the old file
    // kept as RDRVR.user.ini.before-<tag>[.n].bak), and the new file carries the tag, so the mod does not move the
    // launcher's file aside
    public string Save() {
        if (Game.Running() && !AtStart.Holding()) throw new InvalidOperationException("the game is running: close it first (the mod writes this file too)");
        Reload();  // the edits go onto the file as it is now, as config::set does: what the game wrote meanwhile is kept
        // the game went on meanwhile (its deadline): it may be reading the file now, so nothing on disk is changed
        if (AtStart.Active && !AtStart.Holding()) throw new InvalidOperationException("the game went on meanwhile: saved nothing (close it first)");
        string note = "";
        if (ResetPending) {
            string bak = User.Path + ".before-" + ResetTag + ".bak";
            for (int i = 1; File.Exists(bak) && i < 100; ++i) bak = User.Path + ".before-" + ResetTag + "." + i + ".bak";
            File.Move(User.Path, bak);
            User = new IniFile(User.Path);
            ResetPending = false;
            note = "; the old settings kept as " + Path.GetFileName(bak);
        }
        if (!string.IsNullOrEmpty(ResetTag) && User.Get("Compat", "SettingsReset") != ResetTag) User.Set("Compat", "SettingsReset", ResetTag);
        foreach (KeyValuePair<string, string> e in Edits) {
            int slash = e.Key.IndexOf('/');
            User.Set(e.Key.Substring(0, slash), e.Key.Substring(slash + 1), e.Value);
        }
        int n = Edits.Count;
        // the game went on meanwhile (its deadline): it may be reading the file now
        if (AtStart.Active && !AtStart.Holding()) throw new InvalidOperationException("the game went on meanwhile: saved nothing (close it first)");
        User.Save();
        Edits.Clear();
        return n + " setting" + (n == 1 ? "" : "s") + " saved to " + User.Path + note;
    }
}
}
