// The port's rows on the Graphics tab of the game's options window.
//
// The runtime calls GraphicsOptions.Tick once after each presented frame, on
// the main thread, in the domain of the game's scripts (rt_mono_port_tick,
// runtime/src/rt_mono.c). When the Graphics page of UIOptionsManager has
// been shown for one frame for the first time, Tick adds these controls to
// the page:
//   - 5 checkboxes: copies of a checkbox row of the page (a UIOptionsTag) in
//     the grid of the checkbox rows (a UIGrid with 2 columns, sorted by
//     name). The names of the copies sort after the names of the game's rows,
//     so the copies come after them. The row step of the grid becomes
//     RowStep (the game uses 32), so that the grid with the new rows fits on
//     the page. The sliders below the grid move down by the height that the
//     grid gets.
//   - the "RCAS Sharpening" slider (0 = off, 1 to 8 = strength): a copy of
//     the Gamma slider, on the row of the Graphics Quality slider, to the
//     right of it. The Graphics Quality slider moves from the middle of the
//     page to its left edge.
// If the controls do not fit above the tooltip line of the window or within
// its width, Build stops with an error.
//
// The copies have no UIOptionsTag, UIOptionsSliderTag, GUIStringLabel or
// UIOptionsTooltipTrigger: these components would connect a copy to a game
// option or replace its text. The labels and tooltips are English text.
//
// A change takes effect at once and is saved at once (PortGfxSet in
// runtime/src/rt_portgfx.c). Two settings take effect at the next start of
// the game; their labels say so. If the runtime cannot save a setting, the
// control goes back to the saved value and a message box tells the player.
//
// Errors: Tick and the handlers catch every exception and report it with
// PortGfxFail, which stops the game with the message. A page that does not
// have the expected controls is an error.
//
// WARNING: this assembly is compiled against the game's assemblies. Do not
// distribute, upload or commit the compiled assembly or its AOT image.
using System;
using System.Collections.Generic;
using System.Runtime.InteropServices;
using UnityEngine;

namespace PortGraphics
{
    public static class GraphicsOptions
    {
        // Setting ids of runtime/src/rt_portgfx.c.
        const int Upscaling = 0, Sharpness = 1, Hdr = 2, Anisotropic = 3, Shadows = 4, FloatTargets = 5;
        const int SharpnessMax = 8;
        // The row step of the checkbox grid with the new rows. The checkbox
        // sprites are 27 units high.
        const float RowStep = 30f;

        [DllImport("portgfx", CallingConvention = CallingConvention.StdCall)]
        static extern int PortGfxGet(int id);

        [DllImport("portgfx", CallingConvention = CallingConvention.StdCall)]
        static extern int PortGfxSet(int id, int value);

        [DllImport("portgfx", CallingConvention = CallingConvention.StdCall, CharSet = CharSet.Ansi)]
        static extern void PortGfxFail(string message);

        sealed class CheckRow
        {
            public int Id;
            public string Label, Tooltip;
            public UICheckbox Box;
            public CheckRow(int id, string label, string tooltip)
            {
                Id = id;
                Label = label;
                Tooltip = tooltip;
            }
        }

        static readonly CheckRow[] Rows =
        {
            new CheckRow(Upscaling, "MetalFX Upscaling",
                "Upscales the game image to the resolution of the screen with Apple MetalFX. When this is off, the image is stretched with bilinear filtering."),
            new CheckRow(Hdr, "HDR Output",
                "Shows the game with HDR and GT7 tone mapping on a screen that has HDR. When this is off, the game is shown in standard dynamic range."),
            new CheckRow(Anisotropic, "16x Anisotropic Filtering",
                "Uses 16x anisotropic filtering for the textures of the game, so that surfaces seen at an angle stay sharp."),
            new CheckRow(Shadows, "Supersampled Shadows (Restart)",
                "Draws the dynamic shadows of characters at 4 times the resolution in each direction. A change takes effect when you start the game again."),
            new CheckRow(FloatTargets, "32-bit Render Targets (Restart)",
                "Keeps the intermediate images of the game in 32-bit float, for more precise blending. A change takes effect when you start the game again."),
        };

        const string SharpnessLabel = "RCAS Sharpening";
        const string SharpnessTooltip =
            "Sharpens the final image with AMD FidelityFX RCAS. 0 turns sharpening off; 1 to 8 set the strength, and 8 is the sharpest.";
        const string RowPrefix = "PortGraphics.";

