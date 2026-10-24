#pragma once

#include "overlayrasterwindow.h"
#include <QPainter>
#include <QMouseEvent>
#include <QWheelEvent>
#include <QPoint>
#include <QSize>
#include <QFont>
#include <QIcon>
#include <QHash>
#include <QSurfaceFormat>
#include <QElapsedTimer>
#include <QTimer>
#include <QPropertyAnimation>
#include <QVariantAnimation>
#include <functional>
#include <optional>
#include <utility>
#include <vector>

#include "streaming/input/gamepadglyphs.h"
#include "streaming/input/shortcuts.h"
#include "overlaystatspreset.h"
#include "overlaywindowpolicy.h"

/**
 * OverlayMenuPanel - Tabbed, multi-level Qt overlay menu for streaming sessions.
 *
 * Rendered by OS compositor (DWM), completely independent of the
 * D3D11/SDL/EGL video rendering pipeline.
 *
 * Menu structure:
 *   Tab bar:     Quick | Display | Input | Host | Session. Each top-level entry
 *                is filed into a tab by tabForAction() (one table, keyed by
 *                MenuAction). Tabs without entries are hidden, and the last
 *                tab is remembered for the panel's lifetime (one session).
 *   Tab roots:   one MenuLevel per visible tab, holding that tab's entries.
 *   Sub-levels:  Bitrate (scrubber + presets), Menu Position, USB Devices
 *                (when enabled), Performance Overlay, Send Keys, Host Commands
 *                (Foundation Sunshine only), Stream Quality (resolution / FPS
 *                presets). A sub-level records the action of the entry that
 *                opens it (MenuLevel::openAction) and is looked up by it; no
 *                level has a fixed index.
 *
 * Navigation: mouse (tabs, rows, back/close), gamepad (LB/RB tabs, D-pad
 * move, A select, B back/close) and keyboard (handleNavigationKey()).
 * Sub-level navigation uses a title bar with back button (◂ Title); back
 * returns to the tab the sub-level was opened from.
 * Square industrial theme matching Theme.qml, with hard shadows and brand accents.
 */
class OverlayMenuPanel : public OverlayRasterWindow
{
    Q_OBJECT
public:
    enum class MenuAction
    {
        // Quick actions (keyboard shortcuts)
        Quit,
        QuitAndExit,
        ToggleFullScreen,
        ToggleStatsOverlay,
        ToggleMouseMode,
        ToggleCursorHide,
        ToggleMinimize,
        UngrabInput,
        PasteText,
        TogglePointerRegionLock,
        ShowHostFiles,
        SelectRemoteUsbDevice,
        ReleaseRemoteUsbDevice,
        // Microphone
        ToggleMicrophone,
        // Gamepad mouse emulation
        ToggleGamepadMouse,
        // Set bitrate to the kbps value carried in MenuItem::payload.
        // Handled inside the panel (slider row + presets); never dispatched.
        SetBitrate,
        SetMenuPlacementTop,
        SetMenuPlacementRight,
        SetMenuPlacementLeft,
        SetMenuPlacementButton,
        SetMenuPlacementDisabled,
        // Performance overlay (OverlayStatsController). OpenStatsOverlay marks
        // the submenu entry; it is never dispatched.
        OpenStatsOverlay,
        SetStatsPresetOff,
        SetStatsPresetMinimal,
        SetStatsPresetCompact,
        SetStatsPresetDetailed,
        SetStatsPresetCustom,
        EditStatsOverlayPosition,
        // Host actions. The Open* values tag the SubMenu rows of their levels;
        // the others carry their target in MenuItem::payload and are delivered
        // through the payload action callback.
        OpenSendKeys,
        OpenHostCommands,
        SendHostKeys,   // payload: HostKeys payload ("special:<id>" or "keys:<combo>")
        RunHostCommand, // payload: Foundation Sunshine super command id
                        // Tags of the top-level SubMenu entries for the fixed sub-levels.
                        // Never dispatched.
        OpenBitrate,
        OpenMenuPosition,
        OpenRemoteUsbDevices,
        // Stream quality. OpenStreamQuality tags the level's SubMenu row; the
        // Select* rows carry "WxH" / FPS in MenuItem::payload and only change
        // the pending choice. Apply/Save go out through the quality callback.
        OpenStreamQuality,
        SelectStreamResolution,
        SelectStreamFps,
        ApplyStreamQuality,
        SaveStreamQualityAsDefault,
#ifdef MOONLIGHT_ENABLE_FUNCTION_TESTS
        OpenStylusReplayPanel,
#endif
        MenuActionMax
    };

