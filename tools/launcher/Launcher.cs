// RDRVR Launcher: the window. Three tabs: Graphics (the mod's render settings and the in-headset menu's presets),
// Mod settings (every setting of the in-headset menu's tabs, from menu_schema.json: tools/launcher/README.md says how
// it is made from src/core/menu.cpp) and Controls (the picture, Controls.cs). Save writes the changed keys to the
// user file (Core.cs); Play saves, then starts the game the normal way.
//   RDRVR_Launcher.exe                          the window
//   RDRVR_Launcher.exe --at-start <pid> <s>     opened by the mod at the game's start (AtStart, Core.cs): Play saves and
//                                               closes (the game goes on), a countdown of <s> seconds (0: none); the test
//                                               arguments --test-set Section.Key=Value, --test-press play|close <s>,
//                                               --test-hold <s> (as a click), --test-no-countdown (tests/sim/r65_launcher_start.py)
//   RDRVR_Launcher.exe --render-controls <png>  the controls picture as a PNG (no window)
//   RDRVR_Launcher.exe --screenshot <dir>       each tab rendered to <dir>\tab_<n>.png (no window shown)
//   RDRVR_Launcher.exe --selftest <dir>         the ini writer's checks on files in <dir> (prints PASS / FAIL)
using System;
using System.Collections;
using System.Collections.Generic;
using System.Drawing;
using System.Globalization;
using System.IO;
using System.Linq;
using System.Text.RegularExpressions;
using System.Reflection;
using System.Web.Script.Serialization;
using System.Windows.Forms;

namespace RdrvrLauncher {

public class Item {
    public string Type, Label, Section, Key, Default, Help, Requires, Note, On = "1", Off = "0", Format, Written, OptionsText;
    public bool Presets;
    public bool Restart;
    public List<KeyValuePair<string, string>> Options = new List<KeyValuePair<string, string>>();  // (value, label)
    public double Min, Max = 1, Step = 1, Scale = 1;
    public List<string> Keys = new List<string>();

    static string S(Dictionary<string, object> d, string k) {
        object v;
        return d.TryGetValue(k, out v) && v != null ? Convert.ToString(v, CultureInfo.InvariantCulture) : null;
    }

    static double D(Dictionary<string, object> d, string k, double def) {
        object v;
        if (!d.TryGetValue(k, out v) || v == null) return def;
        double r;
        return double.TryParse(Convert.ToString(v, CultureInfo.InvariantCulture), NumberStyles.Float, CultureInfo.InvariantCulture, out r) ? r : def;
    }

    public static Item From(Dictionary<string, object> d) {
        Item it = new Item {
            Type = S(d, "type") ?? "note", Label = S(d, "label") ?? "", Section = S(d, "section"), Key = S(d, "key"), Default = S(d, "default"),
            Help = S(d, "help"), Requires = S(d, "requires"), Note = S(d, "note"), Format = S(d, "format"), Written = S(d, "written")
        };
        it.Presets = d.ContainsKey("presets");
        if (S(d, "on") != null) it.On = S(d, "on");
        if (S(d, "off") != null) it.Off = S(d, "off");
        object r;
        it.Restart = d.TryGetValue("restart", out r) && r is bool && (bool)r;
        it.Min = D(d, "min", 0);
        it.Max = D(d, "max", 1);
        it.Step = D(d, "step", 1);
        it.Scale = D(d, "scale", 1);
        object o;
        if (d.TryGetValue("options", out o) && o is ArrayList)
            foreach (object x in (ArrayList)o) {
                Dictionary<string, object> od = x as Dictionary<string, object>;
                if (od != null) it.Options.Add(new KeyValuePair<string, string>(S(od, "value"), S(od, "label") ?? S(od, "value")));
            }
        else if (o != null) it.OptionsText = Convert.ToString(o, CultureInfo.InvariantCulture);  // "auto + weapon_tokens (...)": a holster's weapon
        if (d.TryGetValue("keys", out o) && o is ArrayList)
            foreach (object x in (ArrayList)o) it.Keys.Add(Convert.ToString(x, CultureInfo.InvariantCulture));
        return it;
    }

    // a bool's state as the mod reads the value: get_bool (1, true, yes, on), inverted for a bool whose ticked state
    // writes 0 (BlockExecutions); [Reload] RoundInHand is a mode read by get_int (2 is on too)
    public bool IsOn(string v) {
        if (Section == "Reload" && Key == "RoundInHand") return ModValue.Int(v) > 0;
        if (On == "1" && Off == "0") return ModValue.Bool(v);
        if (On == "0" && Off == "1") return !ModValue.Bool(v);
        return v == On;
    }

    public Item Copy() {
        Item c = (Item)MemberwiseClone();
        c.Options = new List<KeyValuePair<string, string>>(Options);
        c.Keys = new List<string>(Keys);
        return c;
    }
}

public class Tab {
    public string Name;
    public List<Item> Items = new List<Item>();
}

public static class Schema {
    public static List<Tab> Tabs = new List<Tab>();
    public static string Source = "";
    public static List<KeyValuePair<string, string>> Weapons = new List<KeyValuePair<string, string>>();  // (token, label) by the menu's index

    public static void Load() {
        Assembly a = Assembly.GetExecutingAssembly();
        string res = a.GetManifestResourceNames().FirstOrDefault(n => n.EndsWith("menu_schema.json"));
        if (res == null) return;
        string json;
        using (StreamReader r = new StreamReader(a.GetManifestResourceStream(res))) json = r.ReadToEnd();
        JavaScriptSerializer js = new JavaScriptSerializer { MaxJsonLength = int.MaxValue };
        Dictionary<string, object> root = js.Deserialize<Dictionary<string, object>>(json);
        object v;
        if (root.TryGetValue("source", out v)) Source = Convert.ToString(v);
        if (root.TryGetValue("weapon_tokens", out v) && v is ArrayList)
            foreach (object w in (ArrayList)v) {
                Dictionary<string, object> wd = w as Dictionary<string, object>;
                if (wd != null) Weapons.Add(new KeyValuePair<string, string>(Convert.ToString(wd["value"]), Convert.ToString(wd["label"])));
            }
        if (!root.TryGetValue("tabs", out v) || !(v is ArrayList)) return;
        foreach (object t in (ArrayList)v) {
            Dictionary<string, object> td = t as Dictionary<string, object>;
            if (td == null) continue;
            Tab tab = new Tab { Name = Convert.ToString(td["name"]) };
            object items;
            if (td.TryGetValue("items", out items) && items is ArrayList)
                foreach (object i in (ArrayList)items) {
                    Dictionary<string, object> id = i as Dictionary<string, object>;
                    if (id != null) tab.Items.Add(Item.From(id));
                }
            // a list the tab's items name for their options ("options": "button_targets")
            foreach (KeyValuePair<string, object> kv in td) {
                if (kv.Key == "items" || !(kv.Value is ArrayList)) continue;
                foreach (Item it in tab.Items) {
                    if (it.Options.Count > 0 || it.OptionsText != kv.Key) continue;
                    foreach (object x in (ArrayList)kv.Value) {
                        Dictionary<string, object> od = x as Dictionary<string, object>;
                        if (od == null) continue;
                        string val = Convert.ToString(od["value"]);
                        // a game button with what it does in the game, as the controls picture names it
                        string act = kv.Key == "button_targets" ? ControlsPicture.Pad(val) : null;
                        it.Options.Add(new KeyValuePair<string, string>(val, string.IsNullOrEmpty(act) || act == val ? val : val + "  ·  " + act));
                    }
                }
            }
            Tabs.Add(tab);
        }
    }

