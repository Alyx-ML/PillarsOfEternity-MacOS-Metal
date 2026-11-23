// The port's game controller layer.
//
// The runtime reads the controller with the GameController framework of
// macOS (runtime/src/rt_gamepad.m). The runtime itself moves the cursor
// with the left stick, gives the left mouse button on A and the right
// mouse button on B, and turns the mouse wheel with up and down on the
// directional pad. GraphicsOptions.Tick calls PortController.Tick once
// after each presented frame, on the main thread. Tick reads the state of
// the controller (PortPadRead) and does the rest:
//
//   X              Select All          Y              Pause
//   LB / RB        previous / next party member
//   RT (hold)      Highlight (hold)    LT             radial ability menu
//   left / right   Slow / Fast mode (directional pad)
//   Menu           game menu           View (tap)     Inventory
//   L3             Stealth             R3             Follow camera
//   right stick    camera pan          View (hold)    help overlay
//
// Snap mode: while a game window has the focus (inventory, character
// sheet, dialogue, stores, message boxes; UIWindowManager.IsFocused), the
// directional pad moves the cursor to the next control of the window in
// that direction (a collider with a click handler that a raycast of the UI
// camera reaches), and the right stick scrolls (the runtime turns it into
// the mouse wheel, PortPadSnap). A small glyph of the A button shows next
// to the control under the cursor. The left stick still moves the cursor.
//
// Glyphs: the runtime draws the SF Symbols glyph of each control of the
// current controller (PortPadGlyph), so the hints show the buttons of
// that controller (Xbox, PlayStation, Nintendo). The radial menu shows the
// buttons for use, back and close under the ring. Holding View shows the
// help overlay with all controls and the game's names of the commands.
//
// Each command presses the key that the player bound to it in the game's
// Controls options (GameState.Controls), with its modifier keys, while the
// button is down (PortPadKey). A command without a key binding, or with a
// key that a Mac keyboard does not have, does nothing.
//
// The radial ability menu shows the buttons of the game's ability bar
// (UIAbilityBar) for the selected party member in a ring at the center of
// the screen: their icons, and the name of the selected one in the middle.
// While the menu is open, the left stick selects a button and the runtime
// does not move the cursor; A and B do not click (PortPadCapture).
//   A    uses the button (UIAbilityBarButton.Trigger, as a click on the
//        bar): a button with a row of more buttons (spell levels, item
//        groups) opens that row in the ring; another button uses its
//        ability or item and closes the menu. An ability with a target
//        then waits for the target: move the cursor and press A.
//   B    goes back to the previous row, or closes the menu.
//   LT   closes the menu.
// The menu closes when the selected party member changes or the ability
// bar is not shown. A button that the bar shows as disabled is dimmed and
// A does nothing on it.
//
// Errors: an exception reports with PortGfxFail, which stops the game with
// the message.
//
// WARNING: this assembly is compiled against the game's assemblies. Do not
// distribute, upload or commit the compiled assembly or its AOT image.
using System;
using System.Collections.Generic;
using System.Reflection;
using System.Runtime.InteropServices;
using UnityEngine;

namespace PortGraphics
{
    public static class PortController
    {
        // Bits of PortPadRead (runtime/src/rt_gamepad.m has the same values).
        const int PadA = 1 << 0, PadB = 1 << 1, PadX = 1 << 2, PadY = 1 << 3, PadLB = 1 << 4, PadRB = 1 << 5,
            PadLT = 1 << 6, PadRT = 1 << 7, PadUp = 1 << 8, PadDown = 1 << 9, PadLeft = 1 << 10, PadRight = 1 << 11,
            PadMenu = 1 << 12, PadView = 1 << 13, PadL3 = 1 << 14, PadR3 = 1 << 15;
        // Glyphs of PortPadGlyph: 0..15 the bit numbers above, then these.
        const int GlyphA = 0, GlyphB = 1, GlyphX = 2, GlyphY = 3, GlyphLB = 4, GlyphRB = 5, GlyphLT = 6, GlyphRT = 7,
            GlyphLeft = 10, GlyphRight = 11, GlyphMenu = 12, GlyphView = 13, GlyphL3 = 14, GlyphR3 = 15,
            GlyphLeftStick = 16, GlyphRightStick = 17, GlyphDpad = 18, GlyphCount = 19;

        [DllImport("portgfx", CallingConvention = CallingConvention.StdCall)]
        static extern int PortPadRead(float[] axes);

        [DllImport("portgfx", CallingConvention = CallingConvention.StdCall)]
        static extern void PortPadCapture(int on);

        [DllImport("portgfx", CallingConvention = CallingConvention.StdCall)]
        static extern int PortPadKey(int unityKeyCode, int down);

        [DllImport("portgfx", CallingConvention = CallingConvention.StdCall)]
        static extern void PortPadSnap(int on);

        [DllImport("portgfx", CallingConvention = CallingConvention.StdCall)]
        static extern void PortPadWarp(int x, int y, int width, int height);

        [DllImport("portgfx", CallingConvention = CallingConvention.StdCall)]
        static extern int PortPadGlyphs();

        [DllImport("portgfx", CallingConvention = CallingConvention.StdCall)]
        static extern int PortPadGlyph(int glyph, int size, [Out] Color32[] pixels);

        [DllImport("portgfx", CallingConvention = CallingConvention.StdCall, CharSet = CharSet.Ansi)]
        static extern void PortGfxFail(string message);

        // A command of the game on a controller input, and the keys that it
        // holds down (empty while the input is up).
        sealed class Command
        {
            public readonly MappedControl Control;
            public readonly List<int> Held = new List<int>();
            public Command(MappedControl control)
            {
                Control = control;
            }
        }

