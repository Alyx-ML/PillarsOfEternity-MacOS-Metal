/*
 * The launcher window of the loader. See rt_launcher.h.
 *
 * The layout and the code come from the launcher of the Prototype port.
 * The window is dark, with the title in Baskerville and a gold accent. The
 * controls are standard AppKit controls in the dark appearance:
 *   - left: the graphics settings,
 *   - right, top: the game folder,
 *   - right, bottom: Restore Defaults and Play (Return), level with the
 *     bottom of the graphics settings.
 *
 * The header shows ScreenSelector.bmp of the selected game folder (the
 * banner of the game's own settings window). The repository and the
 * release hold no game art, so the window shows the text title until a
 * usable folder is selected.
 *
 * The settings file is the file of rt_portgfx.c (rt_portgfx_defs.h): one
 * "<key> <value>" line for each setting, and comment lines that start with
 * '#'. A file that does not exist gives the initial values. A file with an
 * error is not changed until the player selects Play; the window tells the
 * player about the error. The labels and hints are the labels and tooltips
 * of the same settings on the Graphics tab of the game
 * (tools/port_graphics/PortGraphics.cs).
 */
#import <Cocoa/Cocoa.h>

#include "rt_launcher.h"
#include "rt_portgfx_defs.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define LAUNCHER_PATH_LEN 1024
#define MAX_VALUES 9
#define BANNER_FILE "PillarsOfEternity_data/ScreenSelector.bmp"

/* The text of a setting in the window. */
typedef struct setting_text {
    unsigned id; /* index in rt_gfx_defs */
    const char *label;
    const char *hint;
    const char *values[MAX_VALUES]; /* one name for each value 0 to the maximum */
} setting_text;

static const setting_text graphics_rows[] = {
    { 0, "MetalFX Upscaling",
      "Upscales the game image to the resolution of the screen with Apple MetalFX. When this is off, the image is "
      "stretched with bilinear filtering.",
      { "Off", "On" } },
    { 1, "RCAS Sharpening",
      "Sharpens the final image with AMD FidelityFX RCAS. 0 turns sharpening off; 1 to 8 set the strength, and 8 is "
      "the sharpest.",
      { "0 (Off)", "1", "2", "3", "4", "5", "6", "7", "8" } },
    { 2, "HDR Output",
      "Shows the game with HDR and GT7 tone mapping on a screen that has HDR. When this is off, the game is shown in "
      "standard dynamic range.",
      { "Off", "On" } },
    { 3, "16x Anisotropic Filtering",
      "Uses 16x anisotropic filtering for the textures of the game, so that surfaces seen at an angle stay sharp.",
      { "Off", "On" } },
    { 4, "Supersampled Shadows",
      "Draws the dynamic shadows of characters at 4 times the resolution in each direction.", { "Off", "On" } },
    { 5, "32-bit Render Targets",
      "Keeps the intermediate images of the game in 32-bit float, for more precise blending.", { "Off", "On" } },
};

#define NGRAPHICS (sizeof graphics_rows / sizeof graphics_rows[0])
_Static_assert(NGRAPHICS == RT_GFX_DEF_COUNT, "each setting needs one row in the launcher window");

/* ---------------------------------------------------------------- palette */

static NSColor *rgb(unsigned hex)
{
    return [NSColor colorWithSRGBRed:(CGFloat)((hex >> 16) & 0xFFu) / 255.0
                               green:(CGFloat)((hex >> 8) & 0xFFu) / 255.0
                                blue:(CGFloat)(hex & 0xFFu) / 255.0
                               alpha:1.0];
}

#define C_GROUND 0x0B0A08u    /* window */
#define C_PANEL 0x16130Fu     /* setting groups */
#define C_TEXT 0xEDE6D6u      /* labels (15:1 on the panel) */
#define C_HINT 0xA39B8Bu      /* hints (6.7:1 on the panel) */
#define C_GOLD 0xB8913Au      /* the rule */
#define C_BUTTON 0x8A6A22u    /* the Play button (white text 5:1) */
#define C_GOLD_TEXT 0xD4AF5Au /* group titles (9:1 on the window) */
#define C_OK 0x6CC08Bu
#define C_WARN 0xE8A93Eu

/* ---------------------------------------------------------------- errors */