    public static Item Find(string section, string key) {
        foreach (Tab t in Tabs)
            foreach (Item i in t.Items)
                if (i.Key != null && string.Equals(i.Section, section, StringComparison.OrdinalIgnoreCase) &&
                    string.Equals(i.Key, key, StringComparison.OrdinalIgnoreCase))
                    return i;
        return null;
    }

    // a compound item (several keys or a runtime list) that writes this "Section.Key" (not a preset, which writes many)
    public static Item FindCompound(string sectionKey) {
        foreach (Tab t in Tabs)
            foreach (Item j in t.Items)
                if (j.Type == "compound" && !j.Presets && j.Keys.Any(k => string.Equals(k, sectionKey, StringComparison.OrdinalIgnoreCase))) return j;
        return null;
    }

    // a key named without its section (a requirement's "Enabled=1"): the item's own section first, else wherever the
    // menu has that key
    public static Item FindKey(string section, string key) {
        Item i = Find(section, key);
        if (i != null) return i;
        foreach (Tab t in Tabs)
            foreach (Item j in t.Items)
                if (j.Key != null && string.Equals(j.Key, key, StringComparison.OrdinalIgnoreCase)) return j;
        return null;
    }
}

// An item's "requires" as the menu greys it out: "enabled only when Enabled=1, Anchor=headset and TurnWithHead=1",
// "(BoltParts=1 or BreechParts=1)", "[Hands] ArmIK=1". Parsed into section/key=value terms joined by and / or; a
// requirement on the running game (a hook, a controller seen, a gun in hand) does not parse and is not checked here
public class Requirement {
    public readonly List<string[]> Terms = new List<string[]>();  // {section, key, value}
    List<string> toks;
    int pos;

    public static Requirement Parse(string text, string section) {
        if (string.IsNullOrEmpty(text)) return null;
        string s = Regex.Replace(text, @"^(enabled|shown) only when\s+", "");
        s = Regex.Replace(s, @"\s*\([^()=]*\)", "");  // "(FXAA)", "(indented under it)"
        s = Regex.Replace(s, @"\[(\w+)\]\s*(\w+=)", "$1.$2");
        Requirement r = new Requirement { toks = new List<string>() };
        foreach (string tk in Regex.Split(s, @"(\(|\)|,|\band\b|\bor\b)")) {
            string t = tk.Trim();
            if (t.Length == 0) continue;
            if (t == "(" || t == ")" || t == "," || t == "and" || t == "or") {
                r.toks.Add(t);
                continue;
            }
            Match m = Regex.Match(t, @"^(?:(\w+)\.)?(\w+)=([\w.]+)$");
            if (!m.Success) return null;
            string sec = m.Groups[1].Success ? m.Groups[1].Value : null;
            if (sec == null) {
                Item it = Schema.FindKey(section, m.Groups[2].Value);
                sec = it != null ? it.Section : section;
            }
            r.toks.Add("#" + r.Terms.Count);
            r.Terms.Add(new[] { sec, m.Groups[2].Value, m.Groups[3].Value });
        }
        return r.Terms.Count > 0 ? r : null;
    }

    // or of ands, "," an and, parentheses grouping
    public bool Met(Settings st) {
        pos = 0;
        bool v = Or(st);
        return pos == toks.Count ? v : true;
    }

    bool Or(Settings st) {
        bool v = And(st);
        while (pos < toks.Count && toks[pos] == "or") {
            ++pos;
            v = And(st) | v;
        }
        return v;
    }

    bool And(Settings st) {
        bool v = Term(st);
        while (pos < toks.Count && (toks[pos] == "and" || toks[pos] == ",")) {
            ++pos;
            v = Term(st) & v;
        }
        return v;
    }