        // Button commands: the bit, then the command.
        // View is not here: Help handles it (tap: Inventory, hold: help).
        static readonly int[] ButtonBits = { PadX, PadY, PadLB, PadRB, PadRT, PadLeft, PadRight, PadMenu, PadL3, PadR3 };
        static readonly int[] ButtonGlyphs = { GlyphX, GlyphY, GlyphLB, GlyphRB, GlyphRT, GlyphLeft, GlyphRight, GlyphMenu, GlyphL3, GlyphR3 };
        static readonly Command[] ButtonCommands =
        {
            new Command(MappedControl.SELECT_ALL), new Command(MappedControl.PAUSE),
            new Command(MappedControl.PREVIOUS_COMPANION), new Command(MappedControl.NEXT_COMPANION),
            new Command(MappedControl.HIGHLIGHT_HOLD), new Command(MappedControl.SLOW_TOGGLE),
            new Command(MappedControl.FAST_TOGGLE), new Command(MappedControl.OPTIONS),
            new Command(MappedControl.STEALTH_TOGGLE), new Command(MappedControl.FOLLOW_CAM),
        };
        static readonly Command Inventory = new Command(MappedControl.INVENTORY);
        // Camera pan on the right stick: left, right, up, down.
        static readonly Command[] PanCommands =
        {
            new Command(MappedControl.PAN_CAMERA_LEFT), new Command(MappedControl.PAN_CAMERA_RIGHT),
            new Command(MappedControl.PAN_CAMERA_UP), new Command(MappedControl.PAN_CAMERA_DOWN),
        };
        // Stick deflection that presses a pan key, and the deflection
        // below which it releases the key.
        const float PanPress = 0.35f, PanRelease = 0.25f;
        // Stick deflection that selects a button of the radial menu.
        const float MenuSelect = 0.5f;

        static readonly float[] s_Axes = new float[6];
        static int s_Prev;
        static bool s_Connected;

        public static void Tick()
        {
            try
            {
                int b = PortPadRead(s_Axes);
                if (b == -1)
                {
                    if (s_Connected) Disconnect();
                    return;
                }
                s_Connected = true;
                int down = b & ~s_Prev, up = s_Prev & ~b;
                s_Prev = b;
                Glyphs.Tick();
                Help.Tick(b, down, up);
                Snap.Tick(b, down, Menu.IsOpen);
                // In the snap mode the directional pad and the right stick
                // belong to the window.
                int commands = Snap.On ? b & ~(PadLeft | PadRight) : b;
                for (int i = 0; i < ButtonBits.Length; i++) Hold(ButtonCommands[i], (commands & ButtonBits[i]) != 0);
                float rx = Snap.On ? 0f : s_Axes[2], ry = Snap.On ? 0f : s_Axes[3];
                Pan(0, -rx);
                Pan(1, rx);
                Pan(2, ry);
                Pan(3, -ry);
                Menu.Tick(down, s_Axes[0], s_Axes[1]);
            }
            catch (Exception e)
            {
                PortGfxFail("PortController.Tick: " + e.ToString());
            }
        }

        static void Disconnect()
        {
            s_Connected = false;
            s_Prev = 0;
            foreach (Command c in ButtonCommands) Hold(c, false);
            foreach (Command c in PanCommands) Hold(c, false);
            Help.Reset();
            Snap.Reset();
            Menu.Close();
        }

        static void Pan(int i, float v)
        {
            Command c = PanCommands[i];
            if (c.Held.Count == 0 ? v > PanPress : v > PanRelease)
                Hold(c, true);
            else
                Hold(c, false);
        }

        // Press (or release) the keys of the first key binding of the
        // command: the modifier keys first, then the key.
        static void Hold(Command c, bool on)
        {
            if (on == (c.Held.Count != 0)) return;
            if (!on)
            {
                for (int i = c.Held.Count - 1; i >= 0; i--) Key(c.Held[i], false);
                c.Held.Clear();
                return;
            }
            ControlMapping map = GameState.Controls;
            if (map == null || map.Controls == null || (int)c.Control >= map.Controls.Length)
                throw new InvalidOperationException("the game has no key bindings for " + c.Control);
            List<KeyControl> binds = map.Controls[(int)c.Control];
            if (binds != null)
                foreach (KeyControl k in binds)
                {
                    if (k.KeyCode == KeyCode.None) continue;
                    if (k.CtrlKey) c.Held.Add((int)KeyCode.LeftControl);
                    if (k.ShiftKey) c.Held.Add((int)KeyCode.LeftShift);
                    if (k.AltKey) c.Held.Add((int)KeyCode.LeftAlt);
                    c.Held.Add((int)k.KeyCode);
                    break;
                }
            if (c.Held.Count == 0) return;
            foreach (int k in c.Held)
                if (PortPadKey(k, 1) == 0)
                {
                    // The game allows keys that a Mac keyboard does not
                    // have (Pause, Scroll Lock): release what was pressed.
                    for (int i = c.Held.IndexOf(k) - 1; i >= 0; i--) Key(c.Held[i], false);
                    c.Held.Clear();
                    return;
                }
        }

        static void Key(int code, bool down)
        {
            if (PortPadKey(code, down ? 1 : 0) == 0)
                throw new InvalidOperationException("the runtime has no key for " + (KeyCode)code);
        }

        // ---------------------------------------------------------------- radial ability menu

        static class Menu
        {
            // The ring: icon size (the size of an ability bar icon) times
            // these factors.
            const float RingMin = 2.4f, SlotSpacing = 1.35f, SelectedScale = 1.3f;
            const int Depth = 2000;