static _Noreturn void launcher_fail(NSString *title, NSString *text)
{
    NSAlert *a = [NSAlert new];
    fprintf(stderr, "Pillars of Eternity: %s %s\n", title.UTF8String, text.UTF8String);
    a.alertStyle = NSAlertStyleCritical;
    a.messageText = title;
    a.informativeText = text;
    [a runModal];
    exit(1);
}

static NSString *nsstr(const char *s)
{
    NSString *r = [NSString stringWithUTF8String:s];
    return r != nil ? r : @"(a name that is not UTF-8)";
}

static NSFont *font_named(NSString *name, CGFloat size)
{
    NSFont *f = [NSFont fontWithName:name size:size];
    if (f == nil)
        launcher_fail(@"A font of macOS is missing.", [NSString stringWithFormat:@"The font %@ is not installed.", name]);
    return f;
}

/* ---------------------------------------------------------------- settings file */

/* Read the settings file into values (the initial values first), with the
   rules of rt_portgfx_init. Returns nil, or the problem of the file. */
static NSString *read_settings(const char *path, int values[RT_GFX_DEF_COUNT])
{
    char line[256];
    unsigned i, num = 0;
    FILE *f;
    for (i = 0; i < RT_GFX_DEF_COUNT; i++) values[i] = rt_gfx_defs[i].initial;
    f = fopen(path, "r");
    if (f == NULL)
    {
        if (errno == ENOENT) return nil;
        return [NSString stringWithFormat:@"The settings file cannot be read (%s).", strerror(errno)];
    }
    while (fgets(line, sizeof line, f) != NULL)
    {
        char key[64], extra[2];
        int value;
        bool found = false;
        num++;
        if (strchr(line, '\n') == NULL && !feof(f))
        {
            fclose(f);
            return [NSString stringWithFormat:@"Line %u of the settings file is too long.", num];
        }
        if (line[0] == '#' || line[strspn(line, " \t\r\n")] == 0) continue;
        if (sscanf(line, "%63s %d %1s", key, &value, extra) != 2)
        {
            fclose(f);
            return [NSString stringWithFormat:@"Line %u of the settings file is not \"<setting> <value>\".", num];
        }
        for (i = 0; i < RT_GFX_DEF_COUNT; i++)
        {
            if (strcmp(key, rt_gfx_defs[i].key) != 0) continue;
            found = true;
            if (value < rt_gfx_defs[i].min || value > rt_gfx_defs[i].max)
            {
                fclose(f);
                return [NSString stringWithFormat:@"Line %u of the settings file: %s %d is not in the range %d to %d.",
                                                  num, key, value, rt_gfx_defs[i].min, rt_gfx_defs[i].max];
            }
            values[i] = value;
        }
        if (!found)
        {
            fclose(f);
            return [NSString stringWithFormat:@"Line %u of the settings file has the unknown setting %s.", num, key];
        }
    }
    if (ferror(f))
    {
        int e = errno;
        fclose(f);
        return [NSString stringWithFormat:@"The settings file cannot be read (%s).", strerror(e)];
    }
    fclose(f);
    return nil;
}

/* Write the settings file (a temporary file, then a rename), in the format
   of rt_portgfx.c. Returns nil, or the problem. */
static NSString *write_settings(const char *path, const int values[RT_GFX_DEF_COUNT])
{
    char tmp[LAUNCHER_PATH_LEN + 8];
    NSString *dir = nsstr(path).stringByDeletingLastPathComponent;
    NSError *err = nil;
    FILE *f;
    unsigned i;
    int r = snprintf(tmp, sizeof tmp, "%s.tmp", path);
    if (r <= 0 || (size_t)r >= sizeof tmp) return @"The path of the settings file is too long.";
    if (![NSFileManager.defaultManager createDirectoryAtPath:dir withIntermediateDirectories:YES attributes:nil error:&err])
        return [NSString stringWithFormat:@"The folder %@ cannot be made: %@", dir, err.localizedDescription];
    f = fopen(tmp, "w");
    if (f == NULL) return [NSString stringWithFormat:@"The settings file cannot be written (%s).", strerror(errno)];
    r = fputs(RT_GFX_FILE_COMMENT, f);
    for (i = 0; r >= 0 && i < RT_GFX_DEF_COUNT; i++) r = fprintf(f, "%s %d\n", rt_gfx_defs[i].key, values[i]);
    if (r < 0)
    {
        int e = errno;
        fclose(f);
        remove(tmp);
        return [NSString stringWithFormat:@"The settings file cannot be written (%s).", strerror(e)];
    }
    if (fclose(f) != 0)
    {
        int e = errno;
        remove(tmp);
        return [NSString stringWithFormat:@"The settings file cannot be written (%s).", strerror(e)];
    }
    if (rename(tmp, path) != 0)
    {
        int e = errno;
        remove(tmp);
        return [NSString stringWithFormat:@"The settings file cannot be replaced (%s).", strerror(e)];
    }
    return nil;
}