    bool Term(Settings st) {
        if (pos >= toks.Count) return true;
        string t = toks[pos++];
        if (t == "(") {
            bool v = Or(st);
            if (pos < toks.Count && toks[pos] == ")") ++pos;
            return v;
        }
        if (t[0] != '#') return true;
        string[] term = Terms[int.Parse(t.Substring(1))];
        Item it = Schema.Find(term[0], term[1]);
        string cur = st.Get(term[0], term[1], it != null ? it.Default : null);
        // a bool by its state (Enabled=1 is "Enabled ticked"), anything else as the mod compares it: as written
        if (it != null && it.Type == "bool" && (term[2] == it.On || term[2] == it.Off)) return it.IsOn(cur) == (term[2] == it.On);
        return string.Equals(cur, term[2], StringComparison.Ordinal);
    }
}

// A list or number that the mouse wheel does not change: a wheel over it scrolls the page, so scrolling a long page
// never edits a setting on the way (the dropped-down list still scrolls)
public class WheelCombo : ComboBox {
    protected override void WndProc(ref Message m) {
        if (m.Msg == 0x020A && !DroppedDown) {  // WM_MOUSEWHEEL
            Wheel.ScrollPage(this, (short)((m.WParam.ToInt64() >> 16) & 0xFFFF));
            return;
        }
        base.WndProc(ref m);
    }
}

public class WheelNumber : NumericUpDown {
    protected override void OnMouseWheel(MouseEventArgs e) {
        Wheel.ScrollPage(this, e.Delta);
        HandledMouseEventArgs h = e as HandledMouseEventArgs;
        if (h != null) h.Handled = true;
    }
}

public static class Wheel {
    public static void ScrollPage(Control c, int delta) {
        for (Control p = c.Parent; p != null; p = p.Parent) {
            ScrollableControl s = p as ScrollableControl;
            if (s != null && s.AutoScroll && s.VerticalScroll.Visible) {
                Point at = s.AutoScrollPosition;
                s.AutoScrollPosition = new Point(-at.X, Math.Max(0, -at.Y - delta));
                return;
            }
        }
    }
}

public class MainForm : Form {
    readonly Settings settings = new Settings();
    string gameDir;
    bool steam;
    readonly ToolTip tips = new ToolTip { AutoPopDelay = 20000, InitialDelay = 300 };
    readonly Label status = new Label { AutoSize = false, Dock = DockStyle.Fill, TextAlign = ContentAlignment.MiddleLeft };
    readonly TabControl tabs = new TabControl { Dock = DockStyle.Fill };
    PictureBox controlsPic;
    // every control bound to a key, refreshed when the key changes anywhere (Graphics and Mod settings share keys)
    readonly Dictionary<string, List<Action>> refreshers = new Dictionary<string, List<Action>>(StringComparer.OrdinalIgnoreCase);
    bool refreshing;
    // the at-start mode (AtStart, Core.cs; research/run30/launcher.md): the strip at the top, its countdown, the tests'
    // presses
    readonly Label atStartText = new Label { AutoSize = false, Dock = DockStyle.Fill, TextAlign = ContentAlignment.MiddleLeft,
                                             Font = new Font("Segoe UI", 10.5f, FontStyle.Bold) };
    Panel atStartStrip;
    readonly System.Windows.Forms.Timer atStartTimer = new System.Windows.Forms.Timer { Interval = 250 };
    DateTime countdownEnd, testPressAt, testHoldAt;
    bool counting, noPrompt;
    int exitWhy = 6, aliveTick;  // (2026-11-02) 6: closed by the user, the game's start cancelled
    Button playButton, closeButton;
    public static readonly List<KeyValuePair<string, string>> TestSets = new List<KeyValuePair<string, string>>();  // ("Section.Key", value)
    public static string TestPress;  // "play" or "close"
    public static double TestPressAfter, TestHoldAfter = -1;
    public static bool TestNoCountdown;

    public MainForm() {
        Text = "RDRVR Launcher";
        Width = 1180;
        Height = 860;
        MinimumSize = new Size(900, 600);
        StartPosition = FormStartPosition.CenterScreen;
        Font = new Font("Segoe UI", 9.5f);
        gameDir = Game.FindGameDir(out steam);
        settings.Load(gameDir);
        settings.Changed += (id, value) => {
            List<Action> rs;
            if (refreshers.TryGetValue(id, out rs) && !refreshing) {
                refreshing = true;
                foreach (Action r in rs) r();
                refreshing = false;
            }
            if (controlsPic != null && (id.StartsWith("Controls/", StringComparison.OrdinalIgnoreCase) || id.StartsWith("Holsters/Mode", StringComparison.OrdinalIgnoreCase) ||
                                        id.StartsWith("XR/Hud", StringComparison.OrdinalIgnoreCase) || id.StartsWith("Comfort/TurnMode", StringComparison.OrdinalIgnoreCase)))
                RedrawControls();
            UpdateStatus();
        };

        tabs.TabPages.Add(GraphicsPage());
        tabs.TabPages.Add(ModPage());
        tabs.TabPages.Add(ControlsPage());

        TableLayoutPanel bar = new TableLayoutPanel { Dock = DockStyle.Bottom, Height = 52, ColumnCount = 5, Padding = new Padding(8, 6, 8, 6) };
        bar.ColumnStyles.Add(new ColumnStyle(SizeType.Percent, 100));
        bar.ColumnStyles.Add(new ColumnStyle(SizeType.Absolute, 290));
        for (int i = 0; i < 3; ++i) bar.ColumnStyles.Add(new ColumnStyle(SizeType.Absolute, 120));
        Button save = new Button { Text = "Save", Dock = DockStyle.Fill }, play = new Button { Text = "Save && Play", Dock = DockStyle.Fill },
               close = new Button { Text = "Close", Dock = DockStyle.Fill };
        playButton = play;
        closeButton = close;
        // [Launcher] ShowAtStart (the mod opens this window at the game's start), as the Mod settings tab's row: an edit,
        // saved with the others
        CheckBox atStartBox = new CheckBox { Text = "Show this window when the game starts", AutoSize = false, Dock = DockStyle.Fill,
                                             TextAlign = ContentAlignment.MiddleLeft };
        Item atStartItem = Schema.Find("Launcher", "ShowAtStart");
        Action atStartRefresh = () => {
            atStartBox.Checked = ModValue.Bool(settings.Get("Launcher", "ShowAtStart", atStartItem != null ? atStartItem.Default : "1"));
        };
        Fill(atStartRefresh);
        atStartBox.CheckedChanged += (o, e) => {
            if (refreshing) return;
            settings.Set("Launcher", "ShowAtStart", atStartBox.Checked ? "1" : "0");
        };
        Bind("Launcher", "ShowAtStart", atStartRefresh);
        tips.SetToolTip(atStartBox, "Ticked, this window opens each time the game starts (Steam, the Rockstar Games Launcher or RDR.exe), and the game " +
                                    "waits for its Play. Unticked, the game starts at once. Saved with Save or Save && Play.");
        save.Click += (o, e) => DoSave();
        play.Click += (o, e) => {
            if (AtStart.Active) {  // opened by the game: save, then let it go on (no second game started)
                if (settings.Pending > 0 && !DoSave()) return;
                exitWhy = 0;
                Close();
                return;
            }
            if (Game.Running()) {
                MessageBox.Show(this, "The game is already running.", "RDRVR Launcher", MessageBoxButtons.OK, MessageBoxIcon.Information);
                return;
            }
            if (settings.Pending > 0 && !DoSave()) return;
            try {
                status.Text = Game.Start(gameDir, steam);
                Close();
            } catch (Exception ex) {
                MessageBox.Show(this, ex.Message, "RDRVR Launcher", MessageBoxButtons.OK, MessageBoxIcon.Warning);
            }
        };
        close.Click += (o, e) => Close();
        bar.Controls.Add(status, 0, 0);
        bar.Controls.Add(atStartBox, 1, 0);
        bar.Controls.Add(save, 2, 0);
        bar.Controls.Add(play, 3, 0);
        bar.Controls.Add(close, 4, 0);
        Controls.Add(tabs);
        Controls.Add(bar);
        if (AtStart.Active) {
            Text = "RDRVR Launcher - Red Dead Redemption is waiting";
            atStartStrip = new Panel { Dock = DockStyle.Top, Height = 42, Padding = new Padding(12, 4, 12, 4), BackColor = Color.FromArgb(255, 241, 200) };
            atStartStrip.Controls.Add(atStartText);
            Controls.Add(atStartStrip);
            tips.SetToolTip(close, "Closes this window and cancels the game's start (Save & Play starts it).");
            Shown += (o, e) => BeginAtStart();
            FormClosed += (o, e) => AtStart.ExitCode = exitWhy;
        }
        FormClosing += (o, e) => {
            if (noPrompt) return;  // the countdown's end, the game gone or gone on: nothing to ask
            if (settings.Pending > 0 &&
                MessageBox.Show(this, settings.Pending + " change(s) not saved. Close anyway?", "RDRVR Launcher", MessageBoxButtons.YesNo, MessageBoxIcon.Question) == DialogResult.No)
                e.Cancel = true;
        };
        // back from the game (or a release installed meanwhile): show the files as they are now, the edits kept
        Activated += (o, e) => {
            if (!settings.ChangedOnDisk() || (Game.Running() && !AtStart.Holding())) return;
            settings.Reload();
            RefreshAll();
        };
        UpdateStatus();
        // the tests' edits (--test-set), as if made in the window
        foreach (KeyValuePair<string, string> kv in TestSets) {
            int dot = kv.Key.IndexOf('.');
            settings.Set(kv.Key.Substring(0, dot), kv.Key.Substring(dot + 1), kv.Value);
        }
    }