    // Tabs of the top level, in display order. Count is not a tab.
    enum class MenuTab
    {
        Quick,
        Display,
        Input,
        Host,
        Session,
        Count
    };

    // Tab a top-level entry belongs to (see the table in the .cpp).
    static MenuTab tabForAction(MenuAction action);
    static QString tabTitle(MenuTab tab);

    enum class FileMappingState {
        Unknown,
        Checking,
        Unavailable,
        Available,
        Mounting,
        Open,
        Error
    };

    enum class RemoteUsbState {
        Unavailable,
        Discovering,
        Available,
        Opening,
        Open,
        Stopping,
        Error
    };

    struct RemoteUsbDevice {
        QString id;
        QString label;
        QString detail;
        bool supported = true;
    };

    // A row in the "Send Keys" level
    struct SendKeyItem
    {
        QString label;
        QString detail;  // key combination text
        QString payload; // passed back with MenuAction::SendHostKeys
    };

    // A Foundation Sunshine super command of the running app
    struct HostCommandItem
    {
        QString id;
        QString name;
    };

    enum class MenuItemType {
        Action,     // dispatch action + close menu
        SubMenu,    // navigate to sub-level
        Toggle,     // dispatch action, toggle visual state, keep menu open
        Back,       // navigate back to top level
        Slider,     // in-row value scrubber (drag/wheel/gamepad); keeps menu open
    };

    using ActionCallback = std::function<void(MenuAction)>;
    using CloseCallback  = std::function<void()>;
    using RemoteUsbDeviceCallback = std::function<void(const QString&)>;
    using RemoteUsbReleaseCallback = std::function<void()>;
    // Fired when a bitrate adjustment settles (debounced while scrubbing,
    // immediate for preset taps). The value is in kbps.
    using BitrateChangeCallback = std::function<void(int)>;
    // Fired for actions that carry a payload (SendHostKeys, RunHostCommand).
    // The menu closes first, like for any other action.
    using PayloadActionCallback = std::function<void(MenuAction, const QString&)>;

    // What the stream runs at, and the client display, for the Stream
    // Quality level's presets.
    struct StreamQualityState
    {
        QSize resolution;
        int fps = 0;
        QSize nativeResolution;
        int displayHz = 0;
    };
    // A requested resolution/FPS. saveAsDefault also stores it in the
    // streaming preferences; otherwise it lasts for this session only.
    struct StreamQualityRequest
    {
        QSize resolution;
        int fps = 0;
        bool saveAsDefault = false;
    };
    using StreamQualityCallback = std::function<void(const StreamQualityRequest&)>;

    explicit OverlayMenuPanel(QWindow* parent = nullptr,
                              OverlayWindowMode windowMode = OverlayWindowMode::Detached);
    ~OverlayMenuPanel() override;

    void setActionCallback(ActionCallback cb) { m_ActionCallback = cb; }
    void setCloseCallback(CloseCallback cb)   { m_CloseCallback = cb; }
    void setRemoteUsbDeviceCallback(RemoteUsbDeviceCallback cb) {
        m_RemoteUsbDeviceCallback = std::move(cb);
    }
    void setRemoteUsbReleaseCallback(RemoteUsbReleaseCallback cb) {
        m_RemoteUsbReleaseCallback = std::move(cb);
    }
    void setBitrateChangeCallback(BitrateChangeCallback cb) {
        m_BitrateChangeCallback = std::move(cb);
    }
    void setPayloadActionCallback(PayloadActionCallback cb)
    {
        m_PayloadActionCallback = std::move(cb);
    }
    void setStreamQualityCallback(StreamQualityCallback cb)
    {
        m_StreamQualityCallback = std::move(cb);
    }

    // Human-readable bitrate label shared with toast/log formatting.
    static QString formatBitrateKbps(int kbps);

    // Shortcut action (see ShortcutActions) behind a quick action, or empty.
    // The session triggers these through the input handler's dispatcher.
    static QString shortcutActionId(MenuAction action);

    // Position the panel at the right edge of the given Qt logical parent rect.
    void showAtRightEdge(int parentX, int parentY, int parentW, int parentH,
                         std::optional<QPoint> pointerGlobalPosition = std::nullopt,
                         bool closeWhenPointerOutside = true);

    // Position the panel at the left edge of the given Qt logical parent rect.
    void showAtLeftEdge(int parentX, int parentY, int parentW, int parentH,
                        std::optional<QPoint> pointerGlobalPosition = std::nullopt,
                        bool closeWhenPointerOutside = true);