/* ---------------------------------------------------------------- window */

@interface RTLauncher : NSObject <NSWindowDelegate>
@property(nonatomic) NSWindow *window;
@property(nonatomic) NSTextField *path;
@property(nonatomic) NSImageView *statusIcon;
@property(nonatomic) NSTextField *status;
@property(nonatomic) NSTextField *banner;
@property(nonatomic) NSButton *play;
@property(nonatomic) NSMutableDictionary<NSNumber *, NSPopUpButton *> *popups;
@property(nonatomic) BOOL finished;
@property(nonatomic) BOOL playChosen;
@property(nonatomic) NSTextField *titleText; /* shown until the art of a game folder is loaded */
@property(nonatomic) NSImageView *logo;
@end

@implementation RTLauncher
{
@public
    char settingsPath[LAUNCHER_PATH_LEN];
    char dir[LAUNCHER_PATH_LEN];
    bool dirOK;
    rt_launcher_check_fn check;
    char artDir[LAUNCHER_PATH_LEN]; /* the folder of the shown art, or "" */
}

static NSTextField *text_label(NSString *s, NSFont *font, unsigned color)
{
    NSTextField *t = [NSTextField labelWithString:s];
    t.font = font;
    t.textColor = rgb(color);
    return t;
}

static NSTextField *group_title(NSString *s)
{
    NSTextField *t = [NSTextField labelWithAttributedString:[[NSAttributedString alloc]
                                                                initWithString:s.uppercaseString
                                                                    attributes:@{
                                                                        NSFontAttributeName : font_named(@"Optima-Bold", 14),
                                                                        NSForegroundColorAttributeName : rgb(C_GOLD_TEXT),
                                                                        NSKernAttributeName : @1.8,
                                                                    }]];
    t.accessibilityRole = NSAccessibilityStaticTextRole;
    return t;
}

#define PANEL_PAD 20 /* the left and right inset of a panel (panel()) */
#define MENU_WIDTH 118
#define GRID_GAP 16
#define HEADER_HEIGHT 96

/* A setting group for a panel of the given width: the title, then one row
   for each setting (the label and its hint at the left, the menu at the
   right). */
- (NSStackView *)groupWithTitle:(NSString *)title
                           rows:(const setting_text *)rows
                          count:(size_t)count
                     panelWidth:(CGFloat)width
{
    NSMutableArray<NSArray<NSView *> *> *cells = [NSMutableArray new];
    NSGridView *grid;
    NSStackView *stack;
    size_t i;
    for (i = 0; i < count; i++)
    {
        const setting_text *st = &rows[i];
        const rt_gfx_def *def = &rt_gfx_defs[st->id];
        NSTextField *label = text_label(nsstr(st->label), [NSFont systemFontOfSize:13 weight:NSFontWeightMedium], C_TEXT);
        NSString *hintText = nsstr(st->hint);
        NSTextField *hint;
        NSStackView *texts;
        NSPopUpButton *pop = [[NSPopUpButton alloc] initWithFrame:NSZeroRect pullsDown:NO];
        int v;
        if (def->restart) hintText = [hintText stringByAppendingString:@" A change takes effect when the game starts."];
        hint = text_label(hintText, [NSFont systemFontOfSize:11], C_HINT);
        texts = [NSStackView stackViewWithViews:@[ label, hint ]];
        texts.orientation = NSUserInterfaceLayoutOrientationVertical;
        texts.alignment = NSLayoutAttributeLeading;
        texts.spacing = 1;
        hint.lineBreakMode = NSLineBreakByWordWrapping;
        hint.preferredMaxLayoutWidth = width - 2 * PANEL_PAD - MENU_WIDTH - GRID_GAP;
        if (def->min != 0 || def->max >= MAX_VALUES)
            launcher_fail(@"The launcher is damaged.", @"A setting has a range that the window cannot show.");
        for (v = 0; v <= def->max; v++)
        {
            if (st->values[v] == NULL) launcher_fail(@"The launcher is damaged.", @"A setting value has no name.");
            [pop addItemWithTitle:nsstr(st->values[v])];
        }
        pop.accessibilityLabel = nsstr(st->label);
        pop.toolTip = hintText;
        [pop.widthAnchor constraintEqualToConstant:MENU_WIDTH].active = YES;
        self.popups[@(st->id)] = pop;
        [cells addObject:@[ texts, pop ]];
    }
    grid = [NSGridView gridViewWithViews:cells];
    grid.rowSpacing = 14;
    grid.columnSpacing = GRID_GAP;
    [grid columnAtIndex:1].xPlacement = NSGridCellPlacementTrailing;
    grid.rowAlignment = NSGridRowAlignmentNone;
    for (i = 0; i < count; i++) [grid rowAtIndex:(NSInteger)i].yPlacement = NSGridCellPlacementCenter;
    stack = [NSStackView stackViewWithViews:@[ group_title(title), grid ]];
    stack.orientation = NSUserInterfaceLayoutOrientationVertical;
    stack.alignment = NSLayoutAttributeLeading;
    stack.spacing = 12;
    [grid.widthAnchor constraintEqualToAnchor:stack.widthAnchor].active = YES;
    return stack;
}