    // ---- the at-start mode: the countdown, the user's presence, the game's going on
    class Presence : IMessageFilter {
        readonly MainForm form;

        public Presence(MainForm f) {
            form = f;
        }

        // a click (in the window or on its frame), a key or the wheel: someone is at the desk
        public bool PreFilterMessage(ref Message m) {
            int k = m.Msg;
            if (k == 0x201 || k == 0x204 || k == 0x207 || k == 0xA1 || k == 0xA4 || k == 0x100 || k == 0x104 || k == 0x20A) form.UserIsHere();
            return false;
        }
    }

    void BeginAtStart() {
        counting = AtStart.Timeout > 0 && !TestNoCountdown;
        countdownEnd = DateTime.UtcNow.AddSeconds(AtStart.Timeout);
        if (TestPress != null) testPressAt = DateTime.UtcNow.AddSeconds(TestPressAfter);
        if (TestHoldAfter >= 0) testHoldAt = DateTime.UtcNow.AddSeconds(TestHoldAfter);
        UpdateStrip();
        // once to the front (the game, with no window yet, allowed it): TopMost for a moment
        TopMost = true;
        Activate();
        BeginInvoke(new Action(() => TopMost = false));
        Application.AddMessageFilter(new Presence(this));
        atStartTimer.Tick += (o, e) => AtStartTick();
        atStartTimer.Start();
    }

    void UserIsHere() {
        if (!counting || !AtStart.Active || !AtStart.Linked) return;  // (unlinked: the game goes on at its own deadline anyway)
        counting = false;
        AtStart.Hold();
        UpdateStrip();
    }

    void UpdateStrip() {
        if (!AtStart.Active) return;
        if (counting) {
            int left = Math.Max(0, (int)Math.Ceiling((countdownEnd - DateTime.UtcNow).TotalSeconds));
            atStartText.Text = "Red Dead Redemption is waiting for this window. It goes on by itself in " + left + " s.   Click anywhere to stay; Close cancels it.";
        } else if (!AtStart.Linked) {
            atStartText.Text = "Red Dead Redemption is waiting for this window, and goes on by itself shortly.";
        } else {
            atStartText.Text = "Red Dead Redemption is waiting for you: Save && Play when ready, Close to cancel (it goes on by itself after 5 minutes).";
        }
    }

    void AtStartTick() {
        if (!AtStart.Active) return;
        if (++aliveTick % 4 == 0 && !AtStart.GameAlive()) {  // the game's process ended (MTLX's own restart, a crash): nothing waits
            exitWhy = 4;
            noPrompt = true;
            Close();
            return;
        }
        if (AtStart.Gone()) {
            LeaveAtStart();
            return;
        }
        if (TestHoldAfter >= 0 && DateTime.UtcNow >= testHoldAt) {  // as a click in the window
            TestHoldAfter = -1;
            UserIsHere();
        }
        if (TestPress != null && DateTime.UtcNow >= testPressAt) {
            Button b = TestPress == "close" ? closeButton : playButton;
            TestPress = null;
            noPrompt = true;  // (nobody to answer a question)
            b.PerformClick();
            return;
        }
        if (counting && DateTime.UtcNow >= countdownEnd) {  // nobody at the desk: the game goes on with what is saved
            exitWhy = 2;
            noPrompt = true;
            Close();
            return;
        }
        UpdateStrip();
    }

    // the game went on without this window (its 5 minutes, or its deadline): from now on the normal launcher, which
    // saves nothing while the game runs
    void LeaveAtStart() {
        AtStart.Leave();
        counting = false;
        atStartTimer.Stop();
        exitWhy = 3;
        if (settings.Pending == 0) {
            noPrompt = true;
            Close();
            return;
        }
        Text = "RDRVR Launcher";
        atStartStrip.BackColor = Color.FromArgb(255, 222, 222);
        atStartText.Text = "The game went on without waiting. Your " + settings.Pending + " unsaved change" + (settings.Pending == 1 ? "" : "s") +
                           " can be saved once the game is closed.";
    }

    void RefreshAll() {
        bool was = refreshing;
        refreshing = true;
        foreach (List<Action> l in refreshers.Values)
            foreach (Action r in l) r();
        refreshing = was;
        if (ResolutionRefresh != null) ResolutionRefresh();
        RedrawControls();
        UpdateStatus();
    }

    public Action ResolutionRefresh;  // Graphics.cs's rows (two pickers share it)

    void UpdateStatus() {
        string where = gameDir == null ? "the game was not found" : ("the game: " + gameDir + (steam ? " (Steam)" : ""));
        string pending = settings.Pending > 0 ? settings.Pending + " unsaved change" + (settings.Pending == 1 ? "" : "s") + "  ·  " : "";
        string reset = settings.ResetPending ? "This release starts from its defaults once: your old settings will be kept as a .bak  ·  " : "";
        status.Text = pending + reset + where;
    }