            static GameObject s_Root;           // the menu, null while closed
            static GameObject s_Owner;          // the party member of the bar
            static readonly List<UIAbilityBarButton> s_Buttons = new List<UIAbilityBarButton>();
            static readonly List<UITexture> s_Icons = new List<UITexture>();
            static readonly List<int> s_OpenedLevels = new List<int>(); // subrow levels that the menu opened
            static UITexture s_Back;
            static UILabel s_Name;
            static Texture2D s_DiscTexture;
            static int s_Selected = -1;
            static float s_IconSize;

            public static bool IsOpen
            {
                get { return s_Root != null; }
            }

            public static void Tick(int down, float lx, float ly)
            {
                if (s_Root == null)
                {
                    if ((down & PadLT) != 0) Open();
                    return;
                }
                UIAbilityBar bar = UIAbilityBar.Instance;
                if (bar == null || !bar.gameObject.activeInHierarchy || UIAbilityBar.GetSelectedForBars() != s_Owner)
                {
                    Close();
                    return;
                }
                if ((down & PadLT) != 0)
                {
                    Close();
                    return;
                }
                if (lx * lx + ly * ly >= MenuSelect * MenuSelect && s_Buttons.Count > 0)
                {
                    // 0 at the top, clockwise.
                    float a = Mathf.Atan2(lx, ly);
                    if (a < 0f) a += 2f * Mathf.PI;
                    int n = s_Buttons.Count;
                    Select(Mathf.RoundToInt(a / (2f * Mathf.PI / n)) % n);
                }
                if ((down & PadB) != 0)
                {
                    Back();
                    return;
                }
                if ((down & PadA) != 0 && s_Selected >= 0) Use(s_Buttons[s_Selected]);
            }

            static void Open()
            {
                UIAbilityBar bar = UIAbilityBar.Instance;
                if (bar == null || !bar.gameObject.activeInHierarchy) return;
                GameObject owner = UIAbilityBar.GetSelectedForBars();
                if (owner == null) return;
                List<UIAbilityBarButton> buttons = TopButtons(bar);
                if (buttons.Count == 0) return;
                s_Owner = owner;
                s_OpenedLevels.Clear();
                Build(bar, buttons);
                PortPadCapture(1);
            }

            public static void Close()
            {
                UIAbilityBar bar = UIAbilityBar.Instance;
                if (bar != null)
                    for (int i = s_OpenedLevels.Count - 1; i >= 0; i--) bar.HideSubrow(s_OpenedLevels[i]);
                s_OpenedLevels.Clear();
                Clear();
                s_Owner = null;
                PortPadCapture(0);
            }

            static void Back()
            {
                UIAbilityBar bar = UIAbilityBar.Instance;
                if (s_OpenedLevels.Count == 0)
                {
                    Close();
                    return;
                }
                int level = s_OpenedLevels[s_OpenedLevels.Count - 1];
                s_OpenedLevels.RemoveAt(s_OpenedLevels.Count - 1);
                bar.HideSubrow(level);
                List<UIAbilityBarButton> buttons = s_OpenedLevels.Count == 0 ? TopButtons(bar)
                                                                             : LevelButtons(bar, s_OpenedLevels[s_OpenedLevels.Count - 1]);
                if (buttons.Count == 0)
                {
                    Close();
                    return;
                }
                Build(bar, buttons);
            }

            static void Use(UIAbilityBarButton button)
            {
                UIAbilityBar bar = UIAbilityBar.Instance;
                if (button == null || button.Disabled) return;
                if (button.ActionIsSubrowWithContent())
                {
                    button.Trigger();
                    UIAbilityBarButtonSet[] sets = button.SubrowSets;
                    List<UIAbilityBarButton> buttons = new List<UIAbilityBarButton>();
                    int level = -1;
                    if (sets != null)
                        foreach (UIAbilityBarButtonSet set in sets)
                            if (set != null && set.Active)
                            {
                                level = set.SubLevel;
                                AddButtons(set, buttons);
                            }
                    if (level < 0 || buttons.Count == 0)
                    {
                        // The row has no usable buttons: leave the bar as
                        // it was.
                        if (level >= 0) bar.HideSubrow(level);
                        return;
                    }
                    s_OpenedLevels.Add(level);
                    Build(bar, buttons);
                    return;
                }
                button.Trigger();
                Close();
            }

            // The buttons of the sets of the first row, from left to right.
            static List<UIAbilityBarButton> TopButtons(UIAbilityBar bar)
            {
                return LevelButtons(bar, 0);
            }

            static List<UIAbilityBarButton> LevelButtons(UIAbilityBar bar, int level)
            {
                Component[] all = bar.GetComponentsInChildren(typeof(UIAbilityBarButtonSet));
                List<UIAbilityBarButtonSet> sets = new List<UIAbilityBarButtonSet>();
                foreach (Component c in all)
                {
                    UIAbilityBarButtonSet set = (UIAbilityBarButtonSet)c;
                    if (set.Active && set.SubLevel == level) sets.Add(set);
                }
                // Insertion sort by screen position.
                for (int i = 1; i < sets.Count; i++)
                {
                    UIAbilityBarButtonSet s = sets[i];
                    int j = i - 1;
                    while (j >= 0 && sets[j].transform.position.x > s.transform.position.x)
                    {
                        sets[j + 1] = sets[j];
                        j--;
                    }
                    sets[j + 1] = s;
                }
                List<UIAbilityBarButton> buttons = new List<UIAbilityBarButton>();
                foreach (UIAbilityBarButtonSet set in sets) AddButtons(set, buttons);
                return buttons;
            }