    // Position the panel at the top edge of the given Qt logical parent rect.
    void showAtTopEdge(int parentX, int parentY, int parentW, int parentH,
                       std::optional<QPoint> pointerGlobalPosition = std::nullopt,
                       bool closeWhenPointerOutside = true);

    // Position the panel at a specific point in the parent rect's coordinate space.
    void showAtCursor(int parentX, int parentY, int parentW, int parentH,
                      const QPoint& cursorPosition, bool pointerTriggered = true);

    void closeMenu();
    void dismissOnOutsideClick(const QPoint& parentPosition);
    void dismissOnOutsidePointerMove(const QPoint& parentPosition);
    bool isMenuVisible() const { return m_Visible; }
    bool isClosing() const { return m_Closing; }
    bool needsEventProcessing() const { return m_Visible || m_Closing; }

    // Update dynamic state before showing the menu
    void updateMicrophoneState(bool enabled);
    void updateBitrateState(int bitrateKbps);
    void updateMenuPositionState(MenuAction activePlacementAction);
    // Marks the active performance overlay preset. canEditPosition is false
    // where the overlay falls back to the in-renderer text.
    void updateStatsOverlayState(StatsOverlay::Preset preset, bool canEditPosition);

    // Overlay preset behind a SetStatsPreset* action
    static std::optional<StatsOverlay::Preset> statsPresetForAction(MenuAction action);
    // Resets the pending Stream Quality choice to what the stream runs at.
    void updateStreamQualityState(const StreamQualityState& state);
    void updateGamepadMouseState(bool enabled);
    void updateFileMappingState(FileMappingState state, const QString& detail);
    void updateRemoteUsbState(bool available,
                              RemoteUsbState state,
                              std::vector<RemoteUsbDevice> devices,
                              const QString& activeDeviceId,
                              const QString& detail);
    // "Send Keys" level: built-in special keys, then the user's key macros
    void updateSendKeys(std::vector<SendKeyItem> specialKeys, std::vector<SendKeyItem> macros);
    // "Host Commands" level, shown only when the host supports super commands
    // (Foundation Sunshine). An empty list shows a setup hint instead.
    void updateHostCommands(bool supported, std::vector<HostCommandItem> commands);
    // Opens the Host Commands level of the visible menu. Returns false when
    // the menu is hidden or the host has no super command support.
    bool showHostCommandsLevel();
    // Opens the Send Keys level of the visible menu. Returns false when the
    // menu is hidden.
    bool showSendKeysLevel();
    // 手柄在位状态、UI 风格、swapFaceButtons 与退出组合键 glyph 文案：菜单
    // 打开前由 Session 推送，任一变化即重建菜单级（手柄专属条目与按键提示
    // 跟着变）
    void setGamepadHints(bool has, GamepadUiStyle style, bool swapFaceButtons,
                         const QString& quitComboGlyphs)
    {
        if (m_HasGamepads != has || m_GamepadUiStyle != style ||
            m_SwapFaceButtons != swapFaceButtons || m_QuitComboGlyphs != quitComboGlyphs) {
            m_HasGamepads = has;
            m_GamepadUiStyle = style;
            m_SwapFaceButtons = swapFaceButtons;
            m_QuitComboGlyphs = quitComboGlyphs;
            buildMenuLevels();
        }
    }

    // Visible tabs in display order, and the one shown (or that a sub-level
    // was opened from).
    std::vector<MenuTab> visibleTabs() const;
    MenuTab currentTab() const;
    bool isAtTabRoot() const { return isTabRootLevel(m_CurrentLevel); }
    // Shows the root of a tab of the visible menu. Returns false when the
    // menu is hidden or the tab has no entries.
    bool showTab(MenuTab tab);
    // Opens the sub-level behind a top-level entry (an Open* action) of the
    // visible menu; back returns to the entry's tab. Returns false when the
    // menu is hidden or the level doesn't exist.
    bool showLevel(MenuAction openAction);
    // Layout queries, in window coordinates, so callers and tests can find
    // rows and tabs by identity instead of by pixel offsets.
    // Row of the shown level with this action (and payload, when given), or -1.
    int rowOfAction(MenuAction action, const QString& payload = QString()) const;
    QRect rowGeometry(int row) const;
    QRect tabGeometry(MenuTab tab) const; // empty when the tab is hidden

