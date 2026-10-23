import QtQuick 2.9
import QtQuick.Controls
import "."
import ".."
import "../theme"

import StreamingPreferences 1.0
import PyrowaveStatus 1.0

// Shown under Basic → Video codec while PyroWave is selected. It reports host
// and GPU status, explains which settings PyroWave locks, and offers a higher
// bitrate without ever changing it on its own.
Column {
    id: panel

    // Not in Theme: an amber for "works, but not here yet" states.
    readonly property color warnColor: "#FFC861"
    readonly property bool lowBitrate:
        StreamingPreferences.bitrateKbps < PyrowaveStatus.lowBitrateThresholdKbps

    width: parent ? parent.width : 0
    spacing: Theme.spaceSm
    topPadding: Theme.spaceXs
    bottomPadding: Theme.spaceSm

    // Refresh whenever the panel appears: a stream may have updated the host
    // status, and the GPU check runs once per app session.
    function refreshStatus() {
        PyrowaveStatus.refreshHostStatus()
        if (PyrowaveStatus.gpuCheckState === PyrowaveStatus.GpuCheckNotRun) {
            PyrowaveStatus.runCapabilityCheck()
        }
    }

    onVisibleChanged: {
        if (visible) {
            refreshStatus()
        }
    }

    Component.onCompleted: {
        if (visible) {
            refreshStatus()
        }
    }

    function hostChipText() {
        if (!PyrowaveStatus.hasLastHost) {
            return qsTr("Host support unknown")
        }
        switch (PyrowaveStatus.lastHostOffer) {
        case PyrowaveStatus.HostOfferOffered:
            return qsTr("%1 offers PyroWave").arg(PyrowaveStatus.lastHostName)
        case PyrowaveStatus.HostOfferNotOffered:
            return qsTr("Not offered by %1").arg(PyrowaveStatus.lastHostName)
        default:
            return qsTr("%1: host support unknown").arg(PyrowaveStatus.lastHostName)
        }
    }

    function hostChipColor() {
        if (PyrowaveStatus.hasLastHost) {
            if (PyrowaveStatus.lastHostOffer === PyrowaveStatus.HostOfferOffered) {
                return Theme.acid
            }
            if (PyrowaveStatus.lastHostOffer === PyrowaveStatus.HostOfferNotOffered) {
                return panel.warnColor
            }
        }
        return Theme.textDim
    }

    function hostNote() {
        if (PyrowaveStatus.hasLastHost &&
                PyrowaveStatus.lastHostOffer === PyrowaveStatus.HostOfferNotOffered) {
            return qsTr("On its last connection (%1), %2 didn't offer PyroWave. The host needs Foundation Sunshine with PyroWave enabled; until then, streams use H.264.")
                    .arg(PyrowaveStatus.lastHostSeen).arg(PyrowaveStatus.lastHostName)
        }
        if (PyrowaveStatus.hasLastHost &&
                PyrowaveStatus.lastHostOffer === PyrowaveStatus.HostOfferOffered) {
            return qsTr("%1 offered PyroWave on its last connection (%2).")
                    .arg(PyrowaveStatus.lastHostName).arg(PyrowaveStatus.lastHostSeen)
        }
        return qsTr("Host support is checked when a stream starts. If the host doesn't offer PyroWave, the stream uses H.264 and says so.")
    }

    function gpuChipText() {
        switch (PyrowaveStatus.gpuCheckState) {
        case PyrowaveStatus.GpuCheckRunning:
            return qsTr("Checking GPU...")
        case PyrowaveStatus.GpuCheckPassed:
            return qsTr("GPU check passed")
        case PyrowaveStatus.GpuCheckFailed:
            return qsTr("This PC can't decode PyroWave")
        case PyrowaveStatus.GpuCheckUnavailable:
            return qsTr("GPU check unavailable")
        default:
            return qsTr("GPU not checked yet")
        }
    }

    function gpuChipColor() {
        switch (PyrowaveStatus.gpuCheckState) {
        case PyrowaveStatus.GpuCheckPassed:
            return Theme.acid
        case PyrowaveStatus.GpuCheckFailed:
            return Theme.danger
        case PyrowaveStatus.GpuCheckUnavailable:
            return panel.warnColor
        default:
            return Theme.textDim
        }
    }

    component StatusChip: Rectangle {
        property alias text: chipLabel.text
        property color tone: Theme.textDim

        implicitWidth: chipLabel.implicitWidth + Theme.spaceMd * 2
        implicitHeight: chipLabel.implicitHeight + Theme.spaceXs * 2
        radius: 0
        color: "transparent"
        border.width: 1
        border.color: Qt.rgba(tone.r, tone.g, tone.b, 0.55)

        Text {
            id: chipLabel
            anchors.centerIn: parent
            color: parent.tone
            font.family: Theme.fontMono
            font.pointSize: Theme.fontCaption
        }
    }

    // Inline components can't see this file's ids, so they size from parent.
    component Note: Text {
        width: parent ? parent.width - Theme.spaceMd * 2 : 0
        x: Theme.spaceMd
        color: Theme.textSettingsSubtitle
        font.family: Theme.fontSans
        font.pointSize: Theme.fontSettingsSubtitle
        wrapMode: Text.Wrap
    }

    component LockedSetting: Column {
        property string name
        property string reason

        width: parent ? parent.width - Theme.spaceMd * 2 : 0
        x: Theme.spaceMd
        spacing: 2

        Text {
            width: parent.width
            text: parent.name
            color: Theme.text
            font.family: Theme.fontSans
            font.pointSize: Theme.fontRowTitle
            font.weight: Font.DemiBold
            wrapMode: Text.Wrap
        }

        Text {
            width: parent.width
            text: parent.reason
            color: Theme.textDim
            font.family: Theme.fontSans
            font.pointSize: Theme.fontSettingsSubtitle
            wrapMode: Text.Wrap
        }
    }

    Flow {
        width: panel.width - Theme.spaceMd * 2
        x: Theme.spaceMd
        spacing: Theme.spaceSm

        StatusChip {
            text: panel.hostChipText()
            tone: panel.hostChipColor()
        }

        StatusChip {
            text: panel.gpuChipText()
            tone: panel.gpuChipColor()
        }

        StatusChip {
            visible: !PyrowaveStatus.decoderImplemented
            text: qsTr("Decoder not in this build yet")
            tone: Theme.danger
        }
    }

    Note {
        text: qsTr("PyroWave is an experimental, lightly compressed codec for wired LAN. It needs much more bandwidth than other codecs (200 Mbps or more); over Wi-Fi or the internet, expect dropped frames.")
    }

    Note {
        text: panel.hostNote()
    }

    Note {
        visible: PyrowaveStatus.gpuCheckState === PyrowaveStatus.GpuCheckFailed ||
                 PyrowaveStatus.gpuCheckState === PyrowaveStatus.GpuCheckUnavailable
        text: PyrowaveStatus.gpuCheckSummary
        color: PyrowaveStatus.gpuCheckState === PyrowaveStatus.GpuCheckFailed ? Theme.danger
                                                                              : panel.warnColor
    }

    Note {
        visible: !PyrowaveStatus.decoderImplemented
        text: qsTr("This build can't decode PyroWave yet, so starting a stream shows an error instead of connecting. Choose another codec to stream.")
        color: Theme.danger
    }

    // Offer, never apply: the user's bitrate only changes on click.
    Row {
        x: Theme.spaceMd
        width: panel.width - Theme.spaceMd * 2
        spacing: Theme.spaceMd
        visible: panel.lowBitrate

        Text {
            width: parent.width - useRecommendedBitrateButton.width - parent.spacing
            anchors.verticalCenter: parent.verticalCenter
            text: qsTr("Your bitrate is %1 Mbps. PyroWave needs far more than other codecs; %2 Mbps is a good start on wired LAN.")
                    .arg(StreamingPreferences.bitrateKbps / 1000)
                    .arg(PyrowaveStatus.recommendedBitrateKbps / 1000)
            color: panel.warnColor
            font.family: Theme.fontSans
            font.pointSize: Theme.fontSettingsSubtitle
            wrapMode: Text.Wrap
        }

        HardButton {
            id: useRecommendedBitrateButton
            anchors.verticalCenter: parent.verticalCenter
            text: qsTr("Use %1 Mbps").arg(PyrowaveStatus.recommendedBitrateKbps / 1000)
            onClicked: {
                StreamingPreferences.autoAdjustBitrate = false
                StreamingPreferences.bitrateKbps = PyrowaveStatus.recommendedBitrateKbps
            }
        }
    }

    // These controls live on other pages. They keep their saved values and
    // apply again when another codec is selected.
    Text {
        x: Theme.spaceMd
        topPadding: Theme.spaceXs
        text: qsTr("Fixed while PyroWave is selected")
        color: Theme.accent
        font.family: Theme.fontSans
        font.pointSize: Theme.fontCaption
        font.weight: Font.ExtraBold
        font.capitalization: Font.AllUppercase
        font.letterSpacing: Theme.tracking(Theme.fontCaption, 0.08)
    }

    LockedSetting {
        name: qsTr("YUV 4:4:4: off")
        reason: qsTr("The PyroWave stream contract covers 4:2:0 only, and its H.264 fallback streams 4:2:0 too. Your saved setting is kept.")
    }

    LockedSetting {
        name: qsTr("Video AI-Enhancement: not used")
        reason: qsTr("It runs inside the D3D11 renderer, which PyroWave doesn't use. Your saved setting is kept and still applies to other codecs, including an H.264 fallback.")
    }

    LockedSetting {
        name: qsTr("Hardware or software decoding: not used")
        reason: qsTr("PyroWave always decodes on the GPU with Vulkan. Your saved setting is kept and still applies to other codecs, including an H.264 fallback.")
    }

    Row {
        x: Theme.spaceMd
        topPadding: Theme.spaceXs
        spacing: Theme.spaceMd

        HardButton {
            id: capabilityCheckButton
            text: qsTr("Run capability check")
            enabled: PyrowaveStatus.gpuCheckState !== PyrowaveStatus.GpuCheckRunning
            onClicked: {
                PyrowaveStatus.runCapabilityCheck()
                capabilityDialog.open()
            }
        }

        Text {
            anchors.verticalCenter: parent.verticalCenter
            width: panel.width - capabilityCheckButton.width - Theme.spaceMd * 3
            text: qsTr("Reads Vulkan version, subgroup operations and size control for each GPU. It doesn't start a stream or change settings.")
            color: Theme.textDim
            font.family: Theme.fontSans
            font.pointSize: Theme.fontCaption
            wrapMode: Text.Wrap
        }
    }

    NavigableDialog {
        id: capabilityDialog
        title: qsTr("PyroWave capability check")
        standardButtons: Dialog.Close

        ScrollView {
            implicitWidth: 560
            implicitHeight: Math.min(420, reportText.implicitHeight)
            clip: true

            TextArea {
                id: reportText
                readOnly: true
                selectByMouse: true
                wrapMode: TextEdit.Wrap
                color: Theme.text
                font.family: Theme.fontMono
                font.pointSize: Theme.fontBody
                background: Item {}
                text: PyrowaveStatus.gpuCheckState === PyrowaveStatus.GpuCheckRunning
                      ? PyrowaveStatus.gpuCheckSummary
                      : PyrowaveStatus.capabilityReport
            }
        }
    }
}