            static void AddButtons(UIAbilityBarButtonSet set, List<UIAbilityBarButton> to)
            {
                int n = set.ButtonCount;
                for (int i = 0; i < n; i++)
                {
                    UIAbilityBarButton b = set.GetButtonByIndex(i);
                    if (b == null || !b.gameObject.activeInHierarchy || b.Icon == null || b.Icon.mainTexture == null) continue;
                    to.Add(b);
                }
            }

            static string NameOf(UIAbilityBarButton b)
            {
                if (b.TargetAbility != null) return b.TargetAbility.Name();
                if (b.TargetAnyItem != null) return b.TargetAnyItem.Name;
                if (b.Label != null && !string.IsNullOrEmpty(b.Label.text)) return b.Label.text;
                if (b.Owner != null && b.Owner.Label != null) return b.Owner.Label.text;
                return "";
            }

            static void Clear()
            {
                if (s_Root != null) UnityEngine.Object.Destroy(s_Root);
                s_Root = null;
                s_Back = null;
                s_Name = null;
                s_Icons.Clear();
                s_Buttons.Clear();
                s_Selected = -1;
            }

            // A soft dark disc (made here; no game art).
            static Texture2D Disc()
            {
                if (s_DiscTexture != null) return s_DiscTexture;
                const int size = 128;
                Texture2D t = new Texture2D(size, size, TextureFormat.ARGB32, false);
                Color32[] px = new Color32[size * size];
                float c = (size - 1) * 0.5f;
                for (int y = 0; y < size; y++)
                    for (int x = 0; x < size; x++)
                    {
                        float d = Mathf.Sqrt((x - c) * (x - c) + (y - c) * (y - c)) / c;
                        float a = d >= 1f ? 0f : 0.78f * Mathf.Clamp01((1f - d) * 12f);
                        px[y * size + x] = new Color32(12, 10, 8, (byte)Mathf.RoundToInt(a * 255f));
                    }
                t.SetPixels32(px);
                t.wrapMode = TextureWrapMode.Clamp;
                t.Apply(false, true);
                s_DiscTexture = t;
                return t;
            }

            static void Build(UIAbilityBar bar, List<UIAbilityBarButton> buttons)
            {
                Clear();
                UIAbilityBarButton first = buttons[0];
                Transform uiRoot = bar.transform.root;
                Camera cam = NGUITools.FindCameraForLayer(bar.gameObject.layer);
                if (cam == null) throw new InvalidOperationException("the ability bar has no UI camera");
                s_Root = NGUITools.AddChild(uiRoot.gameObject);
                s_Root.name = "PortController.RadialMenu";
                s_Root.AddComponent(typeof(UIPanel));
                Vector3 center = cam.ViewportToWorldPoint(new Vector3(0.5f, 0.5f, 0f));
                Vector3 local = uiRoot.InverseTransformPoint(center);
                s_Root.transform.localPosition = new Vector3(local.x, local.y, -10f);
                float rootScale = s_Root.transform.lossyScale.x;
                if (rootScale <= 0f) throw new InvalidOperationException("the UI root has no scale");
                s_IconSize = first.Icon.transform.lossyScale.x / rootScale;
                if (s_IconSize <= 0f) throw new InvalidOperationException("an ability bar icon has no size");
                int n = buttons.Count;
                float radius = Mathf.Max(RingMin * s_IconSize, n * SlotSpacing * s_IconSize / (2f * Mathf.PI));
                Shader shader = first.Icon.shader;
                s_Back = Ui.AddTexture(s_Root, Disc(), shader, 2f * (radius + s_IconSize * 1.1f), Depth);
                for (int i = 0; i < n; i++)
                {
                    UIAbilityBarButton b = buttons[i];
                    float a = i * 2f * Mathf.PI / n;
                    UITexture icon = Ui.AddTexture(s_Root, b.Icon.mainTexture, shader, s_IconSize, Depth + 2);
                    icon.uvRect = b.Icon.uvRect;
                    icon.transform.localPosition = new Vector3(Mathf.Sin(a) * radius, Mathf.Cos(a) * radius, 0f);
                    icon.color = b.Disabled ? new Color(0.45f, 0.45f, 0.45f, 0.6f) : new Color(1f, 1f, 1f, 0.85f);
                    s_Icons.Add(icon);
                    s_Buttons.Add(b);
                }
                UILabel template = first.Label != null && first.Label.font != null ? first.Label
                                   : first.Owner != null && first.Owner.Label != null ? first.Owner.Label : null;
                if (template == null || template.font == null) throw new InvalidOperationException("the ability bar has no label font");
                GameObject lgo = NGUITools.AddChild(s_Root);
                s_Name = (UILabel)lgo.AddComponent(typeof(UILabel));
                s_Name.font = template.font;
                s_Name.pivot = UIWidget.Pivot.Center;
                s_Name.depth = Depth + 4;
                s_Name.multiLine = true;
                s_Name.lineWidth = Mathf.RoundToInt(radius * 1.4f);
                s_Name.effectStyle = UILabel.Effect.Shadow;
                s_Name.text = "";
                s_Name.MakePixelPerfect();
                // The buttons of the menu, under the ring.
                float hy = -(radius + s_IconSize * 1.75f), hg = s_IconSize * 0.5f, step = s_IconSize * 2.4f;
                Ui.Row(s_Root, template.font, shader, -step - hg, hy, hg, GlyphA, "Use", Depth + 4);
                Ui.Row(s_Root, template.font, shader, -hg, hy, hg, GlyphB, "Back", Depth + 4);
                Ui.Row(s_Root, template.font, shader, step - hg, hy, hg, GlyphLT, "Close", Depth + 4);
                s_Selected = -1;
            }