        static UIOptionsManager s_Manager;
        static UIOptionsSliderGroup s_SharpnessGroup;
        static bool s_Updating;
        static int s_ActiveTicks;

        public static void Tick()
        {
            PortController.Tick();
            try
            {
                UIOptionsManager mgr = UIOptionsManager.Instance;
                if (mgr == null)
                {
                    s_Manager = null;
                    return;
                }
                if (mgr == s_Manager) return;
                GameObject page = GraphicsPage(mgr);
                if (!page.activeInHierarchy)
                {
                    s_ActiveTicks = 0;
                    return;
                }
                // UIAnchor and UIStretch place the widgets of the page in
                // Update. After the first frame in which the page is active,
                // the layout is final.
                if (++s_ActiveTicks < 2) return;
                Build(mgr, page);
                s_Manager = mgr;
            }
            catch (Exception e)
            {
                Fail("Tick", e);
            }
        }

        static void Fail(string where, Exception e)
        {
            PortGfxFail("PortGraphics." + where + ": " + e.ToString());
        }

        // The page of mgr.Pages that holds the Graphics Quality slider.
        static GameObject GraphicsPage(UIOptionsManager mgr)
        {
            if (mgr.QualitySlider == null) throw new InvalidOperationException("UIOptionsManager has no QualitySlider");
            foreach (GameObject p in mgr.Pages)
                if (p != null && mgr.QualitySlider.transform.IsChildOf(p.transform)) return p;
            throw new InvalidOperationException("no page of UIOptionsManager holds the Graphics Quality slider");
        }

        static Vector3 PagePos(Transform page, Transform t)
        {
            return page.InverseTransformPoint(t.position);
        }

        static void SetPagePos(Transform page, Transform t, Vector3 p)
        {
            t.position = page.TransformPoint(p);
        }

        static Bounds PageBounds(Transform page, Transform t)
        {
            return NGUIMath.CalculateRelativeWidgetBounds(page, t);
        }

        // The UIGrid of the page with the most checkbox rows (UIOptionsTag
        // children with a checkbox and a label).
        static UIGrid CheckGrid(Transform page, out UIOptionsTag template)
        {
            UIGrid best = null;
            int bestCount = 0;
            template = null;
            foreach (UIGrid g in page.GetComponentsInChildren<UIGrid>(true))
            {
                int n = 0;
                UIOptionsTag first = null;
                foreach (Transform c in g.transform)
                {
                    UIOptionsTag t = c.GetComponent<UIOptionsTag>();
                    if (t == null || t.Checkbox == null || t.Label == null) continue;
                    if (first == null) first = t;
                    n++;
                }
                if (n > bestCount)
                {
                    best = g;
                    bestCount = n;
                    template = first;
                }
            }
            if (bestCount < 2) throw new InvalidOperationException("the Graphics page has no grid of checkbox rows");
            return best;
        }

        // The tooltip line of the options window, below the content of the
        // pages and as wide as it.
        static Bounds TooltipLine(UIOptionsManager mgr, Transform page)
        {
            foreach (Transform t in mgr.Window.GetComponentsInChildren<Transform>(true))
                if (t.name == "TooltipDivider") return PageBounds(page, t);
            throw new InvalidOperationException("the options window has no TooltipDivider");
        }