/* A panel hugs its content, and the content wants the smallest height
   that its views allow (a weak height-0 constraint: weaker than the
   compression resistance of the views, 750, stronger than the hugging of
   the right column, 250). So the free space of the right column goes
   between the panel and the buttons, not into the rows of a panel. A
   panel grows only when a required constraint makes it taller. */
static NSView *panel(NSStackView *content, CGFloat width)
{
    NSView *p = [NSView new];
    NSLayoutConstraint *hug, *shortest;
    p.wantsLayer = YES;
    p.layer.backgroundColor = rgb(C_PANEL).CGColor;
    p.layer.cornerRadius = 12;
    content.translatesAutoresizingMaskIntoConstraints = NO;
    [p addSubview:content];
    [NSLayoutConstraint activateConstraints:@[
        [content.leadingAnchor constraintEqualToAnchor:p.leadingAnchor constant:PANEL_PAD],
        [content.trailingAnchor constraintEqualToAnchor:p.trailingAnchor constant:-PANEL_PAD],
        [content.topAnchor constraintEqualToAnchor:p.topAnchor constant:18],
        [content.bottomAnchor constraintLessThanOrEqualToAnchor:p.bottomAnchor constant:-20],
        [p.widthAnchor constraintEqualToConstant:width],
    ]];
    hug = [content.bottomAnchor constraintEqualToAnchor:p.bottomAnchor constant:-20];
    hug.priority = NSLayoutPriorityDefaultHigh;
    hug.active = YES;
    shortest = [content.heightAnchor constraintEqualToConstant:0];
    shortest.priority = 500;
    shortest.active = YES;
    return p;
}

- (NSStackView *)folderGroup
{
    NSButton *choose = [NSButton buttonWithTitle:@"Choose Folder…" target:self action:@selector(choose:)];
    NSStackView *statusRow, *stack;
    self.path = text_label(@"", [NSFont systemFontOfSize:12], C_TEXT);
    self.path.lineBreakMode = NSLineBreakByTruncatingMiddle;
    self.path.selectable = YES;
    self.path.accessibilityLabel = @"Game folder";
    [self.path setContentCompressionResistancePriority:NSLayoutPriorityDefaultLow
                                        forOrientation:NSLayoutConstraintOrientationHorizontal];
    self.statusIcon = [NSImageView new];
    [self.statusIcon.widthAnchor constraintEqualToConstant:16].active = YES;
    [self.statusIcon.heightAnchor constraintEqualToConstant:16].active = YES;
    self.status = text_label(@"", [NSFont systemFontOfSize:12], C_HINT);
    self.status.lineBreakMode = NSLineBreakByWordWrapping;
    self.status.preferredMaxLayoutWidth = 250;
    statusRow = [NSStackView stackViewWithViews:@[ self.statusIcon, self.status ]];
    statusRow.alignment = NSLayoutAttributeTop;
    statusRow.spacing = 8;
    stack = [NSStackView stackViewWithViews:@[ group_title(@"Game folder"), self.path, statusRow, choose ]];
    stack.orientation = NSUserInterfaceLayoutOrientationVertical;
    stack.alignment = NSLayoutAttributeLeading;
    stack.spacing = 10;
    [stack setCustomSpacing:14 afterView:statusRow];
    [self.path.widthAnchor constraintEqualToAnchor:stack.widthAnchor].active = YES;
    [statusRow.widthAnchor constraintEqualToAnchor:stack.widthAnchor].active = YES;
    return stack;
}