    bool DoSave() {
        // without the game folder the release's settings-reset tag is unknown, and a file without it would be moved
        // aside by the mod at its next start: the folder first
        if (gameDir == null) {
            using (FolderBrowserDialog fb = new FolderBrowserDialog { Description = "The game was not found. Choose the Red Dead Redemption folder (with RDR.exe and RDRVR.ini):" }) {
                if (fb.ShowDialog(this) != DialogResult.OK) return false;
                if (!File.Exists(Path.Combine(fb.SelectedPath, "RDR.exe"))) {
                    MessageBox.Show(this, "RDR.exe is not in that folder.", "RDRVR Launcher: not saved", MessageBoxButtons.OK, MessageBoxIcon.Warning);
                    return false;
                }
                gameDir = fb.SelectedPath;
                settings.GameDir = gameDir;
            }
        }
        try {
            string msg = settings.Save();
            RefreshAll();
            status.Text = msg;
            return true;
        } catch (Exception ex) {
            MessageBox.Show(this, ex.Message, "RDRVR Launcher: not saved", MessageBoxButtons.OK, MessageBoxIcon.Warning);
            return false;
        }
    }

    // a control's first fill is not an edit: a value the ini spells another way ("True", "Wrist") or out of the
    // control's range is shown, and written only if the user changes it
    void Fill(Action rf) {
        bool was = refreshing;
        refreshing = true;
        rf();
        refreshing = was;
    }

    void Bind(string section, string key, Action refresh) {
        string id = Settings.Id(section, key);
        List<Action> l;
        if (!refreshers.TryGetValue(id, out l)) refreshers[id] = l = new List<Action>();
        l.Add(refresh);
    }

    // ---- the generic page: one row per item, the label left, the control right, the help under it
    Panel ItemsPanel(IEnumerable<Item> items) {
        TableLayoutPanel t = new TableLayoutPanel { ColumnCount = 2, AutoSize = true, AutoSizeMode = AutoSizeMode.GrowAndShrink, Dock = DockStyle.Top,
                                                    Padding = new Padding(16, 10, 16, 16) };
        t.ColumnStyles.Add(new ColumnStyle(SizeType.Absolute, 330));
        t.ColumnStyles.Add(new ColumnStyle(SizeType.Percent, 100));
        foreach (Item it in items) AddRow(t, it);
        Panel p = new Panel { Dock = DockStyle.Fill, AutoScroll = true };
        p.Controls.Add(t);
        return p;
    }

    void AddRow(TableLayoutPanel t, Item it) {
        if (it.Type == "note") {
            Label n = new Label { Text = it.Label, AutoSize = true, ForeColor = SystemColors.GrayText, MaximumSize = new Size(1080, 0), Margin = new Padding(0, 4, 0, 8) };
            t.Controls.Add(n, 0, t.RowCount);
            t.SetColumnSpan(n, 2);
            t.RowCount++;
            return;
        }
        if (it.Type == "compound" && it.Keys.Any(k => k.EndsWith("RenderHeadset", StringComparison.OrdinalIgnoreCase))) {
            Resolution.AddRows(this, t, settings, tips);
            return;
        }
        if (it.Type == "compound" && it.Presets) {
            AddPresets(t);
            return;
        }
        // a per-gun key (Weapon.<Token>): the menu shows it for the gun in hand; here one row per long gun (the
        // menu's weapon index 8 to 20), without the menu's "(... once first)" reminders, which need the running game
        if (it.Section != null && it.Section.Contains("<Token>")) {
            for (int i = 8; i <= 20 && i < Schema.Weapons.Count; ++i) {  // Carbine .. Carcano
                Item g = it.Copy();
                g.Section = it.Section.Replace("<Token>", Schema.Weapons[i].Key);
                g.Label = Regex.Replace(it.Label, @"\s*\(per long gun\)", "").TrimEnd(':', ' ') + ": " + Schema.Weapons[i].Value;
                g.Options = it.Options.Select(o => new KeyValuePair<string, string>(o.Key, Regex.Replace(o.Value, @"\s*\([^()]*once first\)", ""))).ToList();
                g.Requires = null;
                if (i > 8) g.Help = null;
                AddRow(t, g);
            }
            return;
        }
        // a holster's weapon: the menu lists the owned weapons that fit the holster's game slot (read in gameplay);
        // here the weapons of its kind. One not owned, or not fitting, draws the automatic one, as in the menu
        if (it.Type == "choice" && it.Options.Count == 0 && it.OptionsText != null && it.OptionsText.Contains("weapon_tokens")) {
            Item g = it.Copy();
            g.Options.Add(new KeyValuePair<string, string>("auto", "Automatic (the game's weapon for it)"));
            foreach (string w in HolsterWeapons(it.Key)) {
                KeyValuePair<string, string> tok = Schema.Weapons.FirstOrDefault(x => x.Key == w);
                if (tok.Key != null) g.Options.Add(tok);
            }
            g.Help = (it.Help ?? "") + " A weapon you don't own, or that doesn't fit this holster, draws the automatic one.";
            AddRow(t, g);
            return;
        }
        if (it.Type == "header") {
            Label h = new Label { Text = it.Label, AutoSize = true, Font = new Font(Font.FontFamily, 11f, FontStyle.Bold), Margin = new Padding(0, 14, 0, 4) };
            t.Controls.Add(h, 0, t.RowCount);
            t.SetColumnSpan(h, 2);
            t.RowCount++;
            return;
        }
        Control c = Editor(it);
        bool restartInLabel = it.Label.IndexOf("restart", StringComparison.OrdinalIgnoreCase) >= 0 ||
                              it.Label.IndexOf("next start", StringComparison.OrdinalIgnoreCase) >= 0;  // (a label that says so already)
        Label l = new Label { Text = it.Label + (it.Restart && !restartInLabel ? "  (at the next start)" : ""), AutoSize = true, Margin = new Padding(0, 6, 8, 0),
                              MaximumSize = new Size(320, 0) };
        t.Controls.Add(l, 0, t.RowCount);
        t.Controls.Add(c, 1, t.RowCount);
        string help = it.Help ?? "";
        if (help != "") {
            tips.SetToolTip(l, help);
            tips.SetToolTip(c, help);
        }
        t.RowCount++;
        Label hl = null;
        if (help != "" && it.Type != "action" && it.Type != "compound") {
            hl = new Label { Text = help, AutoSize = true, ForeColor = SystemColors.GrayText, MaximumSize = new Size(760, 0), Margin = new Padding(0, 0, 0, 6) };
            t.Controls.Add(hl, 1, t.RowCount);
            t.RowCount++;
        }
        // greyed out while the setting it depends on is off, as in the headset menu
        Requirement req = it.Key != null ? Requirement.Parse(it.Requires, it.Section) : null;
        if (req != null) {
            Action en = () => {
                bool on = req.Met(settings);
                c.Enabled = on;
                l.ForeColor = on ? SystemColors.ControlText : SystemColors.GrayText;
            };
            en();
            foreach (string[] term in req.Terms) Bind(term[0], term[1], en);
        }
    }

