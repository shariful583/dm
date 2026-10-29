// SPDX-License-Identifier: GPL-3.0-or-later
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import Lanes

// Queues and scheduler: each queue runs a few downloads at a time and can start and stop by
// itself at set times on chosen days.
ApplicationWindow {
    id: win
    width: 680
    height: 470
    minimumWidth: 560
    minimumHeight: 400
    title: qsTr("Queues and scheduler")
    flags: Qt.Dialog

    property string selected: App.queues.length > 0 ? App.queues[0].name : ""
    readonly property var queue: {
        for (const q of App.queues)
            if (q.name === selected)
                return q
        return null
    }
    property string error

    function save(changes) {
        error = App.saveQueue(Object.assign({ name: selected }, changes))
    }

    Shortcut { sequence: "Esc"; onActivated: win.close() }
    background: Rectangle { color: palette.window }

    RowLayout {
        anchors.fill: parent
        anchors.margins: 16
        spacing: 16

        // Queue list.
        ColumnLayout {
            Layout.preferredWidth: 200
            Layout.fillHeight: true
            spacing: 8
            ListView {
                id: queueList
                Layout.fillWidth: true
                Layout.fillHeight: true
                clip: true
                model: App.queues
                delegate: ItemDelegate {
                    id: item
                    required property var modelData
                    width: queueList.width
                    highlighted: win.selected === modelData.name
                    onClicked: win.selected = modelData.name
                    contentItem: RowLayout {
                        spacing: 10
                        Icon {
                            glyph: item.modelData.enabled ? "" : ""
                            color: item.modelData.enabled ? palette.accent : palette.text
                            size: 14
                        }
                        Label { text: item.modelData.name; Layout.fillWidth: true; elide: Text.ElideRight }
                        Label {
                            readonly property int n: App.model.counts["queue:" + item.modelData.name] || 0
                            text: n
                            visible: n > 0
                            color: palette.placeholderText
                            font.pixelSize: 11
                        }
                    }
                }
            }
            RowLayout {
                Layout.fillWidth: true
                TextField {
                    id: newName
                    placeholderText: qsTr("New queue")
                    Layout.fillWidth: true
                    onAccepted: addButton.clicked()
                }
                ToolButton {
                    id: addButton
                    text: ""
                    font.family: App.iconFont
                    ToolTip.visible: hovered
                    ToolTip.text: qsTr("Add queue")
                    enabled: newName.text.trim().length > 0
                    onClicked: {
                        win.error = App.saveQueue({ name: newName.text.trim() })
                        if (win.error.length === 0) {
                            win.selected = newName.text.trim()
                            newName.text = ""
                        }
                    }
                }
            }
        }

        Rectangle { Layout.fillHeight: true; width: 1; color: Qt.rgba(palette.text.r, palette.text.g, palette.text.b, 0.1) }

        // Selected queue.
        ColumnLayout {
            Layout.fillWidth: true
            Layout.fillHeight: true
            spacing: 12
            visible: win.queue !== null

            RowLayout {
                Layout.fillWidth: true
                Label {
                    text: win.selected
                    font.pixelSize: 18
                    font.weight: Font.DemiBold
                    Layout.fillWidth: true
                    elide: Text.ElideRight
                }
                Label {
                    text: win.queue && win.queue.enabled ? qsTr("Running") : qsTr("Stopped")
                    color: win.queue && win.queue.enabled ? palette.accent : palette.placeholderText
                }
                Button {
                    text: win.queue && win.queue.enabled ? qsTr("Stop") : qsTr("Start now")
                    highlighted: !(win.queue && win.queue.enabled)
                    onClicked: win.error = win.queue.enabled ? App.stopQueue(win.selected) : App.startQueue(win.selected)
                }
            }

            GridLayout {
                columns: 2
                columnSpacing: 16
                rowSpacing: 12
                Layout.fillWidth: true

                Label { text: qsTr("Downloads at the same time") }
                SpinBox {
                    from: 1; to: 64
                    editable: true
                    value: win.queue ? win.queue.maxActive : 3
                    onValueModified: win.save({ maxActive: value })
                }

                CheckBox {
                    id: startOn
                    text: qsTr("Start at")
                    checked: win.queue !== null && win.queue.startTime.length > 0
                    onToggled: win.save({ startTime: checked ? (startTime.text || "02:00") : "" })
                }
                TextField {
                    id: startTime
                    enabled: startOn.checked
                    text: win.queue && win.queue.startTime.length > 0 ? win.queue.startTime : "02:00"
                    inputMask: "99:99"
                    Layout.preferredWidth: 90
                    onEditingFinished: if (startOn.checked) win.save({ startTime: text })
                }

                CheckBox {
                    id: stopOn
                    text: qsTr("Stop at")
                    checked: win.queue !== null && win.queue.stopTime.length > 0
                    onToggled: win.save({ stopTime: checked ? (stopTime.text || "07:00") : "" })
                }
                TextField {
                    id: stopTime
                    enabled: stopOn.checked
                    text: win.queue && win.queue.stopTime.length > 0 ? win.queue.stopTime : "07:00"
                    inputMask: "99:99"
                    Layout.preferredWidth: 90
                    onEditingFinished: if (stopOn.checked) win.save({ stopTime: text })
                }

                Label { text: qsTr("On these days"); Layout.alignment: Qt.AlignTop; Layout.topMargin: 6 }
                Flow {
                    Layout.fillWidth: true
                    spacing: 4
                    enabled: startOn.checked || stopOn.checked
                    Repeater {
                        // Bit 0 = Monday ... bit 6 = Sunday (core/queue/schedule.hpp).
                        model: [qsTr("Mon"), qsTr("Tue"), qsTr("Wed"), qsTr("Thu"), qsTr("Fri"), qsTr("Sat"), qsTr("Sun")]
                        Button {
                            required property string modelData
                            required property int index
                            text: modelData
                            checkable: true
                            checked: win.queue !== null && (win.queue.days & (1 << index)) !== 0
                            implicitWidth: 62
                            onToggled: win.save({ days: checked ? (win.queue.days | (1 << index))
                                                                : (win.queue.days & ~(1 << index)) })
                        }
                    }
                }
            }

            Label {
                text: qsTr("Add downloads to a queue with “Download later” in the Add window. A running queue starts its downloads in the order they were added.")
                wrapMode: Text.Wrap
                color: palette.placeholderText
                Layout.fillWidth: true
            }
            Label {
                visible: win.error.length > 0
                text: win.error
                color: "#c42b1c"
                wrapMode: Text.Wrap
                Layout.fillWidth: true
            }
            Item { Layout.fillHeight: true }
        }

        Label {
            visible: win.queue === null
            text: qsTr("No queues yet. Create one on the left.")
            color: palette.placeholderText
            Layout.fillWidth: true
            horizontalAlignment: Text.AlignHCenter
        }
    }
}
