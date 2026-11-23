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
//   Menu           game menu           View           Inventory
//   L3             Stealth             R3             Follow camera
//   right stick    camera pan
//
// Each command presses the key that the player bound to it in the game's
// Controls options (GameState.Controls), with its modifier keys, while the
// button is down (PortPadKey). A command without a key binding does
// nothing; the log names it once.
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
using System.Runtime.InteropServices;
using UnityEngine;

namespace PortGraphics
{
    public static class PortController
    {
        // Bits of PortPadRead (runtime/src/rt_gamepad.m has the same values).
        const int PadA = 1 << 0, PadB = 1 << 1, PadX = 1 << 2, PadY = 1 << 3, PadLB = 1 << 4, PadRB = 1 << 5,
            PadLT = 1 << 6, PadRT = 1 << 7, PadLeft = 1 << 10, PadRight = 1 << 11, PadMenu = 1 << 12,
            PadView = 1 << 13, PadL3 = 1 << 14, PadR3 = 1 << 15;

        [DllImport("portgfx", CallingConvention = CallingConvention.StdCall)]
        static extern int PortPadRead(float[] axes);

        [DllImport("portgfx", CallingConvention = CallingConvention.StdCall)]
        static extern void PortPadCapture(int on);

        [DllImport("portgfx", CallingConvention = CallingConvention.StdCall)]
        static extern int PortPadKey(int unityKeyCode, int down);

        [DllImport("portgfx", CallingConvention = CallingConvention.StdCall, CharSet = CharSet.Ansi)]
        static extern void PortGfxFail(string message);

        // A command of the game on a controller input, and the keys that it
        // holds down (empty while the input is up).
        sealed class Command
        {
            public readonly MappedControl Control;
            public readonly List<int> Held = new List<int>();
            public bool Logged;
            public Command(MappedControl control)
            {
                Control = control;
            }
        }

        // Button commands: the bit, then the command.
        static readonly int[] ButtonBits = { PadX, PadY, PadLB, PadRB, PadRT, PadLeft, PadRight, PadMenu, PadView, PadL3, PadR3 };
        static readonly Command[] ButtonCommands =
        {
            new Command(MappedControl.SELECT_ALL), new Command(MappedControl.PAUSE),
            new Command(MappedControl.PREVIOUS_COMPANION), new Command(MappedControl.NEXT_COMPANION),
            new Command(MappedControl.HIGHLIGHT_HOLD), new Command(MappedControl.SLOW_TOGGLE),
            new Command(MappedControl.FAST_TOGGLE), new Command(MappedControl.OPTIONS),
            new Command(MappedControl.INVENTORY), new Command(MappedControl.STEALTH_TOGGLE),
            new Command(MappedControl.FOLLOW_CAM),
        };
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
                if (!s_Connected)
                {
                    s_Connected = true;
                    Debug.Log("PortController: a controller is in use");
                }
                int down = b & ~s_Prev;
                s_Prev = b;
                for (int i = 0; i < ButtonBits.Length; i++) Hold(ButtonCommands[i], (b & ButtonBits[i]) != 0);
                Pan(0, -s_Axes[2]);
                Pan(1, s_Axes[2]);
                Pan(2, s_Axes[3]);
                Pan(3, -s_Axes[3]);
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
            Menu.Close();
            Debug.Log("PortController: no controller is in use");
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
            if (c.Held.Count == 0)
            {
                if (!c.Logged) Debug.Log("PortController: " + c.Control + " has no key binding; its controller button does nothing");
                c.Logged = true;
                return;
            }
            foreach (int k in c.Held)
                if (PortPadKey(k, 1) == 0)
                {
                    // The game allows keys that a Mac keyboard does not
                    // have (Pause, Scroll Lock): release what was pressed.
                    for (int i = c.Held.IndexOf(k) - 1; i >= 0; i--) Key(c.Held[i], false);
                    if (!c.Logged)
                        Debug.Log("PortController: the key " + (KeyCode)k + " of " + c.Control + " has no Mac key; its controller button does nothing");
                    c.Logged = true;
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

            static UITexture AddTexture(GameObject parent, Texture tex, Shader shader, float size, int depth)
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
                s_Back = AddTexture(s_Root, Disc(), shader, 2f * (radius + s_IconSize * 1.1f), Depth);
                for (int i = 0; i < n; i++)
                {
                    UIAbilityBarButton b = buttons[i];
                    float a = i * 2f * Mathf.PI / n;
                    UITexture icon = AddTexture(s_Root, b.Icon.mainTexture, shader, s_IconSize, Depth + 2);
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
    }
}