- (void)showFolder
{
    NSString *problem;
    NSImage *img;
    unsigned color;
    if (dir[0] == 0)
    {
        self.path.stringValue = @"No folder selected";
        self.path.textColor = rgb(C_HINT);
        problem = @"Choose the installed GOG game folder (the folder that holds PillarsOfEternity.exe).";
        dirOK = false;
    }
    else
    {
        self.path.stringValue = [nsstr(dir) stringByAbbreviatingWithTildeInPath];
        self.path.textColor = rgb(C_TEXT);
        self.path.toolTip = nsstr(dir);
        problem = check(dir);
        dirOK = problem == nil;
    }
    color = dirOK ? C_OK : C_WARN;
    img = [NSImage imageWithSystemSymbolName:dirOK ? @"checkmark.circle.fill" : @"exclamationmark.triangle.fill"
                    accessibilityDescription:dirOK ? @"Ready" : @"Problem"];
    if (img == nil) launcher_fail(@"A symbol of macOS is missing.", @"The status symbol cannot be loaded.");
    self.statusIcon.image = img;
    self.statusIcon.contentTintColor = rgb(color);
    self.status.stringValue = dirOK ? @"The GOG version of Pillars of Eternity. Ready to play." : problem;
    self.status.textColor = dirOK ? rgb(C_TEXT) : rgb(C_WARN);
    self.play.enabled = dirOK;
    [self updateArt];
}

- (void)clearArt
{
    self.logo.image = nil;
    self.logo.hidden = YES;
    self.titleText.hidden = NO;
    artDir[0] = 0;
}

/* Show the banner of the selected game folder. */
- (void)updateArt
{
    char path[LAUNCHER_PATH_LEN + 64];
    NSImage *img;
    if (!dirOK)
    {
        if (artDir[0] != 0) [self clearArt];
        return;
    }
    if (strcmp(artDir, dir) == 0) return;
    if ((size_t)snprintf(path, sizeof path, "%s/%s", dir, BANNER_FILE) >= sizeof path)
    {
        [self clearArt];
        [self showProblem:@"The title art cannot be shown: the folder path is too long."];
        return;
    }
    img = [[NSImage alloc] initWithContentsOfFile:nsstr(path)];
    if (img == nil || img.size.width <= 0 || img.size.height <= 0)
    {
        [self clearArt];
        [self showProblem:[NSString stringWithFormat:@"The title art cannot be shown: %s cannot be read.", path]];
        return;
    }
    self.logo.image = img;
    self.titleText.hidden = YES;
    self.logo.hidden = NO;
    memcpy(artDir, dir, sizeof artDir);
}

- (void)showValues:(const int *)values
{
    unsigned i;
    for (i = 0; i < RT_GFX_DEF_COUNT; i++) [self.popups[@(i)] selectItemAtIndex:values[i] - rt_gfx_defs[i].min];
}

