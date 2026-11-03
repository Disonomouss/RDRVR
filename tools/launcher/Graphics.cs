// RDRVR Launcher: the render resolution picker and the presets, as the in-headset menu's Display tab writes them
// (src/core/render_res.cpp set_choice / size_of, src/core/menu.cpp's presets); and the ini writer's self-test.
using System;
using System.Collections.Generic;
using System.Drawing;
using System.Globalization;
using System.IO;
using System.Linq;
using System.Windows.Forms;

namespace RdrvrLauncher {

public static class Resolution {
    // render_res.cpp kHeadsets: [Render] RenderHeadset's key, the name, the eye height
    public static readonly string[,] Headsets = {
        { "index", "Valve Index", "1600" }, { "quest2", "Quest 2", "1920" }, { "quest3s", "Quest 3S", "1920" }, { "questpro", "Quest Pro", "1920" },
        { "psvr2", "PlayStation VR2", "2040" }, { "pico4", "Pico 4", "2160" }, { "reverbg2", "HP Reverb G2", "2160" }, { "steamframe", "Steam Frame", "2160" },
        { "quest3", "Quest 3", "2208" }, { "vivepro2", "Vive Pro 2", "2448" }, { "focus3", "Vive Focus 3", "2448" }, { "beyond", "Bigscreen Beyond", "2560" },
        { "crystal", "Pimax Crystal", "2880" }, { "dreamair", "Pimax Dream Air", "3552" }, { "galaxyxr", "Galaxy XR", "3840" },
    };
    public static readonly int[] Scales = { 100, 150, 200 };

    // size_of for a headset: H x sqrt(scale), rounded to even; W = 2 x round(H x 8/9); 640x480 .. 9998x5624, else none
    public static bool Size(int headset, int pct, out int w, out int h) {
        int eye = int.Parse(Headsets[headset, 2], CultureInfo.InvariantCulture);
        h = 2 * (int)Math.Round(eye * Math.Sqrt(pct / 100.0) / 2.0, MidpointRounding.AwayFromZero);
        w = 2 * (int)Math.Round(h * 8.0 / 9.0, MidpointRounding.AwayFromZero);
        return w >= 640 && h >= 480 && w <= 9998 && h <= 5624;
    }

    // set_choice: RenderHeadset, RenderScale and RenderResolution (the height, or off / auto)
    public static void Write(Settings s, int choice, int pct) {
        if (choice == 0) {
            s.Set("Render", "RenderHeadset", "off");
            s.Set("Render", "RenderScale", "100");
            s.Set("Render", "RenderResolution", "off");
        } else if (choice == 1) {
            s.Set("Render", "RenderHeadset", "auto");
            s.Set("Render", "RenderScale", pct.ToString(CultureInfo.InvariantCulture));
            s.Set("Render", "RenderResolution", "auto");
        } else {
            int w, h;
            bool ok = Size(choice - 2, pct, out w, out h);
            s.Set("Render", "RenderHeadset", Headsets[choice - 2, 0]);
            s.Set("Render", "RenderScale", pct.ToString(CultureInfo.InvariantCulture));
            s.Set("Render", "RenderResolution", ok ? h.ToString(CultureInfo.InvariantCulture) : "off");
        }
    }

    public static int WidthOf(int h) {
        return 2 * (int)Math.Round(h * 8.0 / 9.0, MidpointRounding.AwayFromZero);
    }

    // render_res.cpp load_choice: RenderResolution decides the size (off / auto / a height / WxH); RenderHeadset and
    // RenderScale name the headset and scale shown when they give exactly that size, else the headset of that height
    // at 100 %, else a custom size (-1; cw x ch, 0 x 0 when it does not parse)
    public static int Choice(Settings s, out int pct, out int cw, out int ch) {
        cw = ch = 0;
        string rr = (s.Get("Render", "RenderResolution", "off") ?? "").ToLowerInvariant();
        string hk = (s.Get("Render", "RenderHeadset", "") ?? "").ToLowerInvariant();
        long sp = ModValue.Int(s.Get("Render", "RenderScale", "100"));
        int sc = Scales.Contains((int)sp) ? (int)sp : 100;
        pct = 100;
        if (rr == "" || rr == "off" || rr == "0") return 0;
        if (rr == "auto") {
            pct = sc;
            return 1;
        }
        int w = 0, h = 0;
        System.Text.RegularExpressions.Match m = System.Text.RegularExpressions.Regex.Match(rr, @"^[ \t]*(\d+)[ \t]*[xX][ \t]*(\d+)[ \t]*$");
        if (m.Success && int.TryParse(m.Groups[1].Value, out w) && int.TryParse(m.Groups[2].Value, out h) && w > 0 && h > 0) {
        } else if (rr.Length > 0 && rr.All(char.IsDigit) && int.TryParse(rr, out h)) {
            w = WidthOf(h);
        } else {
            return -1;  // refused by the mod
        }
        for (int i = 0; i < Headsets.GetLength(0); ++i) {
            int hw, hh;
            if (Headsets[i, 0] == hk && Size(i, sc, out hw, out hh) && hw == w && hh == h) {
                pct = sc;
                return 2 + i;
            }
        }
        for (int i = 0; i < Headsets.GetLength(0); ++i)
            if (int.Parse(Headsets[i, 2], CultureInfo.InvariantCulture) == h && w == WidthOf(h)) return 2 + i;
        cw = w;
        ch = h;
        return -1;
    }