    // Gamepad navigation
    void gamepadMoveUp();
    void gamepadMoveDown();
    void gamepadSelect();
    void gamepadBack();
    // Step the focused slider row (DPAD left/right); direction is -1 or +1.
    // Rapid consecutive presses accelerate the step size.
    void gamepadAdjustSlider(int direction);
    // Previous (-1) or next (+1) visible tab, wrapping (LB/RB)
    void gamepadSwitchTab(int direction);
    // D-pad left/right: drives the level's slider row if it has one, else
    // switches tabs on a tab root.
    void gamepadMoveHorizontal(int direction);
    // Keyboard navigation with Qt key codes: arrows move (left/right like
    // the D-pad), Tab/Shift+Tab and PageUp/PageDown switch tabs,
    // Enter/Space select, Backspace/Escape go back. Returns false for keys
    // that are not menu keys, or when the menu is hidden.
    bool handleNavigationKey(int key, Qt::KeyboardModifiers modifiers = Qt::NoModifier);

protected:
    void paintOverlay(QPainter& painter) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void wheelEvent(QWheelEvent* event) override;
    bool event(QEvent* event) override;

private:
    struct MenuItem {
        QString      label;
        QString      detail;       // shortcut key, status text, or "✓"
        MenuItemType type;
        MenuAction   action;
        int          targetLevel;  // for SubMenu: which level to navigate to
        bool         enabled;
        bool         toggleState;  // for Toggle: current on/off state
        bool         separatorAfter; // draw group separator after this item
        QString      payload;      // opaque action payload; never rendered

        MenuItem(QString label,
                 QString detail,
                 MenuItemType type,
                 MenuAction action,
                 int targetLevel,
                 bool enabled,
                 bool toggleState,
                 bool separatorAfter,
                 QString payload = {})
            : label(std::move(label)),
              detail(std::move(detail)),
              type(type),
              action(action),
              targetLevel(targetLevel),
              enabled(enabled),
              toggleState(toggleState),
              separatorAfter(separatorAfter),
              payload(std::move(payload))
        {
        }
    };

    struct MenuLevel {
        QString              title;
        std::vector<MenuItem> items;
        // Tab roots carry their tab; sub-levels the action of their entry
        std::optional<MenuTab> tab;
        MenuAction openAction = MenuAction::MenuActionMax;
    };

    enum class AnchorMode { RightEdge, LeftEdge, TopEdge, AtCursor };

    // Hit zones inside the slider row (local coordinates).
    enum class SliderZone { None, Minus, Track, Plus };

    struct SliderRowRects {
        QRect value;
        QRect track;
        QRect minus;
        QRect plus;
    };

    void buildMenuLevels();
    void applyStatsOverlayState();
    QString shortcutDetail(MenuAction action) const; // live key binding text
    MenuLevel buildStreamQualityLevel() const;
    bool handleStreamQualityItem(const MenuItem& item); // true when consumed
    void navigateToLevel(int level);
    // Tab roots and sub-levels, looked up by identity
    bool isTabRootLevel(int level) const;
    int tabRootLevel() const; // root of m_CurrentTab, or the first visible tab
    int levelForOpenAction(MenuAction action) const; // -1 when absent
    MenuItem* findRootItem(MenuAction action);
    void showTabLevel(int level, int direction); // direction: slide, 0 for none
    int contentTop() const { return m_TitleHeight + m_TabBarHeight + m_Padding; }
    QRect tabRect(int visibleIndex, int contentWidth) const;
    void repositionWindow();
    void showInternal();     // shared show logic after geometry is set
    void schedulePointerOutsideCheck();
    void beginInteraction();
    void forceRepaint();     // synchronous repaint (requestUpdate is async on Windows)
    int  itemAtPos(const QPoint& pos) const;
    void dispatchActionItem(const MenuItem& item);

    // --- Bitrate slider row ---
    SliderRowRects sliderRowRects(int contentWidth, int itemY) const;
    SliderZone sliderZoneAt(const QPoint& localPos, int rowIdx) const;
    double bitrateFraction() const;                 // m_BitrateKbps on the segmented scale, 0..1
    void setBitrateFromFraction(double fraction);   // inverse of bitrateFraction()
    void setBitrateKbps(int bitrateKbps);           // clamp + refresh + schedule commit
    void adjustBitrateStep(int direction, int multiplier);
    void selectBitratePreset(const MenuItem& item); // snap to preset + commit now
    void commitBitrateNow();                        // flush pending change to callback
    void refreshBitrateDetails();                   // level-0 detail + preset checkmarks