    Control Editor(Item it) {
        if (it.Type == "bool" && it.Key != null) {
            CheckBox cb = new CheckBox { AutoSize = true, Margin = new Padding(0, 6, 0, 0) };
            Action rf = () => cb.Checked = it.IsOn(settings.Get(it.Section, it.Key, it.Default ?? it.Off));
            Fill(rf);
            cb.CheckedChanged += (o, e) => {
                if (refreshing) return;
                settings.Set(it.Section, it.Key, cb.Checked ? it.On : it.Off);
                // eye_shape.cpp set_dlss_next_start: the DLSS eye shape needs the eye shape itself
                if (cb.Checked && it.Section == "XR" && it.Key == "EyeShapeDlss") settings.Set("XR", "EyeShape", "1");
            };
            Bind(it.Section, it.Key, rf);
            return cb;
        }
        if (it.Type == "choice" && it.Key != null) {
            ComboBox cb = new WheelCombo { DropDownStyle = ComboBoxStyle.DropDownList, Width = 340 };
            foreach (KeyValuePair<string, string> o in it.Options) cb.Items.Add(o.Value);
            Action rf = () => {
                string v = settings.Get(it.Section, it.Key, it.Default);
                // the mod compares a choice as written ("Snap" is not snap to it)
                int idx = it.Options.FindIndex(o => string.Equals(o.Key, v, StringComparison.Ordinal));
                if (idx < 0 && v != null) {  // a value the menu does not list (set by hand): shown as it is
                    if (cb.Items.Count == it.Options.Count) cb.Items.Add(v + " (custom)");
                    idx = it.Options.Count;
                }
                cb.SelectedIndex = idx;
            };
            Fill(rf);
            cb.SelectedIndexChanged += (o, e) => {
                if (!refreshing && cb.SelectedIndex >= 0 && cb.SelectedIndex < it.Options.Count) settings.Set(it.Section, it.Key, it.Options[cb.SelectedIndex].Key);
            };
            Bind(it.Section, it.Key, rf);
            return cb;
        }
        if (it.Type == "number" && it.Key != null) {
            NumericUpDown n = new WheelNumber { Width = 140 };
            double scale = it.Scale == 0 ? 1 : it.Scale;
            // the step's own decimals (0.25: 2, 0.005: 3)
            string stepText = (it.Step * scale).ToString("0.####", CultureInfo.InvariantCulture);
            int dec = stepText.Contains(".") ? stepText.Length - stepText.IndexOf('.') - 1 : 0;
            n.DecimalPlaces = Math.Max(0, Math.Min(4, dec));
            n.Minimum = (decimal)(it.Min * scale);
            n.Maximum = (decimal)(it.Max * scale);
            n.Increment = (decimal)(it.Step * scale);
            bool integer = dec == 0 && scale == 1 && it.Step >= 1;
            Action rf = () => {
                // as the mod reads it (get_int: strtol base 0; get_float: strtof), clamped to the menu's range
                string s = settings.Get(it.Section, it.Key, it.Default);
                double v = integer || (it.Written ?? "").StartsWith("%d") ? ModValue.Int(s) : ModValue.Float(s);
                if (double.IsNaN(v)) v = it.Min;
                v = Math.Max(it.Min, Math.Min(it.Max, v)) * scale;
                n.Value = Math.Max(n.Minimum, Math.Min(n.Maximum, (decimal)v));
            };
            Fill(rf);
            n.ValueChanged += (o, e) => {
                if (refreshing) return;
                double v = (double)n.Value / scale;
                string txt = Written(it, v, integer);
                settings.Set(it.Section, it.Key, txt);
            };
            Bind(it.Section, it.Key, rf);
            if (it.Format == "percent") {
                FlowLayoutPanel f = new FlowLayoutPanel { AutoSize = true, Margin = new Padding(0) };
                f.Controls.Add(n);
                f.Controls.Add(new Label { Text = "%", AutoSize = true, Margin = new Padding(4, 6, 0, 0) });
                return f;
            }
            return n;
        }
        if (it.Type == "text" && it.Key != null) {
            TextBox tb = new TextBox { Width = 340 };
            Action rf = () => tb.Text = settings.Get(it.Section, it.Key, it.Default) ?? "";
            Fill(rf);
            tb.TextChanged += (o, e) => {
                if (!refreshing) settings.Set(it.Section, it.Key, tb.Text.Trim());
            };
            Bind(it.Section, it.Key, rf);
            return tb;
        }
        // a compound control or an action: what it does; it is set in the headset menu, where it needs the headset, a
        // gun in hand or the runtime's list
        string what = it.Help ?? "";
        Label note = new Label { AutoSize = true, ForeColor = SystemColors.GrayText, MaximumSize = new Size(760, 0), Margin = new Padding(0, 6, 0, 6),
                                 Text = what + (what != "" ? " " : "") + "(Set in the headset menu.)" };
        return note;
    }

    // ---- the Graphics tab: the render settings by key (the same items as the Mod settings tab's Display page),
    // the resolution picker (RenderHeadset + RenderScale -> RenderResolution, as the menu computes it) and the presets
    static readonly string[] GraphicsKeys = { "Render/ForceAntiAliasing", "Render/DlssQuality", "XR/Sharpness", "XR/EyeShape", "XR/EyeShapeDlss",
                                              "Render/MotionBlur", "Render/HeatHaze", "XR/RefreshRate", "Screen/CutsceneMode" };  // RefreshRate: the menu's
                                                                                                                                  // compound row (the runtime lists the rates)

    TabPage GraphicsPage() {
        TabPage p = new TabPage("Graphics");
        List<Item> items = new List<Item>();
        items.Add(new Item { Type = "header", Label = "Image" });
        foreach (string k in GraphicsKeys) {
            int s = k.IndexOf('/');
            Item it = Schema.Find(k.Substring(0, s), k.Substring(s + 1)) ?? Schema.FindCompound(k.Substring(0, s) + "." + k.Substring(s + 1));
            if (it != null) items.Add(it);
        }
        Panel panel = ItemsPanel(items);
        TableLayoutPanel t = (TableLayoutPanel)panel.Controls[0];
        AddRow(t, new Item { Type = "header", Label = "Render resolution" });
        Resolution.AddRows(this, t, settings, tips);
        AddRow(t, new Item { Type = "header", Label = "Single-pass stereo (test)" });
        SinglePass.AddRows(this, t, settings, tips);
        AddRow(t, new Item { Type = "header", Label = "Presets" });
        AddPresets(t);
        p.Controls.Add(panel);
        return p;
    }

