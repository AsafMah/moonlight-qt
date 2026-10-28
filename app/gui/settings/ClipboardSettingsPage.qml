import QtQuick 2.9
import QtQuick.Controls
import "."
import "../theme"
import StreamingPreferences 1.0

// Clipboard sync with the host. Settings are read when a stream starts.
Column {
    id: clipboardPage

    width: parent ? parent.width : 0
    spacing: Theme.spaceLg

    SettingsCard {
        title: qsTr("Clipboard")
        subtitle: qsTr("Share what you copy between this device and the host while streaming. Requires a host with clipboard sync enabled. Changes apply to the next stream.")

        ToggleRow {
            id: masterToggle
            title: qsTr("Sync clipboard with host")
            description: qsTr("When off, nothing is copied to or from the host clipboard.")
            checked: StreamingPreferences.clipboardSyncEnabled
            onToggled: function(value) { StreamingPreferences.clipboardSyncEnabled = value }
        }

        ToggleRow {
            title: qsTr("Text")
            description: qsTr("Sync copied text in both directions.")
            checked: StreamingPreferences.clipboardSyncText
            controlEnabled: masterToggle.checked
            onToggled: function(value) { StreamingPreferences.clipboardSyncText = value }
        }

        ToggleRow {
            title: qsTr("Images")
            description: qsTr("Sync copied images and screenshots in both directions.")
            checked: StreamingPreferences.clipboardSyncImages
            controlEnabled: masterToggle.checked
            onToggled: function(value) { StreamingPreferences.clipboardSyncImages = value }
        }
    }
}
