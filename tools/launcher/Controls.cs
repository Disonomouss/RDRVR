// RDRVR Launcher: the controls picture, drawn from the settings the game will use ([Controls] remap and LeftHanded,
// [Holsters] Mode, [XR] Hud, [Controls] WristSatchel, [Comfort] TurnMode). The game actions per pad button are the
// game's own (input_foot.xml; research notes); the chords and gestures are the mod's (src/core/controls.cpp,
// holster.cpp, actions.cpp; the README's controls section).
using System;
using System.Collections.Generic;
using System.Drawing;
using System.Drawing.Drawing2D;

namespace RdrvrLauncher {

public static class ControlsPicture {
    public const int W = 1500, H = 1320;  // (2026-11-02: the off-hand work's lines; 2026-11-04: the gestures since)

    static readonly Color Bg = Color.FromArgb(28, 24, 21), Panel = Color.FromArgb(42, 36, 31), Ink = Color.FromArgb(236, 226, 210),
                          Dim = Color.FromArgb(170, 158, 140), Accent = Color.FromArgb(214, 120, 58), Body = Color.FromArgb(70, 63, 57),
                          Part = Color.FromArgb(112, 102, 92);

    public static string Pad(string b) {
        switch ((b ?? "").Trim().ToUpperInvariant()) {
            case "A": return "Sprint · spur the horse";
            case "B": return "Interact · reload · greet";
            case "X": return "Jump";
            case "Y": return "Use · mount · hogtie";
            case "LB": return "Weapon wheel";
            case "RB": return "Cover · block · Dead Eye paint";
            case "LT": return "Aim";
            case "RT": return "Fire · punch";
            case "L3": return "Crouch";
            case "R3": return "Dead Eye";
            case "BACK": return "Map · satchel";
            case "START": return "Pause";
            case "DPADUP": return "D-pad up";
            case "DPADDOWN": return "D-pad down";
            case "DPADLEFT": return "D-pad left";
            case "DPADRIGHT": return "D-pad right";
            case "NONE": return "(nothing)";
        }
        return b;
    }

    class Callout {
        public PointF At;
        public string Name, Text;
    }

