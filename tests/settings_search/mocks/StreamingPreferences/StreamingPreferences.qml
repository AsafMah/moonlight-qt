pragma Singleton
import QtQuick
QtObject {
    enum BackgroundSource { BGS_NONE, BGS_BUILTIN }
    property int backgroundSource: StreamingPreferences.BGS_NONE
    property int backgroundOverlayOpacity: 50
    property bool clipboardSyncEnabled: true
    property bool clipboardSyncText: true
    property bool clipboardSyncImages: true
    property bool clipboardSyncFiles: true
    property bool clipboardReceiveFiles: true
    property bool clipboardDropFiles: true
    property int bitrateKbps: 100000
    property bool autoAdjustBitrate: false
    function save() {}
}