    IEnumerable<string> HolsterWeapons(string key) {
        IEnumerable<string> sidearms = Schema.Weapons.Take(8).Select(w => w.Key);
        IEnumerable<string> longGuns = Schema.Weapons.Skip(8).Take(13).Select(w => w.Key).Concat(new[] { "ExplosiveRifle", "Blunderbuss" });
        // [Holsters] AnyWeapon: the hip and back holsters take any gun (its kind listed first)
        bool any = ModValue.Bool(settings.Get("Holsters", "AnyWeapon", null));
        // the Sawed-off fits the hips too (holster.cpp weapon_fits, 2026-10-26)
        if (key == "RightHipWeapon" || key == "LeftHipWeapon") return any ? sidearms.Concat(longGuns) : sidearms.Concat(new[] { "SawedOff" });
        if (key == "BackWeapon" || key == "LeftShoulderWeapon") return any ? longGuns.Concat(sidearms) : longGuns;
        if (key == "KnifeWeapon") return new[] { "Knife" };
        if (key == "LassoWeapon") return new[] { "Lasso" };
        if (key == "BeltWeapon") return new[] { "FireBottle", "Dynamite", "ThrowingKnife", "Tomahawk", "HolyWater", "ZombieBait", "BoomBait" };
        return Schema.Weapons.Select(w => w.Key);
    }

    void AddPresets(TableLayoutPanel t) {
        FlowLayoutPanel pf = new FlowLayoutPanel { AutoSize = true };
        foreach (Preset pr in Preset.All) {
            Preset preset = pr;
            Button b = new Button { Text = preset.Name, Width = 140, Height = 32 };
            tips.SetToolTip(b, preset.Describe());
            b.Click += (o, e) => preset.Apply(settings);
            pf.Controls.Add(b);
        }
        t.Controls.Add(new Label { Text = "Presets (as the headset menu's)", AutoSize = true, Margin = new Padding(0, 8, 8, 0) }, 0, t.RowCount);
        t.Controls.Add(pf, 1, t.RowCount);
        t.RowCount++;
    }

    // the value as the menu writes it: its printf format ("%.2f", "%.3f", "%d"), else a plain number
    static string Written(Item it, double v, bool integer) {
        string w = it.Written ?? "";
        if (w.StartsWith("%d") || w.StartsWith("%i")) return ((long)Math.Round(v)).ToString(CultureInfo.InvariantCulture);
        if (w.StartsWith("%.") && w.Length >= 4 && char.IsDigit(w[2]) && w[3] == 'f') return v.ToString("F" + w[2], CultureInfo.InvariantCulture);
        return integer ? ((long)Math.Round(v)).ToString(CultureInfo.InvariantCulture) : v.ToString("0.####", CultureInfo.InvariantCulture);
    }

    TabPage ModPage() {
        TabPage p = new TabPage("Mod settings");
        TabControl inner = new TabControl { Dock = DockStyle.Fill };
        foreach (Tab tab in Schema.Tabs) {
            TabPage ip = new TabPage(tab.Name);
            ip.Controls.Add(ItemsPanel(tab.Items));
            inner.TabPages.Add(ip);
        }
        if (Schema.Tabs.Count == 0) inner.TabPages.Add(new TabPage("(no menu schema in this build)"));
        p.Controls.Add(inner);
        return p;
    }

    TabPage ControlsPage() {
        TabPage p = new TabPage("Controls");
        controlsPic = new PictureBox { Dock = DockStyle.Fill, SizeMode = PictureBoxSizeMode.Zoom, BackColor = Color.FromArgb(28, 24, 21) };
        RedrawControls();
        p.Controls.Add(controlsPic);
        return p;
    }

    void RedrawControls() {
        if (controlsPic == null) return;
        Image old = controlsPic.Image;
        controlsPic.Image = ControlsPicture.Render(settings);
        if (old != null) old.Dispose();
    }

    public void SaveTabShots(string dir) {
        Directory.CreateDirectory(dir);
        CreateControl();
        for (int i = 0; i < tabs.TabPages.Count; ++i) {
            tabs.SelectedIndex = i;
            using (Bitmap b = new Bitmap(Width, Height)) {
                DrawToBitmap(b, new Rectangle(0, 0, Width, Height));
                b.Save(Path.Combine(dir, "tab_" + i + ".png"), System.Drawing.Imaging.ImageFormat.Png);
            }
        }
        // the whole Graphics tab, below the scroll too
        TableLayoutPanel gt = tabs.TabPages[0].Controls.Count > 0 && tabs.TabPages[0].Controls[0].Controls.Count > 0
                                  ? tabs.TabPages[0].Controls[0].Controls[0] as TableLayoutPanel : null;
        if (gt != null && gt.Width > 0 && gt.Height > 0) {
            tabs.SelectedIndex = 0;
            using (Bitmap b = new Bitmap(gt.Width, gt.Height)) {
                gt.DrawToBitmap(b, new Rectangle(0, 0, gt.Width, gt.Height));
                b.Save(Path.Combine(dir, "graphics.png"), System.Drawing.Imaging.ImageFormat.Png);
            }
        }
        TabControl inner = tabs.TabPages[1].Controls.Count > 0 ? tabs.TabPages[1].Controls[0] as TabControl : null;
        if (inner != null) {
            tabs.SelectedIndex = 1;
            for (int i = 0; i < inner.TabPages.Count; ++i) {
                inner.SelectedIndex = i;
                using (Bitmap b = new Bitmap(Width, Height)) {
                    DrawToBitmap(b, new Rectangle(0, 0, Width, Height));
                    b.Save(Path.Combine(dir, "mod_" + i + ".png"), System.Drawing.Imaging.ImageFormat.Png);
                }
                // the whole page, below the scroll too
                TableLayoutPanel t = inner.TabPages[i].Controls.Count > 0 && inner.TabPages[i].Controls[0].Controls.Count > 0
                                         ? inner.TabPages[i].Controls[0].Controls[0] as TableLayoutPanel : null;
                if (t != null && t.Width > 0 && t.Height > 0)
                    using (Bitmap b = new Bitmap(t.Width, t.Height)) {
                        t.DrawToBitmap(b, new Rectangle(0, 0, t.Width, t.Height));
                        b.Save(Path.Combine(dir, "page_" + i + ".png"), System.Drawing.Imaging.ImageFormat.Png);
                    }
            }
        }
    }

    [System.Runtime.InteropServices.DllImport("kernel32.dll", SetLastError = true)]
    static extern bool SetDefaultDllDirectories(uint flags);

