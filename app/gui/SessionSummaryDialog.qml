import QtQuick 2.0
import QtQuick.Controls
import QtQuick.Layouts 1.2

import SessionStatsSettings 1.0

import "theme"

// 串流正常结束后弹出的会话小结：时长、画面、帧时间与低帧、丢帧、码率、延迟、FEC。
//
// 非模态：它只是一张信息卡，不该挡住用户马上再点一次连接（或自动重连）。
// Esc / 关闭按钮关掉；下一次串流开始时 StreamSegue 也会把它收起来。
// 数据来自 Session::sessionSummary()（SessionSummary::toVariantMap() 的字段）。
NavigableDialog {
    id: dialog

    property var summary: ({})
    property var rows: []

    modal: false
    closePolicy: Popup.CloseOnEscape
    title: qsTr("Session summary")
    standardButtons: Dialog.Close

    function present(newSummary)
    {
        summary = newSummary
        rows = buildRows()
        open()
    }

    function formatDuration(seconds)
    {
        var total = Math.max(0, Math.floor(seconds))
        var h = Math.floor(total / 3600)
        var m = Math.floor(total / 60) % 60
        var s = total % 60
        return h + ":" + (m < 10 ? "0" : "") + m + ":" + (s < 10 ? "0" : "") + s
    }

    function fixed(value, digits)
    {
        return Number(value).toFixed(digits)
    }

    function has(key)
    {
        return summary !== null && summary[key] !== undefined
    }

    // [{ label, value }]，界面和「复制小结」共用同一份
    function buildRows()
    {
        var rows = []
        if (!summary || !summary.valid) {
            return rows
        }

        rows.push({ label: qsTr("Duration"), value: formatDuration(summary.durationSeconds) })
        rows.push({ label: qsTr("Stream"),
                    value: qsTr("%1×%2 at %3 FPS, %4")
                               .arg(summary.width).arg(summary.height)
                               .arg(summary.targetFps).arg(summary.codec) })
        if (summary.frameTimeSamples > 0) {
            rows.push({ label: qsTr("Average FPS"), value: fixed(summary.averageFps, 1) })
            rows.push({ label: qsTr("1% / 0.1% low"),
                        value: qsTr("%1 / %2 FPS")
                                   .arg(fixed(summary.onePercentLowFps, 1))
                                   .arg(fixed(summary.pointOnePercentLowFps, 1)) })
            rows.push({ label: qsTr("Frame time"),
                        value: qsTr("%1 ms average, %2 ms 99th percentile, %3 ms max")
                                   .arg(fixed(summary.frameTimeAverageMs, 2))
                                   .arg(fixed(summary.frameTimeP99Ms, 2))
                                   .arg(fixed(summary.frameTimeMaxMs, 1)) })
        }
        rows.push({ label: qsTr("Network loss"),
                    value: qsTr("%1% (%2 frames)")
                               .arg(fixed(summary.networkLossPercent, 2))
                               .arg(summary.framesNetworkDropped) })
        rows.push({ label: qsTr("Pacer drops"),
                    value: qsTr("%1% (%2 frames)")
                               .arg(fixed(summary.pacerDropPercent, 2))
                               .arg(summary.framesPacerDropped) })
        if (has("averageBitrateMbps")) {
            rows.push({ label: qsTr("Bitrate"),
                        value: qsTr("%1 Mbps average, %2 Mbps peak")
                                   .arg(fixed(summary.averageBitrateMbps, 1))
                                   .arg(fixed(summary.peakBitrateMbps, 1)) })
        }
        if (has("averageLatencyMs")) {
            rows.push({ label: qsTr("Latency"),
                        value: qsTr("%1 ms average").arg(fixed(summary.averageLatencyMs, 1)) })
        }
        if (has("averageRttMs")) {
            rows.push({ label: qsTr("Round trip"),
                        value: qsTr("%1 ms average, %2 ms max, %3 ms jitter")
                                   .arg(fixed(summary.averageRttMs, 1))
                                   .arg(summary.maxRttMs)
                                   .arg(fixed(summary.averageJitterMs, 2)) })
        }
        if (has("fecRecoveredFrames") && has("fecUnrecoverableFrames")) {
            rows.push({ label: qsTr("FEC"),
                        value: qsTr("%1 frames recovered, %2 unrecoverable")
                                   .arg(summary.fecRecoveredFrames)
                                   .arg(summary.fecUnrecoverableFrames) })
        } else {
            rows.push({ label: qsTr("FEC"),
                        value: qsTr("%1 of %2 packets, frame results not reported by the host")
                                   .arg(summary.fecPackets).arg(summary.videoPackets) })
        }
        return rows
    }

    function summaryText()
    {
        var lines = [qsTr("Moonlight session summary") + ": " +
                     qsTr("%1 on %2").arg(summary.app).arg(summary.host)]
        for (var i = 0; i < rows.length; i++) {
            lines.push(rows[i].label + ": " + rows[i].value)
        }
        if (has("csvPath")) {
            lines.push(qsTr("CSV") + ": " + summary.csvPath)
        }
        return lines.join("\n")
    }

    onOpened: copyButton.forceActiveFocus(Qt.TabFocusReason)

    onClosed: {
        copiedLabel.visible = false
    }

    ColumnLayout {
        spacing: Theme.spaceMd

        Text {
            text: dialog.summary && dialog.summary.valid
                  ? qsTr("%1 on %2").arg(dialog.summary.app).arg(dialog.summary.host)
                  : ""
            color: Theme.textDim
            font.family: Theme.fontSans
            font.pointSize: Theme.fontRowTitle
            font.weight: Font.DemiBold
            elide: Text.ElideRight
            Layout.fillWidth: true
            Layout.maximumWidth: 560
        }

        // 左列名称、右列数值，每行一对
        Column {
            spacing: Theme.spaceXs
            Layout.fillWidth: true

            Repeater {
                model: dialog.rows

                delegate: Row {
                    id: summaryRow
                    required property var modelData
                    spacing: Theme.spaceLg

                    Text {
                        width: 130
                        text: summaryRow.modelData.label
                        color: Theme.textDim
                        font.family: Theme.fontSans
                        font.pointSize: Theme.fontBody
                        elide: Text.ElideRight
                    }

                    Text {
                        width: 400
                        text: summaryRow.modelData.value
                        color: Theme.text
                        font.family: Theme.fontMono
                        font.pointSize: Theme.fontBody
                        wrapMode: Text.Wrap
                    }
                }
            }
        }

        Text {
            visible: dialog.has("csvPath")
            text: dialog.has("csvPath") ? qsTr("Recorded to %1").arg(dialog.summary.csvPath) : ""
            color: Theme.textFaint
            font.family: Theme.fontMono
            font.pointSize: Theme.fontCaption
            wrapMode: Text.WrapAnywhere
            Layout.fillWidth: true
            Layout.maximumWidth: 560
        }

        RowLayout {
            spacing: Theme.spaceSm

            HardButton {
                id: copyButton
                text: qsTr("Copy summary")
                Keys.onReturnPressed: clicked()
                Keys.onEnterPressed: clicked()
                Keys.onRightPressed: openFolderButton.forceActiveFocus(Qt.TabFocusReason)
                onClicked: {
                    SessionStatsSettings.copyText(dialog.summaryText())
                    copiedLabel.visible = true
                }
            }

            HardButton {
                id: openFolderButton
                text: qsTr("Open CSV folder")
                Keys.onReturnPressed: clicked()
                Keys.onEnterPressed: clicked()
                Keys.onLeftPressed: copyButton.forceActiveFocus(Qt.TabFocusReason)
                onClicked: SessionStatsSettings.openStatsFolder()
            }

            Text {
                id: copiedLabel
                visible: false
                text: qsTr("Copied")
                color: Theme.accent
                font.family: Theme.fontSans
                font.pointSize: Theme.fontBody
                font.weight: Font.DemiBold
            }
        }
    }
}
