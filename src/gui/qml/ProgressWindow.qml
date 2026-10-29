// SPDX-License-Identifier: GPL-3.0-or-later
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import Lanes

// One download's window (IDM's progress dialog): numbers, the connection map, a live speed
// graph and the connections; turns into the "download complete" view when it finishes.
ApplicationWindow {
    id: win
    property var downloadId: -1
    property var d: App.details(downloadId)
    property var samples: []
    property bool details: true

    readonly property string status: d.exists ? d.state : "removed"
    readonly property bool running: status === "running"
    readonly property bool done: status === "completed"
    readonly property bool broken: status === "failed" || status === "needs_address"

    width: 560
    height: done ? 300 : (details ? 600 : 330)
    minimumWidth: 460
    minimumHeight: 260
    flags: Qt.Dialog
    title: {
        if (!d.exists)
            return qsTr("Removed")
        if (done)
            return qsTr("Download complete")
        const pct = d.progress >= 0 ? Math.floor(d.progress * 100) + "% " : ""
        return pct + d.fileName
    }

    function refresh() {
        d = App.details(downloadId)
        if (done && !App.settings["gui/showComplete"])
            win.close()
    }

    Connections {
        target: App.model
        function onRefreshed() { win.refresh() }
    }
    // Speed samples for the graph: twice a second, the last minute.
    Timer {
        interval: 500
        repeat: true
        running: win.running && win.visible
        onTriggered: {
            const s = win.samples.slice(-119)
            s.push(win.d.speed)
            win.samples = s
        }
    }

    Shortcut { sequence: "Esc"; onActivated: win.close() }

    background: Rectangle { color: palette.window }

    // --- running / paused / failed ---
    ColumnLayout {
        visible: !win.done
        anchors.fill: parent
        anchors.margins: 20
        spacing: 12

        RowLayout {
            spacing: 12
            Layout.fillWidth: true
            Icon {
                glyph: win.broken ? "" : ""
                size: 28
                color: win.broken ? "#c42b1c" : palette.accent
            }
            ColumnLayout {
                spacing: 2
                Layout.fillWidth: true
                Label {
                    text: win.d.fileName || ""
                    font.pixelSize: 16
                    font.weight: Font.DemiBold
                    elide: Text.ElideMiddle
                    Layout.fillWidth: true
                }
                Label {
                    text: win.d.url || ""
                    color: palette.placeholderText
                    font.pixelSize: 11
                    elide: Text.ElideMiddle
                    Layout.fillWidth: true
                }
            }
        }

        ProgressBar {
            Layout.fillWidth: true
            from: 0; to: 1
            value: Math.max(0, win.d.progress || 0)
            indeterminate: win.running && win.d.progress < 0 && width > 0
        }

        GridLayout {
            columns: 4
            columnSpacing: 16
            rowSpacing: 6
            Layout.fillWidth: true
            component Key: Label { color: palette.placeholderText }
            component Value: Label { Layout.fillWidth: true; elide: Text.ElideRight }

            Key { text: qsTr("Status") }
            Value {
                text: {
                    switch (win.status) {
                    case "running": return qsTr("Downloading")
                    case "queued": return win.d.queue ? qsTr("Waiting in queue “%1”").arg(win.d.queue) : qsTr("Starting…")
                    case "scheduled": return qsTr("Scheduled")
                    case "paused": return qsTr("Paused")
                    case "failed": return qsTr("Failed")
                    case "needs_address": return qsTr("Link expired")
                    case "removed": return qsTr("Removed from the list")
                    }
                    return win.status
                }
                color: win.broken ? "#c42b1c" : palette.text
            }
            Key { text: qsTr("File size") }
            Value { text: win.d.size >= 0 ? App.formatBytes(win.d.size) : qsTr("Unknown") }

            Key { text: qsTr("Downloaded") }
            Value {
                text: App.formatBytes(win.d.downloaded || 0)
                      + (win.d.progress >= 0 ? "  (" + (win.d.progress * 100).toFixed(1) + "%)" : "")
            }
            Key { text: qsTr("Speed") }
            Value { text: win.running ? App.formatSpeed(win.d.speed) : "—" }

            Key { text: qsTr("Time left") }
            Value { text: win.d.timeLeft >= 0 ? App.formatDuration(win.d.timeLeft) : "—" }
            Key { text: qsTr("Resume") }
            Value { text: win.d.resumable ? qsTr("Supported") : (win.running ? qsTr("Not supported") : "—") }
        }

        Label {
            visible: win.broken && (win.d.error || "").length > 0
            text: win.d.error || ""
            color: "#c42b1c"
            wrapMode: Text.Wrap
            Layout.fillWidth: true
        }
        RowLayout {
            visible: win.status === "needs_address"
            Layout.fillWidth: true
            TextField {
                id: newUrl
                placeholderText: qsTr("New link to the same file")
                Layout.fillWidth: true
            }
            Button {
                text: qsTr("Continue")
                enabled: App.isUrl(newUrl.text.trim())
                onClicked: {
                    const e = App.resume(win.downloadId, newUrl.text.trim())
                    if (e.length > 0)
                        win.d.error = e
                }
            }
        }

        // --- details: connection map, speed graph, connections ---
        ColumnLayout {
            visible: win.details
            spacing: 8
            Layout.fillWidth: true
            Layout.fillHeight: true

            Label {
                text: qsTr("Connections")
                font.weight: Font.DemiBold
                Layout.topMargin: 4
            }
            SegmentBar {
                Layout.fillWidth: true
                Layout.preferredHeight: 18
                segments: win.d.segments || []
                total: win.d.size || 0
                barColor: palette.accent
                trackColor: Qt.rgba(palette.text.r, palette.text.g, palette.text.b, 0.08)
                borderColor: palette.window
            }

            RowLayout {
                Layout.fillWidth: true
                Label { text: qsTr("Speed"); font.weight: Font.DemiBold }
                Item { Layout.fillWidth: true }
                Label {
                    text: win.samples.length > 0
                          ? qsTr("peak %1").arg(App.formatSpeed(Math.max.apply(null, win.samples))) : ""
                    color: palette.placeholderText
                    font.pixelSize: 11
                }
            }
            Rectangle {
                Layout.fillWidth: true
                Layout.preferredHeight: 84
                radius: 4
                color: Qt.rgba(palette.text.r, palette.text.g, palette.text.b, 0.03)
                border.color: Qt.rgba(palette.text.r, palette.text.g, palette.text.b, 0.08)
                SpeedGraph {
                    anchors.fill: parent
                    anchors.margins: 1
                    samples: win.samples
                    lineColor: palette.accent
                    gridColor: Qt.rgba(palette.text.r, palette.text.g, palette.text.b, 0.07)
                }
            }

            ListView {
                id: connectionList
                Layout.fillWidth: true
                Layout.fillHeight: true
                Layout.minimumHeight: 60
                clip: true
                model: win.d.segments || []
                ScrollBar.vertical: ScrollBar {}
                delegate: RowLayout {
                    required property var modelData
                    required property int index
                    readonly property real length: modelData.end >= 0 ? modelData.end - modelData.start + 1 : -1
                    width: connectionList.width - 12
                    spacing: 12
                    Label { text: "#" + (index + 1); color: palette.placeholderText; Layout.preferredWidth: 28 }
                    ProgressBar {
                        from: 0; to: 1
                        value: length > 0 ? modelData.received / length : 0
                        Layout.preferredWidth: 120
                    }
                    Label {
                        text: length > 0
                              ? qsTr("%1 of %2").arg(App.formatBytes(modelData.received)).arg(App.formatBytes(length))
                              : App.formatBytes(modelData.received)
                        Layout.fillWidth: true
                    }
                    Label {
                        text: length > 0 && modelData.received >= length ? qsTr("done") : ""
                        color: palette.placeholderText
                        font.pixelSize: 11
                    }
                }
            }

            RowLayout {
                Layout.fillWidth: true
                spacing: 8
                CheckBox {
                    id: limitOn
                    text: qsTr("Limit this download to")
                    checked: (win.d.speedLimit || 0) > 0
                    onToggled: App.setSpeedLimit(win.downloadId, checked ? Math.max(1024, App.parseSize(limitValue.text)) : 0)
                }
                TextField {
                    id: limitValue
                    Layout.preferredWidth: 90
                    text: (win.d.speedLimit || 0) > 0 ? App.formatSpeed(win.d.speedLimit).replace("/s", "") : "1 MiB"
                    onEditingFinished: {
                        const bytes = App.parseSize(text)
                        if (limitOn.checked && bytes > 0)
                            App.setSpeedLimit(win.downloadId, bytes)
                    }
                }
                Label { text: "/s"; color: palette.placeholderText }
            }
        }

        Item { visible: !win.details; Layout.fillHeight: true }

        RowLayout {
            Layout.fillWidth: true
            spacing: 8
            Button {
                text: win.details ? qsTr("Hide details") : qsTr("Show details")
                flat: true
                onClicked: win.details = !win.details
            }
            Item { Layout.fillWidth: true }
            Button {
                text: qsTr("Show in folder")
                enabled: win.d.exists === true
                onClicked: App.openFolder(win.downloadId)
            }
            Button {
                visible: win.d.exists === true
                text: win.running || win.status === "queued" ? qsTr("Pause")
                      : win.broken ? qsTr("Retry") : qsTr("Resume")
                highlighted: !win.running
                onClicked: win.running || win.status === "queued" ? App.pause(win.downloadId)
                                                                 : App.resume(win.downloadId)
            }
            Button {
                text: qsTr("Close")
                onClicked: win.close()
            }
        }
    }

    // --- complete ---
    ColumnLayout {
        visible: win.done
        anchors.fill: parent
        anchors.margins: 24
        spacing: 12

        RowLayout {
            spacing: 14
            Icon { glyph: ""; size: 34; color: "#0f7b0f" }
            ColumnLayout {
                spacing: 2
                Layout.fillWidth: true
                Label {
                    text: qsTr("Download complete")
                    font.pixelSize: 18
                    font.weight: Font.DemiBold
                }
                Label {
                    text: App.formatBytes(win.d.size || 0)
                    color: palette.placeholderText
                }
            }
        }
        Label {
            text: win.d.filePath || ""
            wrapMode: Text.WrapAnywhere
            Layout.fillWidth: true
        }
        Item { Layout.fillHeight: true }
        CheckBox {
            text: qsTr("Don't show this when a download finishes")
            checked: false
            onToggled: App.setSetting("gui/showComplete", !checked)
        }
        RowLayout {
            Layout.fillWidth: true
            spacing: 8
            Item { Layout.fillWidth: true }
            Button {
                text: qsTr("Open")
                highlighted: true
                onClicked: { App.openFile(win.downloadId); win.close() }
            }
            Button {
                text: qsTr("Open folder")
                onClicked: { App.openFolder(win.downloadId); win.close() }
            }
            Button {
                text: qsTr("Close")
                onClicked: win.close()
            }
        }
    }
}