    [STAThread]
    static int Main(string[] args) {
        // 2026-10-31 (r301): a DLL loaded by name from here on comes from System32 only, never from the launcher's folder
        // or the working directory (in the game folder it took RedHook's winmm.dll proxy and crashed). The framework loads
        // its own files by their full paths, and the controls' comctl32 v6 comes through its manifest (side by side)
        try {
            SetDefaultDllDirectories(0x00000800);  // LOAD_LIBRARY_SEARCH_SYSTEM32
        } catch (Exception) {
        }
        Application.EnableVisualStyles();
        Application.SetCompatibleTextRenderingDefault(false);
        try {
            return Run(args);
        } catch (Exception ex) {
            if (args.Length > 0 && args[0] == "--at-start") return 5;  // the game goes on (no box to answer: it waits for this process)
            if (args.Length > 0) {
                Console.WriteLine("FAIL: " + ex);
                return 2;
            }
            MessageBox.Show("The launcher could not start: " + ex.Message, "RDRVR Launcher", MessageBoxButtons.OK, MessageBoxIcon.Error);
            return 2;
        }
    }

    [System.Runtime.InteropServices.DllImport("user32.dll")]
    static extern IntPtr SendMessage(IntPtr h, int msg, IntPtr w, IntPtr l);

    // the wheel over every list and number, and their inner edit boxes: nothing may change
    public int WheelTest() {
        CreateControl();
        List<Control> all = new List<Control>();
        Action<Control> walk = null;
        walk = c => {
            foreach (Control k in c.Controls) {
                if (k is WheelCombo || k is WheelNumber) all.Add(k);
                walk(k);
            }
        };
        walk(this);
        int changed = 0, sent = 0;
        foreach (Control c in all) {
            string before = c is NumericUpDown ? ((NumericUpDown)c).Value.ToString(CultureInfo.InvariantCulture) : ((ComboBox)c).SelectedIndex.ToString(CultureInfo.InvariantCulture);
            List<IntPtr> hs = new List<IntPtr> { c.Handle };
            foreach (Control k in c.Controls) hs.Add(k.Handle);
            foreach (IntPtr h in hs)
                foreach (int delta in new[] { 120, -120 }) {
                    SendMessage(h, 0x020A, (IntPtr)(delta << 16), IntPtr.Zero);
                    ++sent;
                }
            Application.DoEvents();
            string after = c is NumericUpDown ? ((NumericUpDown)c).Value.ToString(CultureInfo.InvariantCulture) : ((ComboBox)c).SelectedIndex.ToString(CultureInfo.InvariantCulture);
            if (before != after) ++changed;
        }
        // the control case: a plain NumericUpDown does change by the same messages (the test can fail)
        NumericUpDown plain = new NumericUpDown { Minimum = 0, Maximum = 100, Value = 50 };
        Controls.Add(plain);
        plain.CreateControl();
        SendMessage(plain.Handle, 0x020A, (IntPtr)(120 << 16), IntPtr.Zero);
        foreach (Control k in plain.Controls) SendMessage(k.Handle, 0x020A, (IntPtr)(120 << 16), IntPtr.Zero);
        Application.DoEvents();
        bool control = plain.Value != 50;
        Controls.Remove(plain);
        bool ok = changed == 0 && settings.Pending == 0 && control;
        Console.WriteLine((ok ? "PASS" : "FAIL") + ": the wheel over " + all.Count + " lists and numbers (" + sent + " messages): " + changed +
                          " changed, " + settings.Pending + " edits; a plain NumericUpDown " + (control ? "changed" : "did not change (the test proves nothing)"));
        return ok ? 0 : 1;
    }

    static int Run(string[] args) {
        Schema.Load();
        if (args.Length >= 1 && args[0] == "--at-start") {
            int pid = 0, timeout = 30;
            if (args.Length > 1) int.TryParse(args[1], NumberStyles.Integer, CultureInfo.InvariantCulture, out pid);
            if (args.Length > 2) int.TryParse(args[2], NumberStyles.Integer, CultureInfo.InvariantCulture, out timeout);
            for (int i = 3; i < args.Length; ++i) {
                if (args[i] == "--test-set" && i + 1 < args.Length) {
                    string a = args[++i];
                    int dot = a.IndexOf('.'), eq = a.IndexOf('=');
                    if (dot > 0 && eq > dot + 1) MainForm.TestSets.Add(new KeyValuePair<string, string>(a.Substring(0, eq), a.Substring(eq + 1)));
                } else if (args[i] == "--test-press" && i + 2 < args.Length) {
                    TestPress = args[++i] == "close" ? "close" : "play";
                    double.TryParse(args[++i], NumberStyles.Float, CultureInfo.InvariantCulture, out TestPressAfter);
                } else if (args[i] == "--test-hold" && i + 1 < args.Length) {
                    double.TryParse(args[++i], NumberStyles.Float, CultureInfo.InvariantCulture, out TestHoldAfter);
                } else if (args[i] == "--test-no-countdown") {
                    TestNoCountdown = true;
                }
            }
            if (pid <= 0) return 4;
            AtStart.Open(pid, timeout);
            if (!AtStart.GameAlive()) return 4;
            if (!AtStart.Linked) return 5;  // no link to the game (its events): it goes on at its own deadline; nothing saved here could apply
            // an exception in a handler ends this process (Main returns 5) instead of WinForms' box, which nobody may answer
            Application.SetUnhandledExceptionMode(UnhandledExceptionMode.ThrowException);
            Application.Run(new MainForm());
            return AtStart.ExitCode;
        }
        if (args.Length >= 2 && args[0] == "--render-controls") {
            Settings s = new Settings();
            bool st;
            s.Load(Game.FindGameDir(out st));
            using (Bitmap b = ControlsPicture.Render(s)) b.Save(args[1], System.Drawing.Imaging.ImageFormat.Png);
            return 0;
        }
        if (args.Length >= 2 && args[0] == "--screenshot") {
            using (MainForm f = new MainForm()) {
                f.Show();
                Application.DoEvents();
                f.SaveTabShots(args[1]);
                f.Hide();
            }
            return 0;
        }
        if (args.Length >= 2 && args[0] == "--selftest") return SelfTest.Run(args[1]);
        if (args.Length >= 1 && args[0] == "--wheeltest")
            using (MainForm f = new MainForm()) {
                f.Show();
                Application.DoEvents();
                int r = f.WheelTest();
                f.Hide();
                return r;
            }
        Application.Run(new MainForm());
        return 0;
    }
}
}
