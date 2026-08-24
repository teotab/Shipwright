#include "SohMenu.h"
#include "SohGui.hpp"
#include "soh/OTRGlobals.h"

extern "C" {
extern PlayState* gPlayState;
}

void WarpPointsWidget(WidgetInfo& info);

namespace SohGui {

extern std::shared_ptr<SohMenu> mSohMenu;
using namespace UIWidgets;

static const std::map<int32_t, const char*> logLevels = {
    { DEBUG_LOG_TRACE, "Trace" }, { DEBUG_LOG_DEBUG, "Debug" }, { DEBUG_LOG_INFO, "Info" },
    { DEBUG_LOG_WARN, "Warn" },   { DEBUG_LOG_ERROR, "Error" }, { DEBUG_LOG_CRITICAL, "Critical" },
    { DEBUG_LOG_OFF, "Off" },
};

#ifdef _DEBUG
DebugLogOption defaultLogLevel = DEBUG_LOG_TRACE;
#else
DebugLogOption defaultLogLevel = DEBUG_LOG_INFO;
#endif

static const std::map<int32_t, const char*> debugSaveFileModes = {
    { 0, "Off" },
    { 1, "Vanilla" },
    { 2, "Maxed" },
};

void SohMenu::AddMenuDevTools() {
    // Add Dev Tools Menu
    AddMenuEntry("Dev Tools", CVAR_SETTING("Menu.DevToolsSidebarSection"));

    // General
    AddSidebarEntry("Dev Tools", "General", 3);
    WidgetPath path = { "Dev Tools", "General", SECTION_COLUMN_1 };

    AddWidget(path, "Popout Menu", WIDGET_CVAR_CHECKBOX)
        .CVar("gSettings.Menu.Popout")
        .Options(CheckboxOptions().Tooltip("Changes the menu display from overlay to windowed."));
    AddWidget(path, "Debug Mode", WIDGET_CVAR_CHECKBOX)
        .CVar(CVAR_DEVELOPER_TOOLS("DebugEnabled"))
        .Options(
            CheckboxOptions().Tooltip("Enables Debug Mode, allowing you to select maps with L + R + Z, noclip "
                                      "with L + D-pad Right, and open the debug menu with L on the pause screen."));
    AddWidget(path, "Map Select Button Combination:", WIDGET_CVAR_BTN_SELECTOR)
        .CVar("gDeveloperTools.MapSelectBtn")
        .Options(BtnSelectorOptions().DefaultValue(BTN_R | BTN_L | BTN_Z))
        .PreFunc([](WidgetInfo& info) { info.isHidden = !CVarGetInteger(CVAR_DEVELOPER_TOOLS("DebugEnabled"), 0); });
    AddWidget(path, "No Clip Button Combination:", WIDGET_CVAR_BTN_SELECTOR)
        .CVar("gDeveloperTools.NoClipBtn")
        .PreFunc([](WidgetInfo& info) { info.isHidden = !CVarGetInteger(CVAR_DEVELOPER_TOOLS("DebugEnabled"), 0); })
        .Options(BtnSelectorOptions().DefaultValue(BTN_L | BTN_DRIGHT));
    AddWidget(path, "OoT Registry Editor", WIDGET_CVAR_CHECKBOX)
        .CVar(CVAR_DEVELOPER_TOOLS("RegEditEnabled"))
        .PreFunc([](WidgetInfo& info) { info.isHidden = !CVarGetInteger(CVAR_DEVELOPER_TOOLS("DebugEnabled"), 0); })
        .Options(CheckboxOptions().Tooltip("Enables the registry editor."));
    AddWidget(path, "Debug Save File Mode", WIDGET_CVAR_COMBOBOX)
        .CVar(CVAR_DEVELOPER_TOOLS("DebugSaveFileMode"))
        .PreFunc([](WidgetInfo& info) { info.isHidden = !CVarGetInteger(CVAR_DEVELOPER_TOOLS("DebugEnabled"), 0); })
        .Options(ComboboxOptions()
                     .Tooltip("Changes the behavior of debug file select creation (creating a save file on slot 1 "
                              "with debug mode on):\n"
                              "- Off: The debug save file will be a normal savefile.\n"
                              "- Vanilla: The debug save file will be the debug save file from the original game.\n"
                              "- Maxed: The debug save file will be a save file with all of the items & upgrades.")
                     .ComboMap(debugSaveFileModes)
                     .DefaultIndex(1));
    AddWidget(path, "OoT Skulltula Debug", WIDGET_CVAR_CHECKBOX)
        .CVar(CVAR_DEVELOPER_TOOLS("SkulltulaDebugEnabled"))
        .PreFunc([](WidgetInfo& info) { info.isHidden = !CVarGetInteger(CVAR_DEVELOPER_TOOLS("DebugEnabled"), 0); })
        .Options(CheckboxOptions().Tooltip("Enables Skulltula Debug, when moving the cursor in the menu above various "
                                           "map icons (boss key, compass, map screen locations, etc.) will set the GS "
                                           "bits in that area.\nUSE WITH CAUTION AS IT DOES NOT UPDATE THE GS COUNT!"));
    AddWidget(path, "Resource logging", WIDGET_CVAR_CHECKBOX)
        .CVar(CVAR_DEVELOPER_TOOLS("ResourceLogging"))
        .Options(CheckboxOptions().Tooltip("Logs some resources as XML when they're loaded in binary format."));

    AddWidget(path, "Frame Advance", WIDGET_CHECKBOX)
        .Options(CheckboxOptions().Tooltip(
            "This allows you to advance through the game one frame at a time on command. "
            "To advance a frame, hold Z and tap R on the second controller. Holding Z "
            "and R will advance a frame every half second. You can also use the buttons below."))
        .PreFunc([](WidgetInfo& info) {
            info.isHidden = mSohMenu->disabledMap.at(DISABLE_FOR_NULL_PLAY_STATE).active ||
                            mSohMenu->disabledMap.at(DISABLE_FOR_DEBUG_MODE_OFF).active;
            if (gPlayState != nullptr) {
                info.valuePointer = (bool*)&gPlayState->frameAdvCtx.enabled;
            } else {
                info.valuePointer = (bool*)nullptr;
            }
        });
    AddWidget(path, "Advance 1", WIDGET_BUTTON)
        .Options(ButtonOptions().Tooltip("Advance 1 frame.").Size(Sizes::Inline))
        .Callback([](WidgetInfo& info) { CVarSetInteger(CVAR_DEVELOPER_TOOLS("FrameAdvanceTick"), 1); })
        .PreFunc([](WidgetInfo& info) {
            info.isHidden = mSohMenu->disabledMap.at(DISABLE_FOR_FRAME_ADVANCE_OFF).active ||
                            mSohMenu->disabledMap.at(DISABLE_FOR_DEBUG_MODE_OFF).active;
        });
    AddWidget(path, "Advance (Hold)", WIDGET_BUTTON)
        .Options(ButtonOptions().Tooltip("Advance frames while the button is held.").Size(Sizes::Inline))
        .PreFunc([](WidgetInfo& info) {
            info.isHidden = mSohMenu->disabledMap.at(DISABLE_FOR_FRAME_ADVANCE_OFF).active ||
                            mSohMenu->disabledMap.at(DISABLE_FOR_DEBUG_MODE_OFF).active;
        })
        .PostFunc([](WidgetInfo& info) {
            if (ImGui::IsItemActive()) {
                CVarSetInteger(CVAR_DEVELOPER_TOOLS("FrameAdvanceTick"), 1);
            }
        })
        .SameLine(true);
    AddWidget(path, "Log Level", WIDGET_CVAR_COMBOBOX)
        .CVar(CVAR_DEVELOPER_TOOLS("LogLevel"))
        .Options(ComboboxOptions()
                     .Tooltip("The log level determines which messages are printed to the console."
                              " This does not affect the log file output")
                     .ComboMap(logLevels)
                     .DefaultIndex(defaultLogLevel))
        .Callback([](WidgetInfo& info) {
            Ship::Context::GetRawInstance()->GetLogger()->set_level(
                (spdlog::level::level_enum)CVarGetInteger(CVAR_DEVELOPER_TOOLS("LogLevel"), defaultLogLevel));
        });

    path.column = SECTION_COLUMN_2;
    AddWidget(path, "Warping", WIDGET_SEPARATOR_TEXT);
    AddWidget(path, "Better Debug Warp Screen", WIDGET_CVAR_CHECKBOX)
        .CVar(CVAR_DEVELOPER_TOOLS("BetterDebugWarpScreen"))
        .Options(
            CheckboxOptions()
                .Tooltip("Optimized Debug Warp Screen, with the added ability to choose entrances and time of day.")
                .DefaultValue(true));
    AddWidget(path, "Debug Warp Screen Translation", WIDGET_CVAR_CHECKBOX)
        .CVar(CVAR_DEVELOPER_TOOLS("DebugWarpScreenTranslation"))
        .Options(CheckboxOptions()
                     .Tooltip("Translate the Debug Warp Screen based on the game language.")
                     .DefaultValue(true));
    AddWidget(path, "Warp Points", WIDGET_CUSTOM).CustomFunction(WarpPointsWidget).HideInSearch(true);

    // Stats
    path.sidebarName = "Stats";
    AddSidebarEntry("Dev Tools", path.sidebarName, 1);
    AddWidget(path, "Popout Stats Window", WIDGET_WINDOW_BUTTON)
        .CVar(CVAR_WINDOW("SohStats"))
        .RaceDisable(false)
        .WindowName("Stats##Soh")
        .HideInSearch(true)
        .Options(WindowButtonOptions().Tooltip("Enables the separate Stats Window."));

    // Console
    path.sidebarName = "Console";
    AddSidebarEntry("Dev Tools", path.sidebarName, 1);
    AddWidget(path, "Popout Console", WIDGET_WINDOW_BUTTON)
        .CVar(CVAR_WINDOW("SohConsole"))
        .WindowName("Console##SoH")
        .HideInSearch(true)
        .Options(WindowButtonOptions().Tooltip("Enables the separate Console Window."));

    // Save Editor
    path.sidebarName = "Save Editor";
    AddSidebarEntry("Dev Tools", path.sidebarName, 1);
    AddWidget(path, "Popout Save Editor", WIDGET_WINDOW_BUTTON)
        .CVar(CVAR_WINDOW("SaveEditor"))
        .WindowName("Save Editor")
        .HideInSearch(true)
        .Options(WindowButtonOptions().Tooltip("Enables the separate Save Editor Window."));

    // Hook Debugger
    path.sidebarName = "Hook Debugger";
    AddSidebarEntry("Dev Tools", path.sidebarName, 1);
    AddWidget(path, "Popout Hook Debugger", WIDGET_WINDOW_BUTTON)
        .CVar(CVAR_WINDOW("HookDebugger"))
        .WindowName("Hook Debugger")
        .HideInSearch(true)
        .Options(WindowButtonOptions().Tooltip("Enables the separate Hook Debugger Window."));

    // Collision Viewer
    path.sidebarName = "Collision Viewer";
    AddSidebarEntry("Dev Tools", path.sidebarName, 2);
    AddWidget(path, "Popout Collision Viewer", WIDGET_WINDOW_BUTTON)
        .CVar(CVAR_WINDOW("CollisionViewer"))
        .WindowName("Collision Viewer")
        .HideInSearch(true)
        .Options(WindowButtonOptions().Tooltip("Enables the separate Collision Viewer Window."));

    // Actor Viewer
    path.sidebarName = "Actor Viewer";
    AddSidebarEntry("Dev Tools", path.sidebarName, 2);
    AddWidget(path, "Popout Actor Viewer", WIDGET_WINDOW_BUTTON)
        .CVar(CVAR_WINDOW("ActorViewer"))
        .WindowName("Actor Viewer")
        .HideInSearch(true)
        .Options(WindowButtonOptions().Tooltip("Enables the separate Actor Viewer Window."));

    // Display List Viewer
    path.sidebarName = "DList Viewer";
    AddSidebarEntry("Dev Tools", path.sidebarName, 2);
    AddWidget(path, "Popout Display List Viewer", WIDGET_WINDOW_BUTTON)
        .CVar(CVAR_WINDOW("DisplayListViewer"))
        .WindowName("Display List Viewer")
        .HideInSearch(true)
        .Options(WindowButtonOptions().Tooltip("Enables the separate Display List Viewer Window."));

    // Value Viewer
    path.sidebarName = "Value Viewer";
    AddSidebarEntry("Dev Tools", path.sidebarName, 2);
    AddWidget(path, "Popout Value Viewer", WIDGET_WINDOW_BUTTON)
        .CVar(CVAR_WINDOW("ValueViewer"))
        .WindowName("Value Viewer")
        .HideInSearch(true)
        .Options(WindowButtonOptions().Tooltip("Enables the separate Value Viewer Window."));

    // Message Viewer
    path.sidebarName = "Message Viewer";
    AddSidebarEntry("Dev Tools", path.sidebarName, 2);
    AddWidget(path, "Popout Message Viewer", WIDGET_WINDOW_BUTTON)
        .CVar(CVAR_WINDOW("MessageViewer"))
        .WindowName("Message Viewer")
        .HideInSearch(true)
        .Options(WindowButtonOptions().Tooltip("Enables the separate Message Viewer Window."));

    // Gfx Debugger
    path.sidebarName = "Gfx Debugger";
    AddSidebarEntry("Dev Tools", path.sidebarName, 1);
    AddWidget(path, "Popout Gfx Debugger", WIDGET_WINDOW_BUTTON)
        .CVar(CVAR_WINDOW("SohGfxDebugger"))
        .WindowName("GfxDebugger##SoH")
        .HideInSearch(true)
        .Options(WindowButtonOptions().Tooltip("Enables the separate Gfx Debugger Window."));

    // Cinematic Camera
    path.sidebarName = "Cinematic Cam";
    AddSidebarEntry("Dev Tools", path.sidebarName, 2);
    path.column = SECTION_COLUMN_1;
    AddWidget(path, "Open Path Editor", WIDGET_WINDOW_BUTTON)
        .CVar(CVAR_WINDOW("CinematicCamPath"))
        .WindowName("Cinematic Camera Path")
        .Options(
            WindowButtonOptions().Tooltip("Record camera keyframes and play back a smooth spline path through them."));
    AddWidget(path, "Enable Cinematic Camera", WIDGET_CVAR_CHECKBOX)
        .CVar(CVAR_ENHANCEMENT("CinematicCam.Enabled"))
        .Options(CheckboxOptions().DefaultValue(false).Tooltip(
            "A fully detached, controller-driven free camera for cinematic capture.\n\n"
            "Controls (player 1, all rebindable below):\n"
            "- Left stick: move / strafe (up = forward)\n"
            "- Right stick: look\n"
            "- R / Z: ascend / descend\n"
            "- Hold Modifier 2 (map it to a bumper): boost (fast) modifier\n"
            "- Hold L: precision (slow) modifier\n"
            "- D-pad Up/Down: FOV\n"
            "- D-pad Left/Right: roll\n\n"
            "While active the controller drives only the camera — Link ignores all input. (The one exception "
            "is 'Control Link during playback' in the path editor, which hands the stick back to Link "
            "while a path plays.)"));
    AddWidget(path, "Move Speed: %.0f", WIDGET_CVAR_SLIDER_FLOAT)
        .CVar(CVAR_ENHANCEMENT("CinematicCam.MoveSpeed"))
        .Options(FloatSliderOptions().Min(1.0f).Max(200.0f).DefaultValue(30.0f).Format("%.0f").Tooltip(
            "How fast the camera moves, in world units per frame."));
    AddWidget(path, "Boost Multiplier: %.1fx", WIDGET_CVAR_SLIDER_FLOAT)
        .CVar(CVAR_ENHANCEMENT("CinematicCam.BoostMultiplier"))
        .Options(FloatSliderOptions().Min(1.5f).Max(10.0f).DefaultValue(3.0f).Format("%.1f").Tooltip(
            "Speed multiplier while holding A."));
    AddWidget(path, "Look Sensitivity: %.2fx", WIDGET_CVAR_SLIDER_FLOAT)
        .CVar(CVAR_ENHANCEMENT("CinematicCam.LookSpeed"))
        .Options(FloatSliderOptions().Min(0.10f).Max(5.0f).DefaultValue(1.0f).Format("%.2f").Tooltip(
            "Right-stick look sensitivity multiplier."));
    AddWidget(path, "Smoothing", WIDGET_CVAR_SLIDER_FLOAT)
        .CVar(CVAR_ENHANCEMENT("CinematicCam.Smoothing"))
        .Options(FloatSliderOptions().IsPercentage().Min(0.0f).Max(0.95f).DefaultValue(0.5f).Tooltip(
            "Movement and look inertia. 0% is crisp and instant; higher values make the camera accelerate "
            "and glide to a stop for smooth, handheld-style motion."));
    AddWidget(path, "Invert Look X", WIDGET_CVAR_CHECKBOX)
        .CVar(CVAR_ENHANCEMENT("CinematicCam.InvertLookX"))
        .Options(CheckboxOptions().DefaultValue(false).Tooltip("Invert horizontal (yaw) look."));
    AddWidget(path, "Invert Look Y", WIDGET_CVAR_CHECKBOX)
        .CVar(CVAR_ENHANCEMENT("CinematicCam.InvertLookY"))
        .Options(CheckboxOptions().DefaultValue(false).Tooltip("Invert vertical (pitch) look."));
    AddWidget(path, "Freeze World", WIDGET_CVAR_CHECKBOX)
        .CVar(CVAR_ENHANCEMENT("CinematicCam.FreezeWorld"))
        .Options(CheckboxOptions().DefaultValue(true).Tooltip(
            "Freeze actors and physics while flying. Turn off to let the action continue (note: the controller "
            "still only drives the camera, not Link)."));
    AddWidget(path, "Freeze Sky & Time", WIDGET_CVAR_CHECKBOX)
        .CVar(CVAR_ENHANCEMENT("CinematicCam.FreezeSky"))
        .Options(CheckboxOptions().DefaultValue(false).Tooltip(
            "Stop the sky from drifting and the time of day from advancing (clouds, sun/moon, lighting). "
            "Makes looping clips/GIFs line up perfectly. Works independently of the camera."));
    AddWidget(path, "No Idle Fidgets (Link)", WIDGET_CVAR_CHECKBOX)
        .CVar(CVAR_ENHANCEMENT("CinematicCam.NoIdleFidget"))
        .Options(CheckboxOptions().DefaultValue(false).Tooltip(
            "Stop Link's idle fidget animations (stretching, looking around, tapping his foot) so he holds a "
            "clean standing pose for shots. Works independently of the camera."));
    AddWidget(path, "Sync Link Idle to Loop Start", WIDGET_CVAR_CHECKBOX)
        .CVar(CVAR_ENHANCEMENT("CinematicCam.SyncIdleAnim"))
        .Options(CheckboxOptions().DefaultValue(false).Tooltip(
            "Restart Link's standing idle (breathing/head-bob) animation from frame 0 each time playback starts "
            "from the top and each time a loop wraps. Make your timeline a whole number of idle cycles "
            "(~134 frames @ 30fps per cycle) for a perfectly seamless looping GIF. Pairs with No Idle Fidgets."));
    AddWidget(path, "Hide HUD", WIDGET_CVAR_CHECKBOX)
        .CVar(CVAR_ENHANCEMENT("CinematicCam.HideHud"))
        .Options(CheckboxOptions().DefaultValue(true).Tooltip(
            "Hide the in-game HUD/interface while the cinematic camera is active, for clean shots."));
    AddWidget(path, "Letterbox bars", WIDGET_CVAR_CHECKBOX)
        .CVar(CVAR_ENHANCEMENT("CinematicCam.Letterbox"))
        .Options(CheckboxOptions().DefaultValue(false).Tooltip(
            "Draw cinematic black bars at the top and bottom while the cinematic camera is active."));
    AddWidget(path, "Bar size: %.2f", WIDGET_CVAR_SLIDER_FLOAT)
        .CVar(CVAR_ENHANCEMENT("CinematicCam.LetterboxAmount"))
        .Options(FloatSliderOptions().Min(0.02f).Max(0.30f).DefaultValue(0.12f).Format("%.2f").Tooltip(
            "Height of each letterbox bar as a fraction of the screen. From 16:9, 0.12 gives about 2.35:1 and "
            "0.13 about 2.40:1 (CinemaScope); 0.06 gives about 2.00:1."))
        .PreFunc(
            [](WidgetInfo& info) { info.isHidden = !CVarGetInteger(CVAR_ENHANCEMENT("CinematicCam.Letterbox"), 0); });
    static const std::map<int32_t, const char*> cineGridModes = {
        { 0, "Off" }, { 1, "3 x 3 (thirds)" }, { 2, "4 x 4" }, { 3, "5 x 5" }
    };
    AddWidget(path, "Composition grid", WIDGET_CVAR_COMBOBOX)
        .CVar(CVAR_ENHANCEMENT("CinematicCam.Grid"))
        .Options(
            ComboboxOptions()
                .ComboMap(cineGridModes)
                .DefaultIndex(0)
                .Tooltip("Overlay a framing grid while the cinematic camera is active (rule-of-thirds and finer)."));
    AddWidget(path, "Show readout", WIDGET_CVAR_CHECKBOX)
        .CVar(CVAR_ENHANCEMENT("CinematicCam.ShowReadout"))
        .Options(CheckboxOptions().DefaultValue(true).Tooltip(
            "Show a small corner readout (state, FOV, roll, move speed) while the cinematic camera is active."));
    AddWidget(path, "Extend Draw Distance", WIDGET_CVAR_CHECKBOX)
        .CVar(CVAR_ENHANCEMENT("CinematicCam.DisableCulling"))
        .Options(CheckboxOptions().DefaultValue(true).Tooltip(
            "While the cinematic camera is active, extend how far actors stay drawn and push out the far clip "
            "plane so objects don't pop in/out.\n\n"
            "This applies ONLY while flying and takes the larger of this and the global 'Increase Actor Draw "
            "Distance' setting (Enhancements tab) - the two never stack/compound. When off, the camera uses "
            "your normal global setting unchanged.\n\n"
            "Drawing more actors costs performance in busy scenes, so keep the multiplier as low as looks good."));
    AddWidget(path, "Extra Draw Distance: %dx", WIDGET_CVAR_SLIDER_INT)
        .CVar(CVAR_ENHANCEMENT("CinematicCam.CullMultiplier"))
        .Options(IntSliderOptions().Min(1).Max(20).DefaultValue(3).Format("%dx").Tooltip(
            "Cinematic-only draw distance multiplier. Raise if objects still pop out; lower if busy scenes lag. "
            "Each step draws noticeably more actors."))
        .PreFunc([](WidgetInfo& info) {
            info.isHidden = !CVarGetInteger(CVAR_ENHANCEMENT("CinematicCam.DisableCulling"), 1);
        });
    AddWidget(path, "Far Clip Plane: %.0f", WIDGET_CVAR_SLIDER_FLOAT)
        .CVar(CVAR_ENHANCEMENT("CinematicCam.FarPlane"))
        .Options(FloatSliderOptions().Min(12800.0f).Max(60000.0f).DefaultValue(20000.0f).Format("%.0f").Tooltip(
            "Distance at which scene geometry is clipped. Raise for wide vistas."))
        .PreFunc([](WidgetInfo& info) {
            info.isHidden = !CVarGetInteger(CVAR_ENHANCEMENT("CinematicCam.DisableCulling"), 1);
        });

    // Controls (rebindable). Movement/look use the left/right sticks (remap those in Controller Config).
    path.column = SECTION_COLUMN_2;
    AddWidget(path, "Controls", WIDGET_SEPARATOR_TEXT);
    AddWidget(path, "Toggle Camera:", WIDGET_CVAR_BTN_SELECTOR)
        .CVar(CVAR_ENHANCEMENT("CinematicCam.ToggleBtn"))
        .Options(BtnSelectorOptions()
                     .DefaultValue(BTN_CUSTOM_MODIFIER1)
                     .Tooltip("Toggles the cinematic camera on/off. Defaults to Modifier 1, because a real "
                              "button would fire during normal gameplay.\n\n"
                              "Modifier 1 needs mapping once before it does anything: Settings > Controller "
                              "Configuration > your port > Modifier Buttons > M1, and set it to your Select / "
                              "Back button."));
    AddWidget(path, "Boost (faster):", WIDGET_CVAR_BTN_SELECTOR)
        .CVar(CVAR_ENHANCEMENT("CinematicCam.BoostBtn"))
        .Options(
            BtnSelectorOptions()
                .DefaultValue(BTN_CUSTOM_MODIFIER2)
                .Tooltip(
                    "Hold to fly faster. Defaults to Modifier 2: a bumper, not a face button, because you hold it "
                    "while both sticks are busy.\n\n"
                    "Modifier 2 needs mapping once before it does anything: Settings > Controller Configuration > your "
                    "port > Modifier Buttons > M2, and set it to your right bumper (RB / R1)."));
    AddWidget(path, "Precision (slower):", WIDGET_CVAR_BTN_SELECTOR)
        .CVar(CVAR_ENHANCEMENT("CinematicCam.PrecisionBtn"))
        .Options(BtnSelectorOptions().DefaultValue(BTN_L));
    AddWidget(path, "Ascend:", WIDGET_CVAR_BTN_SELECTOR)
        .CVar(CVAR_ENHANCEMENT("CinematicCam.UpBtn"))
        .Options(BtnSelectorOptions().DefaultValue(BTN_R));
    AddWidget(path, "Descend:", WIDGET_CVAR_BTN_SELECTOR)
        .CVar(CVAR_ENHANCEMENT("CinematicCam.DownBtn"))
        .Options(BtnSelectorOptions().DefaultValue(BTN_Z));
    AddWidget(path, "FOV in:", WIDGET_CVAR_BTN_SELECTOR)
        .CVar(CVAR_ENHANCEMENT("CinematicCam.FovInBtn"))
        .Options(BtnSelectorOptions().DefaultValue(BTN_DUP));
    AddWidget(path, "FOV out:", WIDGET_CVAR_BTN_SELECTOR)
        .CVar(CVAR_ENHANCEMENT("CinematicCam.FovOutBtn"))
        .Options(BtnSelectorOptions().DefaultValue(BTN_DDOWN));
    AddWidget(path, "Roll left:", WIDGET_CVAR_BTN_SELECTOR)
        .CVar(CVAR_ENHANCEMENT("CinematicCam.RollLeftBtn"))
        .Options(BtnSelectorOptions().DefaultValue(BTN_DLEFT));
    AddWidget(path, "Roll right:", WIDGET_CVAR_BTN_SELECTOR)
        .CVar(CVAR_ENHANCEMENT("CinematicCam.RollRightBtn"))
        .Options(BtnSelectorOptions().DefaultValue(BTN_DRIGHT));
}

} // namespace SohGui