            static void Select(int i)
            {
                if (i == s_Selected) return;
                if (s_Selected >= 0) Style(s_Selected, false);
                s_Selected = i;
                Style(i, true);
                s_Name.text = NameOf(s_Buttons[i]);
                s_Name.MakePixelPerfect();
            }

            static void Style(int i, bool selected)
            {
                UITexture icon = s_Icons[i];
                bool disabled = s_Buttons[i].Disabled;
                float size = s_IconSize * (selected ? SelectedScale : 1f);
                icon.transform.localScale = new Vector3(size, size, 1f);
                icon.color = disabled ? new Color(0.45f, 0.45f, 0.45f, selected ? 0.8f : 0.6f)
                                      : new Color(1f, 1f, 1f, selected ? 1f : 0.85f);
                icon.depth = Depth + (selected ? 3 : 2);
            }
        }

        // ---------------------------------------------------------------- glyphs

        // The SF Symbols glyphs of the current controller, white with alpha
        // (the widget color tints them).
        static class Glyphs
        {
            const int Size = 64;
            static readonly Texture2D[] s_Tex = new Texture2D[GlyphCount];
            static readonly bool[] s_Has = new bool[GlyphCount];
            static readonly Color32[] s_Pixels = new Color32[Size * Size];
            static bool s_Loaded;
            static int s_Gen;

            public static void Tick()
            {
                int gen = PortPadGlyphs();
                if (s_Loaded && gen == s_Gen) return;
                s_Loaded = true;
                s_Gen = gen;
                for (int i = 0; i < GlyphCount; i++)
                {
                    s_Has[i] = PortPadGlyph(i, Size, s_Pixels) != 0;
                    if (!s_Has[i]) continue;
                    if (s_Tex[i] == null)
                    {
                        s_Tex[i] = new Texture2D(Size, Size, TextureFormat.ARGB32, false);
                        s_Tex[i].wrapMode = TextureWrapMode.Clamp;
                        s_Tex[i].filterMode = FilterMode.Bilinear;
                    }
                    s_Tex[i].SetPixels32(s_Pixels);
                    s_Tex[i].Apply(false, false);
                }
                // Hints that are open show the old glyphs: build them again.
                Help.Rebuild();
            }

            // The glyph, or null if the controller has none for the control.
            public static Texture2D Get(int glyph)
            {
                return s_Has[glyph] ? s_Tex[glyph] : null;
            }
        }

        // ---------------------------------------------------------------- UI helpers

        static class Ui
        {
            static Transform s_FontRoot;
            static UIFont s_Font;
            static Shader s_Shader;

            public static UITexture AddTexture(GameObject parent, Texture tex, Shader shader, float size, int depth)
            {
                GameObject go = NGUITools.AddChild(parent);
                UITexture w = (UITexture)go.AddComponent(typeof(UITexture));
                w.shader = shader;
                w.mainTexture = tex;
                w.pivot = UIWidget.Pivot.Center;
                w.depth = depth;
                go.transform.localScale = new Vector3(size, size, 1f);
                return w;
            }

            public static UILabel AddLabel(GameObject parent, UIFont font, string text, UIWidget.Pivot pivot, int depth)
            {
                GameObject go = NGUITools.AddChild(parent);
                UILabel l = (UILabel)go.AddComponent(typeof(UILabel));
                l.font = font;
                l.pivot = pivot;
                l.depth = depth;
                l.effectStyle = UILabel.Effect.Shadow;
                l.text = text;
                l.MakePixelPerfect();
                return l;
            }

            // A glyph of g units with its left edge at x, and the text after
            // it; nothing if the controller has no such control.
            public static void Row(GameObject parent, UIFont font, Shader shader, float x, float y, float g, int glyph, string text, int depth)
            {
                Texture2D t = Glyphs.Get(glyph);
                if (t == null) return;
                UITexture w = AddTexture(parent, t, shader, g, depth);
                w.transform.localPosition = new Vector3(x + g * 0.5f, y, 0f);
                UILabel l = AddLabel(parent, font, text, UIWidget.Pivot.Left, depth);
                l.transform.localPosition = new Vector3(x + g * 1.35f, y, 0f);
            }

            // The shader of NGUI for textures with alpha and a tint.
            public static Shader TextureShader()
            {
                if (s_Shader != null) return s_Shader;
                s_Shader = Shader.Find("Unlit/Transparent Colored");
                if (s_Shader == null) throw new InvalidOperationException("the game has no shader Unlit/Transparent Colored");
                return s_Shader;
            }

            // The root of the game's HUD and its UI camera; null when the game
            // shows no HUD (the main menu, loading).
            public static Transform HudRoot(out Camera cam)
            {
                cam = null;
                UIWindowManager m = UIWindowManager.Instance;
                if (m == null || !m.gameObject.activeInHierarchy) return null;
                cam = NGUITools.FindCameraForLayer(m.gameObject.layer);
                return cam == null ? null : m.transform.root;
            }

            public static UIFont Font(Transform root)
            {
                if (s_Font != null && s_FontRoot == root) return s_Font;
                s_Font = null;
                s_FontRoot = root;
                foreach (Component c in root.GetComponentsInChildren(typeof(UILabel), true))
                {
                    UILabel l = (UILabel)c;
                    if (l.font != null)
                    {
                        s_Font = l.font;
                        break;
                    }
                }
                if (s_Font == null) throw new InvalidOperationException("the HUD has no label font");
                return s_Font;
            }