    public static Bitmap Render(Settings s) {
        Func<string, string, string> get = (k, d) => s.Get("Controls", k, d);
        bool lefty = get("LeftHanded", "0") == "1";
        string hud = s.Get("XR", "Hud", "wrist");
        bool satchel = get("WristSatchel", "1") == "1";
        string holsters = s.Get("Holsters", "Mode", "holsters");
        string turn = s.Get("Comfort", "TurnMode", "smooth");
        // (2026-11-02, GOAL-older-items: the gestures as the off-hand work left them, by the settings)
        bool keep = s.Get("Holsters", "HoldToKeep", "0") == "1";
        bool carry = s.Get("Hands", "CarryByForegrip", "1") == "1";
        bool dual = s.Get("Hands", "DualWield", "0") == "1";  // (the DLL's own defaults where the ini has none)
        bool items = s.Get("Hands", "OffHandItems", "0") == "1";
        bool cock = s.Get("Reload", "RevolverControl", "open") == "cock";
        bool rodChest = s.Get("Reload", "RamrodAtChest", "1") == "1", rodGun = s.Get("Reload", "RamrodFrom", "gun") != "chest";
        bool melee = s.Get("Gestures", "GunMelee", "0") == "1", hat = s.Get("Gestures", "TipHat", "1") == "1";
        string lasso = s.Get("Gestures", "LassoThrow", "trigger");
        // (2026-11-04, before 0.8.12: the user, "Has the controls image in launcher been updated?") the gestures since
        bool keepItems = keep && s.Get("Holsters", "HoldItems", "1") == "1";
        bool copy = s.Get("Hands", "DualWieldCopy", "0") == "1";
        bool boltTrig = s.Get("Reload", "OffHandBoltTrigger", "1") == "1", carryTrig = s.Get("Reload", "CarriedBoltTrigger", "1") == "1";
        bool fan = cock && s.Get("Reload", "FanHammer", "1") == "1";
        bool dropLit = s.Get("Gestures", "ThrowByGrip", "0") == "1" && s.Get("Gestures", "DropLitSlow", "1") == "1";
        bool wheelPad = s.Get("Holsters", "WheelStickDpad", "1") == "1";
        bool scope = s.Get("Scope", "Enabled", "0") == "1";
        bool room = s.Get("XR", "RoomScale", "0") == "1", crouch = s.Get("XR", "PhysicalCrouch", "0") == "1";

        Bitmap bmp = new Bitmap(W, H);
        using (Graphics g = Graphics.FromImage(bmp)) {
            g.SmoothingMode = SmoothingMode.AntiAlias;
            g.TextRenderingHint = System.Drawing.Text.TextRenderingHint.ClearTypeGridFit;
            g.Clear(Bg);
            using (Font title = new Font("Segoe UI Semibold", 26f), sub = new Font("Segoe UI", 12f), name = new Font("Segoe UI Semibold", 12.5f),
                   text = new Font("Segoe UI", 12f), small = new Font("Segoe UI", 11f), head = new Font("Segoe UI Semibold", 14f))
            using (SolidBrush ink = new SolidBrush(Ink), dim = new SolidBrush(Dim), accent = new SolidBrush(Accent)) {
                g.DrawString("RDRVR controls", title, ink, 40, 24);
                g.DrawString("Touch-style controllers, as your settings stand" + (lefty ? " (left-handed: the two hands' roles swapped)" : "") +
                             ". Index: both B buttons together = the menu button. Vive: the trackpad is the stick.", sub, dim, 44, 74);

                // the two controllers: the physical left one carries the left-hand roles (the right's when left-handed)
                DrawController(g, new PointF(600, 360), true, lefty);
                DrawController(g, new PointF(900, 360), false, lefty);

                // the roles each physical controller carries
                bool leftIsMove = !lefty;
                List<Callout> L = new List<Callout>(), R = new List<Callout>();
                List<Callout> moveSide = leftIsMove ? L : R, aimSide = leftIsMove ? R : L;
                float lx = 600, rx = 900;
                float mx = leftIsMove ? lx : rx, ax = leftIsMove ? rx : lx;
                moveSide.Add(new Callout { At = new PointF(mx + (leftIsMove ? -6 : 6), 300), Name = "Stick", Text = "Move · click: " + Pad(get("LeftStickClick", "L3")) + " (on a horse: brake)" });
                moveSide.Add(new Callout { At = new PointF(mx + (leftIsMove ? 34 : -34), 262), Name = leftIsMove ? "X" : "A", Text = Pad(get("LeftX", "X")) });
                moveSide.Add(new Callout { At = new PointF(mx + (leftIsMove ? 54 : -54), 226), Name = leftIsMove ? "Y" : "B",
                                           Text = Pad(get("LeftY", "Y")) + (hud == "wrist" ? " · at your wrist: hold for the RDRVR menu" + (satchel ? ", double tap for the satchel" : "") : "") });
                moveSide.Add(new Callout { At = new PointF(mx + (leftIsMove ? -34 : 34), 250), Name = "Menu", Text = "Tap: pause · hold: the RDRVR menu" });
                moveSide.Add(new Callout { At = new PointF(mx + (leftIsMove ? 38 : -38), 420), Name = "Trigger", Text = Pad(get("LeftTrigger", "LT")) });
                moveSide.Add(new Callout { At = new PointF(mx + (leftIsMove ? 22 : -22), 520), Name = "Grip",
                                           Text = "Grab (holsters, ammo, guns, props) · elsewhere: " + Pad(get("LeftGrip", "LB")) });
                aimSide.Add(new Callout { At = new PointF(ax + (leftIsMove ? 6 : -6), 300), Name = "Stick", Text = "Turn (" + turn + ") · click: " + Pad(get("RightStickClick", "R3")) });
                aimSide.Add(new Callout { At = new PointF(ax + (leftIsMove ? -34 : 34), 262), Name = leftIsMove ? "A" : "X", Text = Pad(get("RightA", "A")) });
                aimSide.Add(new Callout { At = new PointF(ax + (leftIsMove ? -54 : 54), 226), Name = leftIsMove ? "B" : "Y", Text = Pad(get("RightB", "B")) });
                aimSide.Add(new Callout { At = new PointF(ax + (leftIsMove ? -38 : 38), 420), Name = "Trigger", Text = Pad(get("RightTrigger", "RT")) + " (the mod aims for you)" });
                aimSide.Add(new Callout { At = new PointF(ax + (leftIsMove ? -22 : 22), 520), Name = "Grip",
                                          Text = "Grab (holsters, ammo, guns, props) · elsewhere: " + Pad(get("RightGrip", "RB")) });

                DrawCallouts(g, L, true, name, text, ink, dim);
                DrawCallouts(g, R, false, name, text, ink, dim);

                // the chords and the gestures
                RectangleF box = new RectangleF(40, 640, W - 80, H - 670);
                using (SolidBrush panel = new SolidBrush(Panel)) g.FillRectangle(panel, box);
                string[] chords = {
                    "Both stick clicks held 1 s: recentre (keyboard: Scroll Lock)",
                    "Hold Menu + right stick: the D-pad · hold Menu + A: map and satchel",
                    "Keyboard F7: the RDRVR menu",
                    // (2026-11-04: with the grips mapped to nothing, the paint's RB is the right stick flicked up, FlickUpRB)
                    "Dead Eye: stick click while aiming, then paint " + (get("RightGrip", "RB") == "RB" ? "with the grip"
                                                                         : s.Get("Controls", "FlickUpRB", "1") == "1" ? "by flicking the right stick up" : "with RB") +
                        ", fire with the trigger",
                    "Both triggers with empty hands: fists; swing to punch",
                    "Whistle: a hand at your mouth + its trigger · guard: both fists before the face",
                    hat ? "Tip your hat: the right hand at the brim + its trigger (John greets)" : null,
                    room ? "Room-scale: step in your room and John steps with you" : null,
                    crouch ? "Crouch for real to crouch" : null,
                };
                string draw = holsters == "wheel"
                    ? "Holsters: weapon wheel mode, hold a grip and move toward a weapon" + (wheelPad ? "; that hand's stick picks another of its kind" : "")
                    : "Holsters: grip at a holster to draw" + (keep ? "; keep holding the grip to keep the gun" + (keepItems ? " or item" : "") + ", let go to put it away"
                                                                    : ", let go there to put it away");
                string[] gestures = {
                    draw,
                    holsters == "wheel" ? null : "   right hip / left hip: sidearms · right back / left shoulder: long guns · belt: knife, lasso, throwables",
                    "Reload: take a round (or a magazine) from your chest, bring it to the gun's loading point · grip your chest with the gun hand: full reload",
                    "Two hands: grip the gun's foregrip with the other hand" + (carry ? "; let go with the gun hand and that hand carries it, grip it again to take it back" +
                                                                                         (carryTrig ? " (its bolt: the gun hand's trigger)" : "") : ""),
                    "Levers: hold the right stick down, the gun down then up · " + (boltTrig ? "bolts: your other hand's trigger at the bolt · pumps, breeches and slides: your other hand"
                                                                                               : "bolts, pumps, breeches and slides: your other hand"),
                    cock ? "Single-action revolvers: cock with the stick or your thumb on the hammer" + (fan ? "; fan it: your other palm swept back across the hammer, the trigger held" : "") : null,
                    rodChest ? "Blunderbuss: a round down the muzzle, then the ramrod " + (rodGun ? "from under its barrel" : "from your chest") + ": in and out" : null,
                    dual ? "Second gun: draw a sidearm with your other hand; its own trigger fires it" : null,
                    copy ? "   the other hip with a sidearm in hand: a second of it" : null,
                    items ? "Free hand: the knife or a throwable from your belt beside a gun" : null,
                    "Throw: hold the grip, swing, let go" + (dropLit ? " (lit and let go slowly: it drops)" : "") + " · lasso: " +
                        (lasso == "lock" ? "LT locks on and spins it, RT throws" : lasso == "swing" ? "swing it" : "its trigger throws") + ", yank back to pull",
                    scope ? "Scope: bring the rifle's eyepiece to your eye" : null,
                    melee ? "Gun-butt melee: swing the gun at someone" : null,
                };
                g.DrawString("Chords and keys", head, accent, box.X + 20, box.Y + 14);
                g.DrawString("Gestures", head, accent, box.X + box.Width / 2 + 10, box.Y + 14);
                Column(g, chords, small, ink, box.X + 20, box.Y + 50, box.Width / 2 - 40);
                Column(g, gestures, small, ink, box.X + box.Width / 2 + 10, box.Y + 50, box.Width / 2 - 30);
            }
        }
        return bmp;
    }