    // Index of the performance overlay level, refreshed on every build
    // from levelForOpenAction().
    int m_StatsOverlayLevel = -1;
    StatsOverlay::Preset m_StatsOverlayPreset = StatsOverlay::Preset::Off;
    bool m_StatsOverlayEditable = true;

    std::vector<MenuLevel> m_MenuLevels;
    // Level indices of the visible tab roots, in display order
    std::vector<int> m_TabLevels;
    // Last chosen tab; survives rebuilds, closes and reopens
    MenuTab m_CurrentTab = MenuTab::Quick;
    int  m_CurrentLevel;
    int  m_HoveredIndex;
    bool m_Visible;
    bool m_HasGamepads;
    GamepadUiStyle m_GamepadUiStyle;
    bool m_SwapFaceButtons;
    QString m_QuitComboGlyphs;
    ShortcutProfile m_ShortcutProfile;
    FileMappingState m_FileMappingState;
    QString m_FileMappingDetail;
    bool m_RemoteUsbAvailable;
    bool m_RemoteUsbNeedsActivation = false;
    RemoteUsbState m_RemoteUsbState;
    std::vector<RemoteUsbDevice> m_RemoteUsbDevices;
    QString m_RemoteUsbActiveDeviceId;
    QString m_RemoteUsbDetail;
    std::vector<SendKeyItem> m_SpecialKeyItems;
    std::vector<SendKeyItem> m_KeyMacroItems;
    bool m_HostCommandsSupported = false;
    std::vector<HostCommandItem> m_HostCommands;
    // Indices of the host action levels in m_MenuLevels, -1 when absent
    int m_SendKeysLevel = -1;
    int m_HostCommandsLevel = -1;

    ActionCallback m_ActionCallback;
    CloseCallback  m_CloseCallback;
    RemoteUsbDeviceCallback m_RemoteUsbDeviceCallback;
    RemoteUsbReleaseCallback m_RemoteUsbReleaseCallback;
    BitrateChangeCallback m_BitrateChangeCallback;
    PayloadActionCallback m_PayloadActionCallback;
    StreamQualityCallback m_StreamQualityCallback;

    // Stream Quality level: running state, the pending choice, and the level
    // index in m_MenuLevels (-1 before the first build).
    StreamQualityState m_StreamQuality;
    QSize m_PendingStreamResolution;
    int m_PendingStreamFps = 0;
    int m_StreamQualityLevel = -1;

    // Bitrate slider state. m_BitrateKbps is the on-screen value;
    // m_CommittedBitrateKbps is the last value sent to the callback.
    int m_BitrateKbps = 10000;
    int m_CommittedBitrateKbps = 10000;
    QTimer m_BitrateCommitTimer;      // debounces callback while scrubbing
    bool m_SliderDragging = false;    // pointer is scrubbing the track
    SliderZone m_SliderPressedZone = SliderZone::None;
    SliderZone m_SliderHotZone = SliderZone::None;
    qreal m_WheelAccum = 0.0;         // high-resolution wheel accumulation
    QElapsedTimer m_SliderAdjustClock; // gamepad repeat acceleration window
    int m_SliderAdjustStreak = 0;

    // Parent rect in the panel's coordinate space. This is global for detached
    // desktop windows and parent-local for native Wayland child surfaces.
    int m_ParentX, m_ParentY, m_ParentW, m_ParentH;
    OverlayWindowMode m_WindowMode;

    // Layout constants (logical units, Qt 6 auto-scales)
    int m_ItemHeight;
    int m_Padding;
    int m_MenuWidth;
    int m_ShadowMargin;
    int m_TitleHeight;
    int m_IconAreaWidth;
    int m_TabBarHeight;

    // Fonts
    QFont m_LabelFont;
    QFont m_DetailFont;
    QFont m_TitleFont;
    QFont m_TabFont;
    QHash<QString, QIcon> m_MenuIcons;

    // Anti-flicker: grace period after show
    QElapsedTimer m_ShowTimer;
    QTimer m_LeaveTimer;
    bool m_CloseWhenPointerOutside;

    // Animations
    QPropertyAnimation* m_OpacityAnim;
    QPropertyAnimation* m_SlideAnim;    // animates x or y based on the anchor
    QVariantAnimation*  m_ContentSlideAnim; // animates content offset for level nav
    qreal  m_ContentOffset;   // horizontal paint offset during level transition
    bool   m_Closing;         // true while close animation is running
    QPoint m_TargetPosition;  // cached final position for show animation

    // Menu anchor mode and pointer position in the parent rect's coordinates.
    AnchorMode m_AnchorMode;
    std::optional<QPoint> m_TriggerPosition;
};