            // UI units (local to root) for one screen pixel.
            public static float UnitsPerPixel(Transform root, Camera cam)
            {
                float scale = root.lossyScale.x;
                if (scale <= 0f) throw new InvalidOperationException("the UI root has no scale");
                Vector3 a = cam.ScreenToWorldPoint(new Vector3(0f, 0f, 0f)), b = cam.ScreenToWorldPoint(new Vector3(100f, 0f, 0f));
                return (b - a).magnitude / 100f / scale;
            }

            // A panel of the menu's colors: a dark ground with a thin gold rim
            // and round corners (made here; no game art).
            public static Texture2D Panel(int w, int h, int radius)
            {
                Texture2D t = new Texture2D(w, h, TextureFormat.ARGB32, false);
                Color32[] px = new Color32[w * h];
                for (int y = 0; y < h; y++)
                    for (int x = 0; x < w; x++)
                    {
                        // Distance outside the rectangle inset by the radius.
                        float dx = Mathf.Max(0f, Mathf.Max(radius - x - 0.5f, x + 0.5f - (w - radius)));
                        float dy = Mathf.Max(0f, Mathf.Max(radius - y - 0.5f, y + 0.5f - (h - radius)));
                        float edge = radius - Mathf.Sqrt(dx * dx + dy * dy); // pixels inside the shape
                        float a = Mathf.Clamp01(edge);
                        if (a <= 0f)
                        {
                            px[y * w + x] = new Color32(0, 0, 0, 0);
                            continue;
                        }
                        float rim = Mathf.Clamp01(1.5f - Mathf.Abs(edge - 1.5f)) * 0.55f;
                        byte r = (byte)Mathf.RoundToInt(Mathf.Lerp(12f, 184f, rim)), g = (byte)Mathf.RoundToInt(Mathf.Lerp(10f, 145f, rim)),
                             bl = (byte)Mathf.RoundToInt(Mathf.Lerp(8f, 58f, rim));
                        px[y * w + x] = new Color32(r, g, bl, (byte)Mathf.RoundToInt(a * Mathf.Lerp(0.86f, 1f, rim) * 255f));
                    }
                t.SetPixels32(px);
                t.wrapMode = TextureWrapMode.Clamp;
                t.Apply(false, true);
                return t;
            }
        }

        // ---------------------------------------------------------------- snap mode

        static class Snap
        {
            const float RepeatDelay = 0.35f, RepeatStep = 0.14f;
            // Minimum move in the direction (pixels), and the weight of the
            // distance across the direction in the choice of the next control.
            const float MinStep = 4f, Across = 2.5f;
            static readonly Vector2[] Dirs = { new Vector2(0f, 1f), new Vector2(0f, -1f), new Vector2(-1f, 0f), new Vector2(1f, 0f) };
            static readonly int[] DirBits = { PadUp, PadDown, PadLeft, PadRight };
            static readonly object Yes = new object(), No = new object();
            static readonly Dictionary<Type, object> s_Handlers = new Dictionary<Type, object>();
            static readonly List<UIHudWindow> s_Focused = new List<UIHudWindow>();
            static FieldInfo s_Windows;
            static bool s_On;
            static int s_Dir = -1;
            static float s_RepeatAt;
            static Vector2 s_Aim;
            static int s_AimFrame = -100;
            static Collider s_Target;

            public static bool On
            {
                get { return s_On; }
            }

            public static void Reset()
            {
                if (s_On) PortPadSnap(0);
                s_On = false;
                s_Dir = -1;
                s_Target = null;
                Badge.Hide();
            }

            public static void Tick(int b, int down, bool menuOpen)
            {
                bool want = !menuOpen && FindFocused();
                if (want != s_On)
                {
                    s_On = want;
                    PortPadSnap(want ? 1 : 0);
                    s_Dir = -1;
                    if (!want) s_Target = null;
                }
                if (!s_On)
                {
                    Badge.Hide();
                    return;
                }
                float now = Time.realtimeSinceStartup;
                int dir = -1;
                for (int i = 0; i < 4; i++)
                    if ((down & DirBits[i]) != 0)
                    {
                        dir = i;
                        break;
                    }
                if (dir >= 0)
                {
                    s_Dir = dir;
                    s_RepeatAt = now + RepeatDelay;
                }
                else if (s_Dir >= 0 && (b & DirBits[s_Dir]) != 0)
                {
                    if (now >= s_RepeatAt)
                    {
                        dir = s_Dir;
                        s_RepeatAt = now + RepeatStep;
                    }
                }
                else
                    s_Dir = -1;
                if (dir >= 0) Move(dir);
                Badge.Tick(s_Target);
            }

            // The windows with the focus that take the player's attention
            // (not a part of the HUD that only shows).
            static bool FindFocused()
            {
                s_Focused.Clear();
                UIWindowManager m = UIWindowManager.Instance;
                if (m == null) return false;
                if (s_Windows == null)
                {
                    s_Windows = typeof(UIWindowManager).GetField("m_Windows", BindingFlags.Instance | BindingFlags.NonPublic);
                    if (s_Windows == null) throw new InvalidOperationException("UIWindowManager has no m_Windows");
                }
                List<UIHudWindow> all = (List<UIHudWindow>)s_Windows.GetValue(m);
                if (all == null) return false;
                foreach (UIHudWindow w in all)
                {
                    if (w == null || !m.IsFocused(w)) continue;
                    if (w.PausesGame || w.HidesHud || w.DimsBackground || w.EatsKeyInput || w.EatsMouseInput || w.MessageBox || w is UIConversationManager)
                        s_Focused.Add(w);
                }
                return s_Focused.Count > 0;
            }