- (void)build
{
    NSTextField *title, *versionNote;
    NSView *rule, *left, *content;
    NSStackView *header, *columns, *rightStack, *buttons, *notes, *actions, *root;
    NSButton *restore;
    self.popups = [NSMutableDictionary new];

    self.window = [[NSWindow alloc] initWithContentRect:NSMakeRect(0, 0, 800, 640)
                                              styleMask:NSWindowStyleMaskTitled | NSWindowStyleMaskClosable |
                                                        NSWindowStyleMaskMiniaturizable |
                                                        NSWindowStyleMaskFullSizeContentView
                                                backing:NSBackingStoreBuffered
                                                  defer:NO];
    self.window.title = @"Pillars of Eternity";
    self.window.titleVisibility = NSWindowTitleHidden;
    self.window.titlebarAppearsTransparent = YES;
    self.window.appearance = [NSAppearance appearanceNamed:NSAppearanceNameDarkAqua];
    self.window.backgroundColor = rgb(C_GROUND);
    self.window.releasedWhenClosed = NO;
    self.window.delegate = self;

    title = [NSTextField labelWithAttributedString:[[NSAttributedString alloc]
                                                       initWithString:@"PILLARS OF ETERNITY"
                                                           attributes:@{
                                                               NSFontAttributeName : font_named(@"Baskerville-SemiBold", 44),
                                                               NSForegroundColorAttributeName : rgb(C_TEXT),
                                                               NSKernAttributeName : @3.0,
                                                           }]];
    self.titleText = title;
    self.logo = [NSImageView new];
    self.logo.imageScaling = NSImageScaleProportionallyUpOrDown;
    self.logo.imageAlignment = NSImageAlignLeft;
    self.logo.accessibilityLabel = @"Pillars of Eternity";
    self.logo.hidden = YES;
    /* The text title and the banner have the same height, so the window
       does not change size when the art is loaded. */
    [title.heightAnchor constraintEqualToConstant:HEADER_HEIGHT].active = YES;
    [self.logo.heightAnchor constraintEqualToConstant:HEADER_HEIGHT].active = YES;
    [self.logo.widthAnchor constraintEqualToConstant:HEADER_HEIGHT * 432.0 / 163.0].active = YES;
    header = [NSStackView stackViewWithViews:@[ title, self.logo ]];
    header.orientation = NSUserInterfaceLayoutOrientationVertical;
    header.alignment = NSLayoutAttributeLeading;
    header.spacing = 0;

    rule = [NSView new];
    rule.wantsLayer = YES;
    rule.layer.backgroundColor = rgb(C_GOLD).CGColor;
    [rule.heightAnchor constraintEqualToConstant:2].active = YES;

    self.play = [NSButton buttonWithTitle:@"Play" target:self action:@selector(play:)];
    self.play.keyEquivalent = @"\r";
    self.play.controlSize = NSControlSizeLarge;
    self.play.bezelColor = rgb(C_BUTTON);
    self.play.font = [NSFont systemFontOfSize:15 weight:NSFontWeightSemibold];
    [self.play.widthAnchor constraintGreaterThanOrEqualToConstant:120].active = YES;

    restore = [NSButton buttonWithTitle:@"Restore Defaults" target:self action:@selector(restore:)];
    restore.controlSize = NSControlSizeLarge;
    versionNote = text_label(@"The game uses these settings when it starts. The Graphics tab of the game's options "
                             @"has the same settings.",
                             [NSFont systemFontOfSize:11], C_HINT);
    self.banner = text_label(@"", [NSFont systemFontOfSize:12 weight:NSFontWeightMedium], C_WARN);
    self.banner.lineBreakMode = NSLineBreakByWordWrapping;
    self.banner.preferredMaxLayoutWidth = 330;
    self.banner.hidden = YES;
    versionNote.lineBreakMode = NSLineBreakByWordWrapping;
    versionNote.preferredMaxLayoutWidth = 330;
    notes = [NSStackView stackViewWithViews:@[ self.banner, versionNote ]];
    notes.orientation = NSUserInterfaceLayoutOrientationVertical;
    notes.alignment = NSLayoutAttributeLeading;
    notes.spacing = 4;
    buttons = [NSStackView stackViewWithViews:@[ restore, self.play ]];
    buttons.distribution = NSStackViewDistributionEqualSpacing;
    actions = [NSStackView stackViewWithViews:@[ notes, buttons ]];
    actions.orientation = NSUserInterfaceLayoutOrientationVertical;
    actions.alignment = NSLayoutAttributeLeading;
    actions.spacing = 12;
    [buttons.widthAnchor constraintEqualToAnchor:actions.widthAnchor].active = YES;
    [notes.widthAnchor constraintEqualToAnchor:actions.widthAnchor].active = YES;
    [actions.widthAnchor constraintEqualToConstant:330].active = YES;
    {
        NSLayoutConstraint *shortest = [actions.heightAnchor constraintEqualToConstant:0];
        shortest.priority = 500; /* as in panel() */
        shortest.active = YES;
    }

    /* Left: Graphics. Right: Game folder at the top, the notes and the
       buttons at the bottom. The two columns have the same height, so the
       free space of the right column is between the panel and the buttons,
       and Play is level with the bottom of Graphics. */
    left = panel([self groupWithTitle:@"Graphics" rows:graphics_rows count:NGRAPHICS panelWidth:480], 480);
    rightStack = [NSStackView new];
    rightStack.orientation = NSUserInterfaceLayoutOrientationVertical;
    rightStack.alignment = NSLayoutAttributeLeading;
    rightStack.spacing = 16;
    [rightStack addView:panel([self folderGroup], 330) inGravity:NSStackViewGravityTop];
    [rightStack addView:actions inGravity:NSStackViewGravityBottom];
    [rightStack setHuggingPriority:NSLayoutPriorityDefaultLow forOrientation:NSLayoutConstraintOrientationVertical];
    columns = [NSStackView stackViewWithViews:@[ left, rightStack ]];
    columns.alignment = NSLayoutAttributeTop;
    columns.spacing = 16;
    [left.heightAnchor constraintEqualToAnchor:rightStack.heightAnchor].active = YES;

    root = [NSStackView stackViewWithViews:@[ header, rule, columns ]];
    root.orientation = NSUserInterfaceLayoutOrientationVertical;
    root.alignment = NSLayoutAttributeLeading;
    root.spacing = 16;
    [root setCustomSpacing:8 afterView:header];
    [root setCustomSpacing:22 afterView:rule];
    root.translatesAutoresizingMaskIntoConstraints = NO;
    content = self.window.contentView;
    [content addSubview:root];
    [NSLayoutConstraint activateConstraints:@[
        [root.leadingAnchor constraintEqualToAnchor:content.leadingAnchor constant:28],
        [root.trailingAnchor constraintEqualToAnchor:content.trailingAnchor constant:-28],
        [root.topAnchor constraintEqualToAnchor:content.topAnchor constant:36],
        [root.bottomAnchor constraintEqualToAnchor:content.bottomAnchor constant:-24],
        [rule.widthAnchor constraintEqualToAnchor:root.widthAnchor],
        [header.widthAnchor constraintEqualToAnchor:root.widthAnchor],
    ]];
    [self.window setContentSize:content.fittingSize];
    self.window.initialFirstResponder = self.play;
}