    public static void AddRows(MainForm form, TableLayoutPanel t, Settings s, ToolTip tips) {
        ComboBox head = new WheelCombo { DropDownStyle = ComboBoxStyle.DropDownList, Width = 340 };
        ComboBox size = new WheelCombo { DropDownStyle = ComboBoxStyle.DropDownList, Width = 340 };
        Label info = new Label { AutoSize = true, ForeColor = SystemColors.GrayText, MaximumSize = new Size(760, 0), Margin = new Padding(0, 2, 0, 8) };
        head.Items.Add("The game's own (no change)");
        head.Items.Add("Automatic: what the headset asks for");
        for (int i = 0; i < Headsets.GetLength(0); ++i) head.Items.Add(Headsets[i, 1]);
        foreach (int p in Scales) size.Items.Add(p + "% (" + (p == 100 ? "the headset's size" : p == 150 ? "sharper" : "sharpest, heavy") + ")");
        bool busy = false;
        Action refresh = () => {
            busy = true;
            int pct, cw, ch;
            int c = Choice(s, out pct, out cw, out ch);
            string rr = s.Get("Render", "RenderResolution", "off");
            while (head.Items.Count > Headsets.GetLength(0) + 2) head.Items.RemoveAt(head.Items.Count - 1);
            if (c < 0) {
                head.Items.Add("Custom (RenderResolution=" + rr + ")");
                head.SelectedIndex = head.Items.Count - 1;
            } else {
                head.SelectedIndex = c;
            }
            size.SelectedIndex = Array.IndexOf(Scales, pct);
            size.Enabled = c != 0;
            int w, h;
            info.Text = c >= 2 && Size(c - 2, pct, out w, out h) ? ("Renders " + w + " x " + h + ".")
                      : c == 1 ? "The height your headset's runtime asks for, capped by AutoMaxHeight (" + s.Get("Render", "AutoMaxHeight", "0") + "; 0 = none)."
                      : c == 0 ? "The game's own window size."
                      : ch > 0 ? "Renders " + cw + " x " + ch + " (RenderResolution=" + rr + ", set by hand)."
                               : "RenderResolution=" + rr + " is not a size the mod takes: the game's own size.";
            info.Text += " Applies at the next start. Windowed mode only; it needs at least 2.5 GB of video memory.";
            busy = false;
        };
        EventHandler changed = (o, e) => {
            if (busy) return;
            int c = head.SelectedIndex;
            if (c < 0 || c > Headsets.GetLength(0) + 1) return;
            Write(s, c, Scales[Math.Max(0, size.SelectedIndex)]);
        };
        head.SelectedIndexChanged += changed;
        size.SelectedIndexChanged += changed;
        refresh();
        form.ResolutionRefresh += refresh;
        s.Changed += (id, v) => {
            if (id.StartsWith("Render/Render", StringComparison.OrdinalIgnoreCase) || id.StartsWith("Render/AutoMaxHeight", StringComparison.OrdinalIgnoreCase)) refresh();
        };
        t.Controls.Add(new Label { Text = "Headset  (at the next start)", AutoSize = true, Margin = new Padding(0, 6, 8, 0) }, 0, t.RowCount);
        t.Controls.Add(head, 1, t.RowCount);
        t.RowCount++;
        t.Controls.Add(new Label { Text = "Size  (at the next start)", AutoSize = true, Margin = new Padding(0, 6, 8, 0) }, 0, t.RowCount);
        t.Controls.Add(size, 1, t.RowCount);
        t.RowCount++;
        t.Controls.Add(info, 1, t.RowCount);
        t.RowCount++;
        tips.SetToolTip(head, "The size the game renders at, for your headset's resolution (the headset menu's Display tab writes the same).");
    }
}

// Single pass (GOAL-single-pass-default.md item 3, 2026-10-31): [Stereo] SinglePass, 1 by default. On, config.cpp supplies
// the single-pass stack's keys wherever no file sets them, the shaders converted from the player's own game files by
// RDRVR_Converter\rdrvr_vi_convert.exe at the first start (InstancedShadersAuto), or taken from a folder of converted
// shaders when one is chosen ([Stereo] InstancedShaders: vs_*.dxil, ps_*.dxil and twins.txt; its instprev<K> lines name
// PrepassInstPrevK, pso.cpp load_twin_sources). Off (0): two-pass, the stack's master keys off whatever the files say.
public static class SinglePass {
    // the stack's values (config.cpp kSinglePassStack); InstancedShaders, PrepassInstPrevMirror and PrepassInstPrevK come
    // from a folder when one is chosen
    public static readonly string[,] On = {
        { "PrepassArrays", "1" }, { "PrepassViews", "2" }, { "PrepassUnionCull", "1" }, { "PrepassInstanced", "1" }, { "PrepassWalkCut", "1" },
        { "PrepassWvpMirror", "1" }, { "PrepassCutVelocity", "4" }, { "PrepassCutHoldStarted", "1" }, { "PrepassCutEmptyDraws", "1" },
    };