            static void Move(int dir)
            {
                Vector2 from = Time.frameCount - s_AimFrame <= 2 ? s_Aim : new Vector2(Input.mousePosition.x, Input.mousePosition.y);
                Vector2 d = Dirs[dir];
                float sw = Screen.width, sh = Screen.height, best = float.MaxValue;
                Collider target = null;
                Vector2 at = Vector2.zero;
                foreach (UIHudWindow w in s_Focused)
                    foreach (Component comp in w.GetComponentsInChildren(typeof(Collider)))
                    {
                        Collider c = (Collider)comp;
                        if (!c.enabled || !Clickable(c.gameObject)) continue;
                        Camera cam = NGUITools.FindCameraForLayer(c.gameObject.layer);
                        if (cam == null) continue;
                        Bounds bb = c.bounds;
                        Vector3 lo = cam.WorldToScreenPoint(bb.min), hi = cam.WorldToScreenPoint(bb.max), mid = cam.WorldToScreenPoint(bb.center);
                        float wpx = Mathf.Abs(hi.x - lo.x), hpx = Mathf.Abs(hi.y - lo.y);
                        // Not a point of the screen, or a background that
                        // catches clicks.
                        if (wpx < 2f || hpx < 2f || (wpx > 0.5f * sw && hpx > 0.5f * sh)) continue;
                        if (mid.x < 0f || mid.y < 0f || mid.x >= sw || mid.y >= sh) continue;
                        float vx = mid.x - from.x, vy = mid.y - from.y;
                        float along = vx * d.x + vy * d.y;
                        if (along < MinStep) continue;
                        float score = along + Across * Mathf.Abs(vx * d.y - vy * d.x);
                        if (score >= best || !Reaches(c, mid)) continue;
                        best = score;
                        target = c;
                        at = new Vector2(mid.x, mid.y);
                    }
                if (target == null) return;
                s_Target = target;
                s_Aim = at;
                s_AimFrame = Time.frameCount;
                PortPadWarp(Mathf.RoundToInt(at.x), Screen.height - Mathf.RoundToInt(at.y), Screen.width, Screen.height);
            }

            // A click at the point goes to the collider (or a part of it):
            // nothing covers it and the panel does not clip it.
            static bool Reaches(Collider c, Vector3 at)
            {
                RaycastHit hit = new RaycastHit();
                Camera keep = UICamera.currentCamera;
                bool ok = UICamera.Raycast(new Vector3(at.x, at.y, 0f), out hit);
                UICamera.currentCamera = keep;
                return ok && hit.collider != null && (hit.collider == c || hit.collider.transform.IsChildOf(c.transform));
            }

            // NGUI sends OnClick, OnPress and OnDoubleClick to the object
            // under the cursor.
            static bool Clickable(GameObject go)
            {
                foreach (Component comp in go.GetComponents(typeof(MonoBehaviour)))
                {
                    MonoBehaviour mb = comp as MonoBehaviour;
                    if (mb == null || !mb.enabled) continue;
                    UIEventListener l = mb as UIEventListener;
                    if (l != null)
                    {
                        if (l.onClick != null || l.onPress != null || l.onDoubleClick != null) return true;
                        continue;
                    }
                    Type t = mb.GetType();
                    object h;
                    if (!s_Handlers.TryGetValue(t, out h))
                    {
                        h = Handles(t, "OnClick") || Handles(t, "OnPress") || Handles(t, "OnDoubleClick") ? Yes : No;
                        s_Handlers[t] = h;
                    }
                    if (h == Yes) return true;
                }
                return false;
            }

            static bool Handles(Type t, string name)
            {
                const BindingFlags flags = BindingFlags.Instance | BindingFlags.Public | BindingFlags.NonPublic | BindingFlags.DeclaredOnly;
                for (; t != null && t != typeof(MonoBehaviour); t = t.BaseType)
                    if (t.GetMember(name, MemberTypes.Method, flags).Length > 0) return true;
                return false;
            }
        }

        // The glyph of A at the top right corner of the control under the
        // cursor in the snap mode.
        static class Badge
        {
            const float SizePx = 22f;
            const int Depth = 2050;
            static GameObject s_Root;
            static Transform s_UiRoot;
            static UITexture s_Icon;

            public static void Hide()
            {
                if (s_Root != null) UnityEngine.Object.Destroy(s_Root);
                s_Root = null;
                s_UiRoot = null;
                s_Icon = null;
            }

            public static void Tick(Collider target)
            {
                Texture2D tex = Glyphs.Get(GlyphA);
                if (target == null || tex == null || !target.enabled || !target.gameObject.activeInHierarchy)
                {
                    Hide();
                    return;
                }
                Camera cam = NGUITools.FindCameraForLayer(target.gameObject.layer);
                if (cam == null)
                {
                    Hide();
                    return;
                }
                Bounds bb = target.bounds;
                Vector3 lo = cam.WorldToScreenPoint(bb.min), hi = cam.WorldToScreenPoint(bb.max), m = Input.mousePosition;
                float x0 = Mathf.Min(lo.x, hi.x), x1 = Mathf.Max(lo.x, hi.x), y0 = Mathf.Min(lo.y, hi.y), y1 = Mathf.Max(lo.y, hi.y);
                if (m.x < x0 - 2f || m.x > x1 + 2f || m.y < y0 - 2f || m.y > y1 + 2f)
                {
                    Hide();
                    return;
                }
                Transform root = target.transform.root;
                if (s_Root == null || s_UiRoot != root)
                {
                    Hide();
                    s_UiRoot = root;
                    s_Root = NGUITools.AddChild(root.gameObject);
                    s_Root.name = "PortController.Badge";
                    s_Root.AddComponent(typeof(UIPanel));
                    s_Icon = Ui.AddTexture(s_Root, tex, Ui.TextureShader(), SizePx * Ui.UnitsPerPixel(root, cam), Depth);
                }
                s_Icon.mainTexture = tex;
                Vector3 corner = cam.ScreenToWorldPoint(new Vector3(x1 - SizePx * 0.25f, y1 - SizePx * 0.25f, 0f));
                Vector3 local = root.InverseTransformPoint(corner);
                s_Root.transform.localPosition = new Vector3(local.x, local.y, -10f);
            }
        }