- (void)showProblem:(NSString *)text
{
    self.banner.stringValue = text != nil ? text : @"";
    self.banner.hidden = text == nil;
}

- (IBAction)choose:(id)sender
{
    NSOpenPanel *p = [NSOpenPanel openPanel];
    (void)sender;
    p.canChooseDirectories = YES;
    p.canChooseFiles = NO;
    p.allowsMultipleSelection = NO;
    p.prompt = @"Choose";
    p.message = @"Choose the installed GOG game folder of Pillars of Eternity (the folder that holds "
                @"PillarsOfEternity.exe).";
    if (dir[0] != 0) p.directoryURL = [NSURL fileURLWithPath:nsstr(dir) isDirectory:YES];
    [p beginSheetModalForWindow:self.window
              completionHandler:^(NSModalResponse r) {
                const char *fs;
                if (r != NSModalResponseOK || p.URL == nil) return;
                fs = p.URL.fileSystemRepresentation;
                if (fs == NULL || strlen(fs) >= sizeof self->dir)
                {
                    [self showProblem:@"The folder path is too long. Choose a folder with a shorter path."];
                    return;
                }
                memcpy(self->dir, fs, strlen(fs) + 1);
                [self showProblem:nil];
                [self showFolder];
              }];
}

- (IBAction)restore:(id)sender
{
    int values[RT_GFX_DEF_COUNT];
    unsigned i;
    (void)sender;
    for (i = 0; i < RT_GFX_DEF_COUNT; i++) values[i] = rt_gfx_defs[i].initial;
    [self showValues:values];
    [self showProblem:nil];
}

/* End the event loop of rt_launcher_run. The loop waits for the next
   event, so a posted event wakes it when the action came from outside the
   handling of an event (a menu action that AppKit sends later from the run
   loop). */
- (void)finishWithPlay:(BOOL)play
{
    NSEvent *wake;
    if (self.finished) return;
    self.finished = YES;
    self.playChosen = play;
    wake = [NSEvent otherEventWithType:NSEventTypeApplicationDefined
                              location:NSZeroPoint
                         modifierFlags:0
                             timestamp:0
                          windowNumber:0
                               context:nil
                               subtype:0
                                 data1:0
                                 data2:0];
    if (wake == nil) launcher_fail(@"The window cannot be closed.", @"The wake event cannot be made.");
    [NSApp postEvent:wake atStart:NO];
}