    static bool ReadsOff(string v) {
        return v != null && !ModValue.Bool(v) && ModValue.Int(v) == 0;
    }

    // null when the folder holds twins; k: its instprev<K> lines' K (0 when it has none, or they disagree: -1)
    public static string Problem(string dir, out int shaders, out int k) {
        shaders = 0;
        k = 0;
        if (string.IsNullOrEmpty(dir) || !Directory.Exists(dir)) return "the folder is not there";
        shaders = Directory.GetFiles(dir, "vs_*.dxil").Length + Directory.GetFiles(dir, "ps_*.dxil").Length;
        string tw = Path.Combine(dir, "twins.txt");
        if (shaders == 0 || !File.Exists(tw)) return "no converted shaders in it (vs_*.dxil and twins.txt)";
        foreach (string line in File.ReadAllLines(tw)) {
            if (line.StartsWith("#")) continue;
            foreach (string w in line.Split(new[] { ' ', '\t' }, StringSplitOptions.RemoveEmptyEntries).Skip(2)) {
                int n;
                if (!w.StartsWith("instprev") || !int.TryParse(w.Substring(8), NumberStyles.None, CultureInfo.InvariantCulture, out n)) continue;
                if (k == 0) k = n;
                else if (k != n) k = -1;
            }
        }
        if (k < 0 || k > 768) return "its twins.txt names more than one instprev size, or one past 768";
        return null;
    }

    public static bool IsOn(Settings s) {
        return s.Get("Stereo", "SinglePass", "1") != "0";
    }

    // the converter the mod runs when no folder is chosen (vi_auto.cpp's default: the game folder's RDRVR_Converter\)
    public static bool ConverterInstalled(Settings s) {
        string exe = s.Get("Stereo", "InstancedShadersConverter", "");
        if (string.IsNullOrEmpty(exe)) {
            if (s.GameDir == null) return false;
            exe = Path.Combine(s.GameDir, "RDRVR_Converter", "rdrvr_vi_convert.exe");
        }
        string dxc = s.Get("Stereo", "InstancedShadersDxc", "");
        if (string.IsNullOrEmpty(dxc)) dxc = Path.Combine(Path.GetDirectoryName(exe) ?? "", "rdrvr_dxcompiler.dll");
        return File.Exists(exe) && File.Exists(dxc);
    }

    public static void TurnOn(Settings s) {
        s.Set("Stereo", "SinglePass", "1");
        // an older launcher's off values (it wrote each key's 0): back to the stack's, or they would win over the switch
        for (int i = 0; i < On.GetLength(0); ++i)
            if (ReadsOff(s.Get("Stereo", On[i, 0], null))) s.Set("Stereo", On[i, 0], On[i, 1]);
        // (the review) its untick also wrote PrepassInstPrevMirror=0, and a folder's InstancedShadersAuto=0: without a
        // folder both back to the stack's (a folder's twins.txt decides the mirror)
        if (string.IsNullOrEmpty(s.Get("Stereo", "InstancedShaders", null))) {
            if (ReadsOff(s.Get("Stereo", "PrepassInstPrevMirror", null))) s.Set("Stereo", "PrepassInstPrevMirror", "1");
            if (ReadsOff(s.Get("Stereo", "InstancedShadersAuto", null))) s.Set("Stereo", "InstancedShadersAuto", "1");
        }
    }

    public static void TurnOff(Settings s) {
        s.Set("Stereo", "SinglePass", "0");
    }