        // ---------------------------------------------------------------- help overlay

        // View: a tap presses the Inventory key; holding it shows the help
        // overlay until it is released.
        static class Help
        {
            const float HoldTime = 0.4f;
            const int Depth = 2100;
            static float s_ViewAt = -1f;
            static bool s_TapRelease;
            static GameObject s_Root;

            public static void Reset()
            {
                Close();
                s_ViewAt = -1f;
                s_TapRelease = false;
                Hold(Inventory, false);
            }

            public static void Rebuild()
            {
                if (s_Root == null) return;
                Close();
                Open();
            }

            public static void Tick(int b, int down, int up)
            {
                if (s_TapRelease)
                {
                    Hold(Inventory, false);
                    s_TapRelease = false;
                }
                float now = Time.realtimeSinceStartup;
                if ((down & PadView) != 0) s_ViewAt = now;
                if ((b & PadView) != 0 && s_ViewAt >= 0f && s_Root == null && now - s_ViewAt >= HoldTime)
                {
                    Open();
                    // A HUD that does not show cancels the hold: no tap.
                    if (s_Root == null) s_ViewAt = -1f;
                }
                if ((up & PadView) != 0)
                {
                    if (s_Root != null)
                        Close();
                    else if (s_ViewAt >= 0f)
                    {
                        Hold(Inventory, true);
                        s_TapRelease = true;
                    }
                    s_ViewAt = -1f;
                }
            }

            static void Close()
            {
                if (s_Root != null) UnityEngine.Object.Destroy(s_Root);
                s_Root = null;
            }

            sealed class Line
            {
                public readonly int Glyph;
                public readonly string Text;
                public Line(int glyph, string text)
                {
                    Glyph = glyph;
                    Text = text;
                }
            }

            static string Name(MappedControl c)
            {
                string n = MappedInput.GetControlName(c);
                return string.IsNullOrEmpty(n) ? c.ToString() : n;
            }

            static void Open()
            {
                Camera cam;
                Transform root = Ui.HudRoot(out cam);
                if (root == null) return;
                UIFont font = Ui.Font(root);
                Shader shader = Ui.TextureShader();
                List<Line> left = new List<Line>();
                List<Line> right = new List<Line>();
                left.Add(new Line(GlyphLeftStick, "Move the cursor"));
                left.Add(new Line(GlyphA, "Click"));
                left.Add(new Line(GlyphB, "Right-click"));
                left.Add(new Line(GlyphLT, "Ability menu"));
                left.Add(new Line(GlyphRightStick, "Pan the camera"));
                left.Add(new Line(GlyphDpad, "Zoom (up, down)"));
                left.Add(new Line(GlyphDpad, "In windows: go to the next control"));
                left.Add(new Line(GlyphRightStick, "In windows: scroll"));
                left.Add(new Line(GlyphView, "Hold: this help"));
                for (int i = 0; i < ButtonBits.Length; i++) right.Add(new Line(ButtonGlyphs[i], Name(ButtonCommands[i].Control)));
                right.Add(new Line(GlyphView, Name(Inventory.Control)));
                // Rows for which the controller has no glyph are left out.
                left.RemoveAll(NoGlyph);
                right.RemoveAll(NoGlyph);

                float f = font.size, g = f * 1.25f, rowH = f * 1.7f, colW = f * 19f, pad = f * 1.3f, titleH = f * 2f;
                int rows = Mathf.Max(left.Count, right.Count);
                float w = 2f * pad + 2f * colW, h = 2f * pad + titleH + rows * rowH;
                s_Root = NGUITools.AddChild(root.gameObject);
                s_Root.name = "PortController.Help";
                s_Root.AddComponent(typeof(UIPanel));
                Vector3 center = root.InverseTransformPoint(cam.ViewportToWorldPoint(new Vector3(0.5f, 0.5f, 0f)));
                s_Root.transform.localPosition = new Vector3(center.x, center.y, -12f);
                int tw = Mathf.Clamp(Mathf.RoundToInt(w), 64, 2048), th = Mathf.Clamp(Mathf.RoundToInt(h), 64, 2048);
                UITexture back = Ui.AddTexture(s_Root, Ui.Panel(tw, th, Mathf.RoundToInt(f * 0.9f)), shader, 1f, Depth);
                back.transform.localScale = new Vector3(w, h, 1f);
                UILabel title = Ui.AddLabel(s_Root, font, "Controller", UIWidget.Pivot.Center, Depth + 2);
                title.color = new Color(0.94f, 0.82f, 0.52f, 1f);
                title.transform.localPosition = new Vector3(0f, h * 0.5f - pad - titleH * 0.5f, 0f);
                float top = h * 0.5f - pad - titleH - rowH * 0.5f, x0 = -w * 0.5f + pad;
                for (int i = 0; i < left.Count; i++)
                    Ui.Row(s_Root, font, shader, x0, top - i * rowH, g, left[i].Glyph, left[i].Text, Depth + 2);
                for (int i = 0; i < right.Count; i++)
                    Ui.Row(s_Root, font, shader, x0 + colW, top - i * rowH, g, right[i].Glyph, right[i].Text, Depth + 2);
            }

            static bool NoGlyph(Line l)
            {
                return Glyphs.Get(l.Glyph) == null;
            }
        }
    }
}
