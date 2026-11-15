pragma Singleton
import QtQml
import StreamingPreferences 1.0

// Test double for the C++ singleton registered in app/main.cpp. Only the
// calls reachable from the settings components are provided.
QtObject {
    property int connectedGamepads: 0
    property bool uiNavMode: false

    function getConnectedGamepads() { return connectedGamepads }
    function gamepadUiNavigationEnabled() { return StreamingPreferences.enableGamepadUiNavigation }
    function setUiNavMode(mode) { uiNavMode = mode }
    function suspendUiNavMode() {}
    function resumeUiNavMode() {}
    function enable() {}
    function disable() {}
    function notifyWindowFocus(focused) {}
    function startButtonName() { return "Start" }
    function selectButtonName() { return "Back" }
    function leftShoulderName() { return "LB" }
    function rightShoulderName() { return "RB" }
    function faceButtonGlyph(button) { return "" }
    function gamepadQuitComboEnabled() { return false }
}