    // dir: a folder Problem() passed (null: none, the shaders converted on this PC)
    public static void UseFolder(Settings s, string dir) {
        if (dir == null) {
            if (!string.IsNullOrEmpty(s.Get("Stereo", "InstancedShaders", null))) s.Set("Stereo", "InstancedShaders", "");
            // (the review) a folder's choice wrote InstancedShadersAuto=0: the conversion on this PC needs it on again
            if (ReadsOff(s.Get("Stereo", "InstancedShadersAuto", null))) s.Set("Stereo", "InstancedShadersAuto", "1");
            if (ReadsOff(s.Get("Stereo", "PrepassInstPrevMirror", null))) s.Set("Stereo", "PrepassInstPrevMirror", "1");
            if (s.Get("Stereo", "PrepassInstPrevK", null) != null && s.Get("Stereo", "PrepassInstPrevK", null) != "64") s.Set("Stereo", "PrepassInstPrevK", "64");  // (the measured stack's)
            return;
        }
        int n, k;
        Problem(dir, out n, out k);
        s.Set("Stereo", "InstancedShaders", dir);
        s.Set("Stereo", "PrepassInstPrevMirror", k > 0 ? "1" : "0");
        if (k > 0) s.Set("Stereo", "PrepassInstPrevK", k.ToString(CultureInfo.InvariantCulture));
        if (ModValue.Bool(s.Get("Stereo", "InstancedShadersAuto", null))) s.Set("Stereo", "InstancedShadersAuto", "0");  // (pso.cpp: one source of twins)
    }

    public static void AddRows(MainForm form, TableLayoutPanel t, Settings s, ToolTip tips) {
        CheckBox cb = new CheckBox { AutoSize = true, Margin = new Padding(0, 6, 0, 0) };
        Label folder = new Label { AutoSize = true, MaximumSize = new Size(600, 0), Margin = new Padding(0, 8, 8, 0) };
        // (2026-10-31, the user: "Use a folder" hidden: a hand-made folder is a test aid, [Stereo] InstancedShaders in the ini
        // only) the button that leaves a folder named in the ini, shown only then
        Button clear = new Button { Text = "Convert on this PC", AutoSize = true };
        FlowLayoutPanel fr = new FlowLayoutPanel { AutoSize = true, WrapContents = false };
        fr.Controls.Add(clear);
        fr.Controls.Add(folder);
        Label info = new Label { AutoSize = true, ForeColor = SystemColors.GrayText, MaximumSize = new Size(760, 0), Margin = new Padding(0, 2, 0, 8) };
        bool busy = false;
        Action refresh = () => {
            busy = true;
            bool on = IsOn(s);
            cb.Checked = on;
            string dir = s.Get("Stereo", "InstancedShaders", "");
            int n, k;
            string why = string.IsNullOrEmpty(dir) ? null : Problem(dir, out n, out k);
            Problem(dir, out n, out k);
            bool conv = ConverterInstalled(s);
            folder.Text = string.IsNullOrEmpty(dir) ? (conv ? "converted on this PC at the first start" : "converted on this PC: the converter is not installed (two-pass)")
                        : "the folder named in your ini: " + dir + (why == null ? "  (" + n + " shaders)" : "  (" + why + ")");
            clear.Visible = !string.IsNullOrEmpty(dir);
            info.Text = "Draws both eyes from one walk of the scene instead of one each: about a millisecond a frame faster. Its shaders are converted "
                      + "from your own game files in the background at the first start (until one is ready, what uses it is drawn as two passes). "
                      + "Unticked: two-pass. Applies at the next start.";
            busy = false;
        };
        cb.CheckedChanged += (o, e) => {
            if (busy) return;
            if (cb.Checked) TurnOn(s);
            else TurnOff(s);
            refresh();
        };
        clear.Click += (o, e) => {
            UseFolder(s, null);
            refresh();
        };
        refresh();
        form.ResolutionRefresh += refresh;
        s.Changed += (id, v) => {
            if (!busy && id.StartsWith("Stereo/", StringComparison.OrdinalIgnoreCase)) refresh();
        };
        t.Controls.Add(new Label { Text = "Single pass  (at the next start)", AutoSize = true, Margin = new Padding(0, 6, 8, 0) }, 0, t.RowCount);
        t.Controls.Add(cb, 1, t.RowCount);
        t.RowCount++;
        t.Controls.Add(new Label { Text = "Its shaders", AutoSize = true, Margin = new Padding(0, 8, 8, 0) }, 0, t.RowCount);
        t.Controls.Add(fr, 1, t.RowCount);
        t.RowCount++;
        t.Controls.Add(info, 1, t.RowCount);
        t.RowCount++;
        tips.SetToolTip(cb, "Writes [Stereo] SinglePass=1 (the game supplies " + string.Join(", ", Enumerable.Range(0, On.GetLength(0)).Select(i => On[i, 0] + "=" + On[i, 1]))
                            + " and InstancedShadersAuto=1 where you have not set them). Unticked: SinglePass=0, two-pass.");
        tips.SetToolTip(clear, "Forget the folder: the shaders are converted on this PC at the next start.");
    }
}

// menu.cpp's three presets: the keys each writes. The Performance preset's 72 Hz is the runtime's offer, which only
// the game knows: the launcher leaves the refresh rate as it is for it (choose 72 Hz in the headset menu if wanted)
public class Preset {
    public string Name, Tip;
    public List<KeyValuePair<string, string>> Values = new List<KeyValuePair<string, string>>();

