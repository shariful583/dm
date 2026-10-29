// SPDX-License-Identifier: GPL-3.0-or-later
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import Lanes

// One download in the main list: icon, name with a status line, then the size, progress, time
// left, speed and date columns (widths shared with the header through `columns`).
ItemDelegate {
    id: row
    required property int index
    required property var downloadId
    required property string fileName
    required property string url
    required property real size
    required property real downloaded
    required property real progress
    required property real speed
    required property real timeLeft
    required property string status
    required property string category
    required property string queue
    required property string error
    required property var added
    required property var startAt

    property var columns
    property bool selected: false
    signal rowPressed(var mouse)
    signal menuRequested(real x, real y)
    signal opened()

    width: ListView.view ? ListView.view.width : 600
    height: 52
    highlighted: selected
    hoverEnabled: true
    padding: 0
    leftPadding: 12
    rightPadding: 12

    readonly property bool finished: status === "completed"
    readonly property bool broken: status === "failed" || status === "needs_address"

    function categoryGlyph() {
        switch (category) {
        case "Compressed": return ""
        case "Documents": return ""
        case "Music": return ""
        case "Programs": return ""
        case "Video": return ""
        default: return ""
        }
    }

    function statusText() {
        const pct = progress >= 0 ? Math.floor(progress * 100) + "%" : ""
        switch (status) {
        case "running":
            if (size > 0)
                return qsTr("%1 of %2").arg(App.formatBytes(downloaded)).arg(App.formatBytes(size))
            return qsTr("%1 downloaded").arg(App.formatBytes(downloaded))
        case "queued":
            return queue.length > 0 ? qsTr("Waiting in queue “%1”").arg(queue) : qsTr("Starting…")
        case "scheduled":
            return qsTr("Starts %1").arg(Qt.formatDateTime(startAt, "ddd d MMM, hh:mm"))
        case "paused":
            return pct.length > 0 ? qsTr("Paused at %1").arg(pct) : qsTr("Paused")
        case "completed":
            return qsTr("Complete")
        case "failed":
            return error.length > 0 ? error : qsTr("Failed")
        case "needs_address":
            return qsTr("Link expired: right-click → Refresh address")
        }
        return status
    }

    background: Rectangle {
        radius: 4
        color: row.selected ? Qt.rgba(palette.highlight.r, palette.highlight.g, palette.highlight.b, 0.22)
             : row.hovered ? Qt.rgba(palette.text.r, palette.text.g, palette.text.b, 0.05)
             : "transparent"
        Rectangle { // selection marker, as in WinUI lists
            visible: row.selected
            width: 3; radius: 1.5
            height: parent.height * 0.5
            anchors.verticalCenter: parent.verticalCenter
            color: palette.accent
        }
    }

    MouseArea {
        anchors.fill: parent
        acceptedButtons: Qt.LeftButton | Qt.RightButton
        onPressed: mouse => {
            row.forceActiveFocus()
            if (mouse.button === Qt.RightButton) {
                if (!row.selected)
                    row.rowPressed(mouse)
                row.menuRequested(mouse.x, mouse.y)
            } else {
                row.rowPressed(mouse)
            }
        }
        onDoubleClicked: row.opened()
    }

    contentItem: RowLayout {
        spacing: 12
        Icon {
            glyph: row.categoryGlyph()
            size: 20
            color: row.broken ? "#c42b1c" : row.finished ? palette.placeholderText : palette.accent
            Layout.preferredWidth: 24
        }
        ColumnLayout {
            spacing: 1
            Layout.fillWidth: true
            Layout.minimumWidth: 120
            Label {
                text: row.fileName.length > 0 ? row.fileName : row.url
                elide: Text.ElideMiddle
                font.weight: row.status === "running" ? Font.DemiBold : Font.Normal
                Layout.fillWidth: true
            }
            Label {
                text: row.statusText()
                elide: Text.ElideRight
                font.pixelSize: 11
                color: row.broken ? "#c42b1c" : palette.placeholderText
                Layout.fillWidth: true
            }
        }
        Label {
            text: row.size >= 0 ? App.formatBytes(row.size) : "—"
            horizontalAlignment: Text.AlignRight
            color: palette.placeholderText
            Layout.preferredWidth: row.columns.size
        }
        Item {
            Layout.preferredWidth: row.columns.progress
            implicitHeight: 20
            ProgressBar {
                id: bar
                visible: !row.finished && (row.progress >= 0 || row.status === "running")
                anchors.left: parent.left
                anchors.right: percent.left
                anchors.rightMargin: 8
                anchors.verticalCenter: parent.verticalCenter
                from: 0; to: 1
                value: Math.max(0, row.progress)
                indeterminate: row.status === "running" && row.progress < 0 && width > 0
                opacity: row.status === "running" ? 1 : 0.55
            }
            Label {
                id: percent
                anchors.right: parent.right
                anchors.verticalCenter: parent.verticalCenter
                width: row.finished ? parent.width : 38
                horizontalAlignment: row.finished ? Text.AlignLeft : Text.AlignRight
                text: row.finished ? "✓ " + qsTr("Done")
                    : row.progress >= 0 ? Math.floor(row.progress * 100) + "%" : ""
                color: row.finished ? "#0f7b0f" : palette.text
                font.pixelSize: 12
            }
        }
        Label {
            text: row.timeLeft >= 0 ? App.formatDuration(row.timeLeft) : ""
            horizontalAlignment: Text.AlignRight
            Layout.preferredWidth: row.columns.timeLeft
        }
        Label {
            text: row.speed > 0 ? App.formatSpeed(row.speed) : ""
            horizontalAlignment: Text.AlignRight
            Layout.preferredWidth: row.columns.speed
        }
        Label {
            text: Qt.formatDateTime(row.added, "d MMM yyyy, hh:mm")
            horizontalAlignment: Text.AlignRight
            color: palette.placeholderText
            font.pixelSize: 12
            Layout.preferredWidth: row.columns.added
        }
    }
}