- (IBAction)play:(id)sender
{
    int values[RT_GFX_DEF_COUNT];
    unsigned i;
    NSString *problem;
    (void)sender;
    [self showFolder];
    if (!dirOK) return;
    for (i = 0; i < RT_GFX_DEF_COUNT; i++)
        values[i] = (int)self.popups[@(i)].indexOfSelectedItem + rt_gfx_defs[i].min;
    problem = write_settings(settingsPath, values);
    if (problem != nil)
    {
        [self showProblem:problem];
        return;
    }
    [self finishWithPlay:YES];
}

- (IBAction)quit:(id)sender
{
    (void)sender;
    [self finishWithPlay:NO];
}

- (BOOL)windowShouldClose:(NSWindow *)sender
{
    (void)sender;
    [self finishWithPlay:NO];
    return YES;
}

/* The quit Apple event (Quit in the Dock, log out, shut down). The loader
   does not call finishLaunching, so AppKit has no handler of its own; without
   this handler the event gets the error -1708 (not handled) and the window
   stays open. */
- (void)handleQuitEvent:(NSAppleEventDescriptor *)event withReplyEvent:(NSAppleEventDescriptor *)reply
{
    (void)event;
    (void)reply;
    [self finishWithPlay:NO];
}
@end

/* The application menu while the window is open: Quit closes the window. */
static void launcher_menu(RTLauncher *l)
{
    NSMenu *bar = [NSMenu new], *app = [NSMenu new];
    NSMenuItem *appItem = [NSMenuItem new], *quit;
    quit = [[NSMenuItem alloc] initWithTitle:@"Quit Pillars of Eternity" action:@selector(quit:) keyEquivalent:@"q"];
    quit.target = l;
    [app addItem:quit];
    appItem.submenu = app;
    [bar addItem:appItem];
    NSApp.mainMenu = bar;
}

bool rt_launcher_run(const char *settings, const char *initial, rt_launcher_check_fn check, char *out, size_t n)
{
    RTLauncher *l = [RTLauncher new];
    int values[RT_GFX_DEF_COUNT];
    NSString *problem;
    if (strlen(settings) >= sizeof l->settingsPath) launcher_fail(@"The settings path is too long.", nsstr(settings));
    memcpy(l->settingsPath, settings, strlen(settings) + 1);
    l->check = check;
    l->dir[0] = 0;
    if (initial != NULL)
    {
        if (strlen(initial) >= sizeof l->dir) launcher_fail(@"The game folder path is too long.", nsstr(initial));
        memcpy(l->dir, initial, strlen(initial) + 1);
    }
    [l build];
    launcher_menu(l);
    problem = read_settings(settings, values);
    [l showValues:values];
    if (problem != nil)
        [l showProblem:[problem stringByAppendingString:@" The window shows the default values. Play writes a new file."]];
    [l showFolder];
    [l.window center];
    [NSApp activate];
    [l.window makeKeyAndOrderFront:nil];
    /* An event loop in the default run loop mode, not runModalForWindow:.
       AppKit sends the action of a main-menu item (Quit, Command-Q) later
       from the run loop, and a modal loop (NSModalPanelRunLoopMode) does not
       run it. The loader uses the same loop for its progress window. */
    [NSAppleEventManager.sharedAppleEventManager setEventHandler:l
                                                     andSelector:@selector(handleQuitEvent:withReplyEvent:)
                                                   forEventClass:kCoreEventClass
                                                      andEventID:kAEQuitApplication];
    while (!l.finished)
    {
        @autoreleasepool
        {
            NSEvent *e = [NSApp nextEventMatchingMask:NSEventMaskAny
                                            untilDate:NSDate.distantFuture
                                               inMode:NSDefaultRunLoopMode
                                              dequeue:YES];
            if (e != nil && e.type != NSEventTypeApplicationDefined) [NSApp sendEvent:e];
        }
    }
    [NSAppleEventManager.sharedAppleEventManager removeEventHandlerForEventClass:kCoreEventClass
                                                                  andEventID:kAEQuitApplication];
    [l.window orderOut:nil];
    if (!l.playChosen) return false;
    if (strlen(l->dir) >= n) launcher_fail(@"The game folder path is too long.", nsstr(l->dir));
    memcpy(out, l->dir, strlen(l->dir) + 1);
    return true;
}