    public static List<Preset> All {
        get {
            List<Preset> l = new List<Preset>();
            foreach (int p in new[] { 0, 1, 2 }) {
                Preset pr = new Preset { Name = p == 0 ? "Quality" : p == 1 ? "Balanced" : "Performance" };
                pr.Tip = p == 0 ? "The sharpest: eyes in the headset's shape, DLSS at DLAA, the runtime's full size and refresh rate. For a fast PC."
                       : p == 1 ? "Eyes in the headset's shape, DLSS Quality, at most 2880 pixels tall. For most PCs."
                                : "Eyes in the headset's shape, DLSS Performance (without DLSS: 85% size), at most 2208 tall. For a slower PC. (The headset menu's preset also picks 72 Hz where offered.)";
                pr.Values.Add(new KeyValuePair<string, string>("XR/EyeShape", "1"));
                pr.Values.Add(new KeyValuePair<string, string>("XR/EyeShapeDlss", "1"));
                pr.Values.Add(new KeyValuePair<string, string>("Stereo/Split_shadows", "1"));
                pr.Values.Add(new KeyValuePair<string, string>("Render/DlssQuality", p == 0 ? "0" : p == 1 ? "5" : "3"));
                pr.Values.Add(new KeyValuePair<string, string>("Render/AutoMaxHeight", p == 0 ? "0" : p == 1 ? "2880" : "2208"));
                pr.Values.Add(new KeyValuePair<string, string>("XR/EyeScale", p == 2 ? "@perf" : "1.0"));  // 0.85 without DLSS, as the menu
                if (p < 2) pr.Values.Add(new KeyValuePair<string, string>("XR/RefreshRate", "0"));
                l.Add(pr);
            }
            return l;
        }
    }

    public string Describe() {
        return Tip + "\n" + string.Join("  ", Values.Select(v => v.Key.Substring(v.Key.IndexOf('/') + 1) + "=" + (v.Value == "@perf" ? "0.85 (1.0 with DLSS)" : v.Value)));
    }

    public void Apply(Settings s) {
        foreach (KeyValuePair<string, string> kv in Values) {
            int i = kv.Key.IndexOf('/');
            string v = kv.Value == "@perf" ? (s.Get("Render", "ForceAntiAliasing", "1") == "3" ? "1.0" : "0.85") : kv.Value;
            s.Set(kv.Key.Substring(0, i), kv.Key.Substring(i + 1), v);
        }
    }
}

// The ini writer's checks (no game needed): run with --selftest <scratch dir>
public static class SelfTest {
    static int fails, checks;

    static int Clamp(int s) {
        AtStart.Open(0x7ffffff0, s);
        int r = AtStart.Timeout;
        AtStart.Leave();
        return r;
    }

    static void Check(string name, bool ok, string detail) {
        ++checks;
        if (!ok) ++fails;
        Console.WriteLine((ok ? "PASS: " : "FAIL: ") + name + (detail != null ? "  " + detail : ""));
    }