    // a list of lines, each wrapped to the width and spaced by its own height; a line starting with spaces continues the one above
    static void Column(Graphics g, string[] lines, Font f, Brush b, float x, float y, float w) {
        foreach (string l in lines) {
            if (l == null) continue;  // (a line for an option that is off)
            bool cont = l.StartsWith(" ");
            string t = cont ? "    " + l.Trim() : "•  " + l;
            SizeF sz = g.MeasureString(t, f, (int)w);
            g.DrawString(t, f, b, new RectangleF(x, y - (cont ? 8 : 0), w, sz.Height + 4));
            y += sz.Height + (cont ? 6 : 14);
        }
    }

    // A Touch-style controller seen from the front: the ring and face, the stick, two face buttons and the menu
    // button, the trigger and the grip drawn on the inner side
    static void DrawController(Graphics g, PointF c, bool left, bool lefty) {
        float s = left ? 1f : -1f;
        using (SolidBrush body = new SolidBrush(Body), part = new SolidBrush(Part), dark = new SolidBrush(Color.FromArgb(48, 43, 39)))
        using (Pen edge = new Pen(Color.FromArgb(140, 128, 115), 2f), ring = new Pen(Color.FromArgb(120, 110, 100), 9f)) {
            // the handle
            using (GraphicsPath h = new GraphicsPath()) {
                h.AddBezier(c.X - 40, c.Y - 40, c.X - 70, c.Y + 80, c.X - 40, c.Y + 190, c.X + 5 * s, c.Y + 230);
                h.AddBezier(c.X + 5 * s, c.Y + 230, c.X + 50, c.Y + 190, c.X + 60, c.Y + 60, c.X + 40, c.Y - 40);
                h.CloseFigure();
                g.FillPath(body, h);
                g.DrawPath(edge, h);
            }
            // the grip button (inner side of the handle) and the trigger (inner front)
            g.FillEllipse(part, c.X - 18 * s - 14, c.Y + 120, 28, 70);
            g.FillEllipse(part, c.X + 10 * s - 16, c.Y + 30, 32, 60);
            // the face and the tracking ring
            g.FillEllipse(body, c.X - 95, c.Y - 165, 190, 190);
            g.DrawEllipse(edge, c.X - 95, c.Y - 165, 190, 190);
            g.DrawEllipse(ring, c.X - 125 - 20 * s, c.Y - 205, 230, 230);
            // the stick, the two buttons, the menu (or system) button
            g.FillEllipse(dark, c.X - 6 * s - 30, c.Y - 90, 60, 60);
            g.FillEllipse(part, c.X - 6 * s - 20, c.Y - 80, 40, 40);
            g.FillEllipse(part, c.X + 34 * s - 16, c.Y - 114, 32, 32);
            g.FillEllipse(part, c.X + 54 * s - 16, c.Y - 150, 32, 32);
            g.FillEllipse(dark, c.X - 34 * s - 9, c.Y - 120, 18, 18);
        }
    }

    // Labels on the picture's outer sides, joined to their part by a line
    static void DrawCallouts(Graphics g, List<Callout> cs, bool leftSide, Font name, Font text, Brush ink, Brush dim) {
        cs.Sort((a, b) => a.At.Y.CompareTo(b.At.Y));
        float colX = leftSide ? 30 : 1120, colW = 350, y0 = 120, step = 84;
        using (Pen line = new Pen(Color.FromArgb(214, 120, 58), 1.6f)) {
            for (int i = 0; i < cs.Count; ++i) {
                float y = y0 + i * step;
                PointF anchor = new PointF(leftSide ? colX + colW + 6 : colX - 6, y + 12);
                g.DrawLine(line, anchor, cs[i].At);
                g.FillEllipse(Brushes.White, cs[i].At.X - 3, cs[i].At.Y - 3, 6, 6);
                StringFormat f = new StringFormat { Alignment = leftSide ? StringAlignment.Far : StringAlignment.Near };
                g.DrawString(cs[i].Name, name, ink, new RectangleF(colX, y, colW, 24), f);
                g.DrawString(cs[i].Text, text, dim, new RectangleF(colX, y + 22, colW, 62), f);
            }
        }
    }
}
}