        static void Build(UIOptionsManager mgr, GameObject pageGo)
        {
            Transform page = pageGo.transform;
            foreach (Transform c in page.GetComponentsInChildren<Transform>(true))
                if (c.name.StartsWith(RowPrefix, StringComparison.Ordinal))
                    throw new InvalidOperationException("the Graphics page already has the rows of the port");

            UIOptionsTag template;
            UIGrid grid = CheckGrid(page, out template);
            Bounds line = TooltipLine(mgr, page);
            float oldBottom = PageBounds(page, grid.transform).min.y;

            // The checkbox rows. "PortGraphics.<n>" sorts after the game's
            // names, which start with digits.
            for (int i = 0; i < Rows.Length; i++)
                AddCheckRow(grid.transform, template, Rows[i], i);
            if (grid.cellHeight > RowStep) grid.cellHeight = RowStep;
            grid.Reposition();
            float shift = oldBottom - PageBounds(page, grid.transform).min.y;

            // Move the parts of the page below the grid down.
            float lowest = float.MaxValue;
            foreach (Transform c in page)
            {
                if (c == grid.transform) continue;
                Bounds b = PageBounds(page, c);
                if (b.size == Vector3.zero || b.max.y >= oldBottom) continue;
                if (shift > 0f) SetPagePos(page, c, PagePos(page, c) - new Vector3(0f, shift, 0f));
                lowest = Mathf.Min(lowest, b.min.y - shift);
            }
            if (lowest <= line.max.y)
                throw new InvalidOperationException("the Graphics page with the rows of the port goes below the tooltip line (" + lowest +
                                                    " <= " + line.max.y + ")");

            // The sharpening slider: a copy of the Gamma slider, to the right of
            // Graphics Quality, with the gap between the Gamma and Frame Rate
            // Max sliders. Graphics Quality (in the middle of the page) moves
            // to the left edge of the tooltip line.
            Transform quality = SliderUnit(page, mgr.QualitySlider.transform);
            Transform gamma = SliderUnit(page, mgr.GammaSlider.transform);
            Transform frameRate = SliderUnit(page, mgr.FrameRateMaxSlider.transform);
            Bounds qb = PageBounds(page, quality), gb = PageBounds(page, gamma), fb = PageBounds(page, frameRate);
            float dx = line.min.x - qb.min.x;
            SetPagePos(page, quality, PagePos(page, quality) + new Vector3(dx, 0f, 0f));
            Vector3 gp = PagePos(page, gamma);
            float gap = fb.min.x - gb.max.x;
            float x = qb.max.x + dx + gap - (gb.min.x - gp.x);
            if (x + (gb.max.x - gp.x) > line.max.x)
                throw new InvalidOperationException("the sharpening slider does not fit to the right of Graphics Quality (Graphics Quality " +
                                                    (qb.min.x + dx) + " to " + (qb.max.x + dx) + ", slider " + (x + gb.min.x - gp.x) + " to " +
                                                    (x + gb.max.x - gp.x) + ", tooltip line " + line.min.x + " to " + line.max.x + ")");
            AddSharpness(page, gamma, new Vector3(x, PagePos(page, quality).y, gp.z));

            Refresh();
            Debug.Log("PortGraphics: " + Rows.Length + " checkbox rows and the sharpening slider are on the Graphics page; the sliders " +
                      "moved down " + shift + " units");
        }

        // The smallest ancestor of a slider group (below the page) that holds
        // no other slider group: the group with its title.
        static Transform SliderUnit(Transform page, Transform group)
        {
            Transform unit = group;
            while (unit.parent != null && unit.parent != page &&
                   unit.parent.GetComponentsInChildren<UIOptionsSliderGroup>(true).Length == 1)
                unit = unit.parent;
            return unit;
        }

        // Remove the components that connect a copy to a game option or set
        // its text.
        static void Detach(GameObject go)
        {
            foreach (UIOptionsTag t in go.GetComponentsInChildren<UIOptionsTag>(true)) UnityEngine.Object.DestroyImmediate(t);
            foreach (UIOptionsSliderTag t in go.GetComponentsInChildren<UIOptionsSliderTag>(true)) UnityEngine.Object.DestroyImmediate(t);
            foreach (GUIStringLabel t in go.GetComponentsInChildren<GUIStringLabel>(true)) UnityEngine.Object.DestroyImmediate(t);
            foreach (UIOptionsTooltipTrigger t in go.GetComponentsInChildren<UIOptionsTooltipTrigger>(true))
                UnityEngine.Object.DestroyImmediate(t);
        }

        static GameObject Copy(GameObject src, Transform parent, string name)
        {
            GameObject go = (GameObject)UnityEngine.Object.Instantiate(src);
            go.name = RowPrefix + name;
            go.transform.parent = parent;
            go.transform.localPosition = src.transform.localPosition;
            go.transform.localRotation = src.transform.localRotation;
            go.transform.localScale = src.transform.localScale;
            return go;
        }