    public static int Run(string dir) {
        Directory.CreateDirectory(dir);
        string p = Path.Combine(dir, "t.ini");
        File.WriteAllBytes(p, System.Text.Encoding.ASCII.GetBytes("; header\r\n[XR]\r\nHud=wrist ; a note\r\n; a comment\r\nEyeShape=0\r\n\r\n[Render]\r\nHeatHaze=1\r\n"));
        IniFile f = new IniFile(p);
        Check("read a value with an inline comment", f.Get("XR", "Hud") == "wrist", f.Get("XR", "Hud"));
        Check("read case-insensitively", f.Get("xr", "eyeshape") == "0", f.Get("xr", "eyeshape"));
        f.Set("XR", "EyeShape", "1");
        f.Set("XR", "Sharpness", "0.3");
        f.Set("Render", "HeatHaze", "0");
        f.Set("Body", "Height", "1.8");
        f.Save();
        string t = System.Text.Encoding.ASCII.GetString(File.ReadAllBytes(p));
        Check("comments kept", t.Contains("; a comment") && t.Contains("; header") && t.Contains("Hud=wrist ; a note"), null);
        Check("a key replaced in place", t.Contains("EyeShape=1") && !t.Contains("EyeShape=0"), null);
        Check("a new key at its section's end", t.IndexOf("Sharpness=0.3") > t.IndexOf("EyeShape=1") && t.IndexOf("Sharpness=0.3") < t.IndexOf("[Render]"), null);
        Check("a new section appended", t.TrimEnd().EndsWith("[Body]\r\nHeight=1.8"), null);
        Check("CRLF only, no BOM", !t.Replace("\r\n", "").Contains("\n") && (byte)t[0] == (byte)';', null);
        Check("no .tmp left", !File.Exists(p + ".tmp"), null);
        IniFile g = new IniFile(p);
        Check("re-read", g.Get("Body", "Height") == "1.8" && g.Get("Render", "HeatHaze") == "0", null);
        // the resolution formula against render_res.cpp: Quest 3 at 100 % = 3926 x 2208, at 150 % = 4808 x 2704
        int w, h;
        Resolution.Size(8, 100, out w, out h);
        Check("Quest 3 100% = 3926x2208", w == 3926 && h == 2208, w + "x" + h);
        Resolution.Size(8, 150, out w, out h);
        Check("Quest 3 150% = sqrt-scaled, even", h == 2 * (int)Math.Round(2208 * Math.Sqrt(1.5) / 2.0, MidpointRounding.AwayFromZero) && h % 2 == 0 && w % 2 == 0, w + "x" + h);
        bool ok = Resolution.Size(14, 200, out w, out h);
        Check("Galaxy XR 200% = 9654x5430, inside the 5624 limit", ok && w == 9654 && h == 5430, w + "x" + h);
        // the settings reset: a release tag the user file lacks moves that file aside on the first save, as the mod would
        string gd = Path.Combine(dir, "game");
        Directory.CreateDirectory(gd);
        foreach (string old in Directory.GetFiles(dir, "user.ini*")) File.Delete(old);
        File.WriteAllBytes(Path.Combine(gd, "RDRVR.ini"), System.Text.Encoding.ASCII.GetBytes("[Compat]\r\nSettingsReset=t2\r\n[XR]\r\nHud=off\r\nEyeShape=1\r\n"));
        string up = Path.Combine(dir, "user.ini");
        File.WriteAllBytes(up, System.Text.Encoding.ASCII.GetBytes("; mine\r\n[Compat]\r\nSettingsReset=t1\r\n[XR]\r\nHud=wrist\r\n"));
        Settings rs = new Settings();
        rs.Load(gd, up);
        Check("a reset pending shows the release's values", rs.ResetPending && rs.Get("XR", "Hud", null) == "off", rs.ResetPending + " " + rs.Get("XR", "Hud", null));
        rs.Set("XR", "EyeShape", "0");
        string msg = rs.Save();
        IniFile after = new IniFile(up);
        Check("the old file kept, the new one tagged with only the edit", File.Exists(up + ".before-t2.bak") && after.Get("Compat", "SettingsReset") == "t2" &&
              after.Get("XR", "EyeShape") == "0" && after.Get("XR", "Hud") == null, msg);
        rs.Load(gd, up);
        Check("no reset after it", !rs.ResetPending && rs.Get("XR", "EyeShape", null) == "0" && rs.Get("XR", "Hud", null) == "off", null);
        // a save after the game wrote the file: the game's keys kept, only the edit added
        File.AppendAllText(up, "[Holsters]\r\nRightHipOffset=0.010 0.020 0.030\r\n");
        rs.Set("XR", "Sharpness", "0.3");
        rs.Save();
        IniFile kept = new IniFile(up);
        Check("a save keeps what the game wrote since", kept.Get("Holsters", "RightHipOffset") == "0.010 0.020 0.030" && kept.Get("XR", "Sharpness") == "0.3" &&
              kept.Get("XR", "EyeShape") == "0", null);
        rs.Set("XR", "Hud", "wrist");
        rs.Set("XR", "Hud", "off");
        Check("an edit set back is no edit", rs.Pending == 0, rs.Pending + " pending");
        // bytes the mod reads as ANSI (a wav path) and a BOM kept as they are
        string ap = Path.Combine(dir, "ansi.ini");
        byte[] ansi = { (byte)'[', (byte)'A', (byte)']', 13, 10, (byte)'P', (byte)'=', (byte)'c', (byte)':', 0xE9, 13, 10 };
        File.WriteAllBytes(ap, ansi);
        IniFile af = new IniFile(ap);
        af.Set("A", "Q", "1");
        af.Save();
        byte[] back = File.ReadAllBytes(ap);
        Check("ANSI bytes kept", back.Length > 10 && Array.IndexOf(back, (byte)0xE9) > 0 && Array.IndexOf(back, (byte)0xC3) < 0, null);
        File.WriteAllBytes(ap, new byte[] { 0xEF, 0xBB, 0xBF, (byte)'[', (byte)'X', (byte)'R', (byte)']', 13, 10, (byte)'H', (byte)'u', (byte)'d', (byte)'=', (byte)'o', (byte)'n', 13, 10 });
        Check("a [section] behind a BOM is no section, as the mod reads it", new IniFile(ap).Get("XR", "Hud") == null, null);
        // values as the mod reads them
        Check("get_int: strtol base 0", ModValue.Int("010") == 8 && ModValue.Int("0x10") == 16 && ModValue.Int("12abc") == 12 && ModValue.Int("true") == 0 && ModValue.Int("-3") == -3, null);
        Check("get_float: strtof", ModValue.Float("1.5abc") == 1.5 && double.IsNaN(ModValue.Float("nan")) && double.IsPositiveInfinity(ModValue.Float("INF")) && ModValue.Float("x") == 0, null);
        Item rih = new Item { Type = "bool", Section = "Reload", Key = "RoundInHand" };
        Item blk = new Item { Type = "bool", Section = "Guns", Key = "BlockExecutions", On = "0", Off = "1" };
        Check("bool states: get_bool, inverted, RoundInHand's mode 2", new Item { Type = "bool" }.IsOn("True") && !new Item { Type = "bool" }.IsOn("2") && rih.IsOn("2") &&
              blk.IsOn("0") && !blk.IsOn("yes"), null);
        // the resolution as load_choice reads it
        Func<string, string, string, string> res = (head, scale, rres) => {
            Settings q = new Settings();
            q.User = new IniFile(Path.Combine(dir, "none.ini"));
            if (head != null) q.Edits["Render/RenderHeadset"] = head;
            if (scale != null) q.Edits["Render/RenderScale"] = scale;
            if (rres != null) q.Edits["Render/RenderResolution"] = rres;
            int pct, cw, ch;
            int c = Resolution.Choice(q, out pct, out cw, out ch);
            return c + "/" + pct + (c < 0 ? "/" + cw + "x" + ch : "");
        };
        Check("RenderHeadset=off, RenderResolution=2208 is Quest 3 (the size decides)", res("off", null, "2208") == "10/100", res("off", null, "2208"));
        Check("RenderResolution=2208 alone is Quest 3", res(null, null, "2208") == "10/100", res(null, null, "2208"));
        Check("RenderResolution=0 is off", res("quest3", null, "0") == "0/100", res("quest3", null, "0"));
        Check("quest3 at 150 % with its height", res("quest3", "150", "2704") == "10/150", res("quest3", "150", "2704"));
        Check("a hand-written WxH is custom", res(null, null, "3000x1500") == "-1/100/3000x1500", res(null, null, "3000x1500"));
        // single pass: the folder's K read from twins.txt, round 34's lines written, then turned off without new lines
        string tw = Path.Combine(dir, "twins");
        Directory.CreateDirectory(tw);
        File.WriteAllBytes(Path.Combine(tw, "vs_0123456789abcdef.dxil"), new byte[40]);
        File.WriteAllText(Path.Combine(tw, "twins.txt"), "# vs ps\r\n0123456789abcdef 0 wvp\r\n0123456789abcdef 1 instprev64\r\n0123456789abcdef 2 wvp instprev64\r\n");
        int sn, sk;
        Check("a twins folder's instprev K", SinglePass.Problem(tw, out sn, out sk) == null && sn == 1 && sk == 64, sn + " shaders, K " + sk);
        Check("a folder without twins refused", SinglePass.Problem(dir, out sn, out sk) != null, null);
        // (GOAL-single-pass-default.md item 3) the switch: SinglePass=1/0; on clears an older launcher's 0s; a folder optional
        string sp = Path.Combine(dir, "sp.ini");
        File.WriteAllBytes(sp, System.Text.Encoding.ASCII.GetBytes("[Stereo]\r\nSplit_shadows=1\r\nPrepassArrays=0\r\nPrepassCutVelocity=0\r\n"));
        Settings ss = new Settings();
        ss.Load(null, sp);
        SinglePass.TurnOn(ss);
        Check("single pass on: SinglePass=1, an older launcher's 0s back to the stack's", SinglePass.IsOn(ss) && ss.Get("Stereo", "SinglePass", null) == "1" &&
              ss.Get("Stereo", "PrepassArrays", null) == "1" && ss.Get("Stereo", "PrepassCutVelocity", null) == "4" && ss.Get("Stereo", "PrepassViews", null) == null, null);
        SinglePass.UseFolder(ss, tw);
        Check("a folder chosen: InstancedShaders and its K, InstancedShadersAuto not set", ss.Get("Stereo", "InstancedShaders", null) == tw &&
              ss.Get("Stereo", "PrepassInstPrevK", null) == "64" && ss.Get("Stereo", "InstancedShadersAuto", null) == null, null);
        SinglePass.UseFolder(ss, null);
        Check("the folder forgotten: InstancedShaders empty, the conversion on again", ss.Get("Stereo", "InstancedShaders", null) == "" &&
              ss.Get("Stereo", "InstancedShadersAuto", null) != "0", ss.Get("Stereo", "InstancedShadersAuto", null));  // (unset: config supplies it)
        ss.Save();
        ss.Load(null, sp);
        SinglePass.TurnOff(ss);
        ss.Save();
        IniFile spf = new IniFile(sp);
        Check("single pass off: SinglePass=0, the other keys kept", !SinglePass.IsOn(ss) && spf.Get("Stereo", "SinglePass") == "0" &&
              spf.Get("Stereo", "PrepassArrays") == "1" && spf.Get("Stereo", "Split_shadows") == "1", null);
        File.WriteAllBytes(sp, System.Text.Encoding.ASCII.GetBytes("[Stereo]\r\nSplit_shadows=1\r\n"));
        ss.Load(null, sp);
        Check("no key: single pass (the shipped default)", SinglePass.IsOn(ss), null);
        File.WriteAllBytes(sp, System.Text.Encoding.ASCII.GetBytes("[Stereo]\r\nPrepassArrays=0\r\nPrepassInstPrevMirror=0\r\nInstancedShadersAuto=0\r\n"));
        ss.Load(null, sp);
        SinglePass.TurnOn(ss);
        Check("on over an older launcher's untick: the mirror and the conversion on again", ss.Get("Stereo", "PrepassArrays", null) == "1" &&
              ss.Get("Stereo", "PrepassInstPrevMirror", null) == "1" && ss.Get("Stereo", "InstancedShadersAuto", null) == "1", null);
        // the menu's requirements: a parsed one greys its control out as the headset menu does
        Settings st = new Settings();
        st.User = new IniFile(Path.Combine(dir, "req.ini"));
        Requirement r = Requirement.Parse("enabled only when ManualActions=1 and (BoltParts=1 or BreechParts=1 or SemiAutoParts=1)", "Reload");
        Check("an and of an or parses", r != null && r.Terms.Count == 4, r != null ? r.Terms.Count + " terms" : "null");
        if (r != null) {
            string sec = r.Terms[0][0];
            foreach (string[] term in r.Terms) st.Set(term[0], term[1], "0");
            st.Set(sec, "ManualActions", "1");
            bool off = r.Met(st);
            st.Set(r.Terms[2][0], "BreechParts", "1");
            bool on = r.Met(st);
            st.Set(sec, "ManualActions", "0");
            Check("and / or evaluate", !off && on && !r.Met(st), off + " " + on + " " + r.Met(st));
        }
        Requirement aa = Requirement.Parse("enabled only when ForceAntiAliasing=1 (FXAA)", "XR");
        Check("a bare key found in its own section", aa != null && aa.Terms[0][0] == "Render" && aa.Terms[0][2] == "1", aa != null ? aa.Terms[0][0] + "/" + aa.Terms[0][1] + "=" + aa.Terms[0][2] : "null");
        Requirement ik = Requirement.Parse("enabled only when [Hands] ArmIK=1 (Advanced tab, Arms follow the controllers)", "Body");
        Check("a [Section] key", ik != null && ik.Terms[0][0] == "Hands" && ik.Terms[0][1] == "ArmIK", null);
        Check("a requirement on the running game is not checked", Requirement.Parse("shown only when the game build's shot-spawn hook is installed", "Weapons") == null, null);
        int parsed = 0, all = 0;
        foreach (Tab tab in Schema.Tabs)
            foreach (Item it in tab.Items)
                if (!string.IsNullOrEmpty(it.Requires) && it.Key != null) {
                    ++all;
                    if (Requirement.Parse(it.Requires, it.Section) != null) ++parsed;
                    else Console.WriteLine("  not checked: " + it.Label + ": " + it.Requires);
                }
        Console.WriteLine("  requirements checked live: " + parsed + " of " + all);
        // the launcher's own Play: the note the mod reads at the game's start (launcher_start.cpp handed_off)
        string note = Game.NotePath(Path.Combine(dir, "RDRVR.user.ini"));
        Game.WriteNote(note);
        string nt = File.ReadAllText(note);
        System.Text.RegularExpressions.Match nm = System.Text.RegularExpressions.Regex.Match(nt, "^started (\\d+) pid (\\d+)\r\n$");
        long nowUnix = (long)(DateTime.UtcNow - new DateTime(1970, 1, 1, 0, 0, 0, DateTimeKind.Utc)).TotalSeconds;
        Check("the start note: \"started <unix s> pid <n>\" now, next to the user file", Path.GetFileName(note) == "RDRVR.launcher_started" && nm.Success &&
              Math.Abs(long.Parse(nm.Groups[1].Value) - nowUnix) <= 5, nt.Trim());
        // the at-start mode with no game: no link, not holding, so saving stays refused while a game runs
        AtStart.Open(0x7ffffff0, 30);
        Check("at start with no such game: unlinked, the game not alive, not holding", !AtStart.Linked && !AtStart.GameAlive() && !AtStart.Holding(), null);
        AtStart.Leave();
        Check("the countdown clamped to 5-300 s, 0 none", Clamp(3) == 5 && Clamp(0) == 0 && Clamp(900) == 300 && Clamp(30) == 30, null);
        Console.WriteLine(checks + " checks, " + fails + " failed");
        return fails == 0 ? 0 : 1;
    }
}
}