        static void AddTooltip(GameObject go, string text)
        {
            foreach (Collider c in go.GetComponentsInChildren<Collider>(true))
            {
                UIEventListener l = UIEventListener.Get(c.gameObject);
                l.onHover = (UIEventListener.BoolDelegate)Delegate.Combine(l.onHover,
                    new UIEventListener.BoolDelegate((g, over) => { if (over) UIOptionsTooltip.Show(text); }));
            }
        }

        static void AddCheckRow(Transform grid, UIOptionsTag template, CheckRow row, int index)
        {
            GameObject go = Copy(template.gameObject, grid, index.ToString());
            UIOptionsTag tag = go.GetComponent<UIOptionsTag>();
            if (tag == null || tag.Checkbox == null || tag.Label == null)
                throw new InvalidOperationException("the copy of a checkbox row has no UIOptionsTag, checkbox or label");
            UICheckbox box = tag.Checkbox;
            UILabel label = tag.Label;
            if (!box.transform.IsChildOf(go.transform) || !label.transform.IsChildOf(go.transform))
                throw new InvalidOperationException("the checkbox or label of a checkbox row is not part of the row");
            Detach(go);
            label.text = row.Label;
            box.eventReceiver = null;
            box.functionName = null;
            box.onStateChange = (sender, state) => OnCheck(row, state);
            row.Box = box;
            AddTooltip(go, row.Tooltip);
            // A click on the label changes the checkbox, as on the game's rows:
            // the UIMultiSpriteImageButton of the copy sends a click on a
            // collider of the row to the other colliders of the row, which
            // include the checkbox. Another click handler here would change
            // the checkbox a second time.
            if (go.GetComponent<UIMultiSpriteImageButton>() == null)
                throw new InvalidOperationException("the copy of a checkbox row has no UIMultiSpriteImageButton");
        }

        static void AddSharpness(Transform page, Transform gammaUnit, Vector3 pos)
        {
            GameObject go = Copy(gammaUnit.gameObject, gammaUnit.parent, "sharpness");
            UIOptionsSliderGroup group = go.GetComponentInChildren<UIOptionsSliderGroup>();
            if (group == null || group.Slider == null) throw new InvalidOperationException("the copy of the Gamma slider has no slider group");
            // The title: the label of the copy that is not the number label.
            UILabel title = null;
            foreach (UILabel l in go.GetComponentsInChildren<UILabel>(true))
                if (l != group.NumberLabel) title = l;
            if (title == null) throw new InvalidOperationException("the copy of the Gamma slider has no title");
            Detach(go);
            SetPagePos(page, go.transform, pos);
            title.text = SharpnessLabel;
            group.OnChanged = null;
            group.Slider.OnChanged = null;
            // The slider has the values 0 to Range - 1.
            group.Slider.Range = SharpnessMax + 1;
            group.Slider.SpotTooltipStrings = null;
            group.NumberMultiplier = 1f;
            group.NumberAdd = 0f;
            // No format string (StringID -1): UpdateText shows the number.
            group.FormatString = new GUIDatabaseString();
            group.FormatString.StringID = -1;
            group.Slider.OnChanged = (sender, setting) => OnSharpness(setting);
            s_SharpnessGroup = group;
            AddTooltip(go, SharpnessTooltip);
        }

        // Show the saved values. The number label of the slider follows
        // (UIOptionsSliderGroup.OnSliderChanged).
        static void Refresh()
        {
            s_Updating = true;
            try
            {
                foreach (CheckRow r in Rows) r.Box.isChecked = PortGfxGet(r.Id) != 0;
                s_SharpnessGroup.Slider.Setting = PortGfxGet(Sharpness);
            }
            finally
            {
                s_Updating = false;
            }
        }

        static void OnCheck(CheckRow row, bool state)
        {
            try
            {
                if (s_Updating) return;
                Save(row.Id, state ? 1 : 0);
            }
            catch (Exception e)
            {
                Fail("OnCheck", e);
            }
        }

        static void OnSharpness(int setting)
        {
            try
            {
                if (s_Updating) return;
                Save(Sharpness, setting);
            }
            catch (Exception e)
            {
                Fail("OnSharpness", e);
            }
        }

        static void Save(int id, int value)
        {
            if (PortGfxSet(id, value) != 0) return;
            Refresh();
            UIWindowManager.ShowMessageBox(UIMessageBox.ButtonStyle.OK, "Graphics",
                "The graphics setting could not be saved, so it was not changed. The log of the port has the cause.");
        }
    }
}
