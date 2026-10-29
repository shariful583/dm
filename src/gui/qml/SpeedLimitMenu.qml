// SPDX-License-Identifier: GPL-3.0-or-later
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import Lanes

// Global speed limit: presets plus a custom value ("1.5M", "800K").
Menu {
    id: menu
    readonly property real current: Number(App.settings["speed_limit"])
    readonly property var presets: [0, 262144, 524288, 1048576, 2097152, 5242880, 10485760]

    Repeater {
        model: menu.presets
        MenuItem {
            required property real modelData
            text: modelData === 0 ? qsTr("No limit") : App.formatSpeed(modelData)
            checkable: true
            checked: menu.current === modelData
            onTriggered: App.setSetting("speed_limit", String(modelData))
        }
    }
    MenuSeparator {}
    MenuItem {
        text: menu.presets.indexOf(menu.current) < 0
              ? qsTr("Custom (%1)…").arg(App.formatSpeed(menu.current)) : qsTr("Custom…")
        checkable: true
        checked: menu.presets.indexOf(menu.current) < 0
        onTriggered: custom.open()
    }

    Dialog {
        id: custom
        parent: Overlay.overlay
        anchors.centerIn: parent
        modal: true
        title: qsTr("Speed limit")
        standardButtons: Dialog.Ok | Dialog.Cancel
        onAboutToShow: {
            value.text = menu.current > 0 ? App.formatSpeed(menu.current).replace("/s", "") : ""
            value.forceActiveFocus()
        }
        onAccepted: {
            const bytes = value.text.trim().length === 0 ? 0 : App.parseSize(value.text)
            if (bytes >= 0)
                App.setSetting("speed_limit", String(Math.round(bytes)))
        }
        ColumnLayout {
            spacing: 8
            Label { text: qsTr("Maximum total download speed (e.g. 800K, 2M; empty or 0 = no limit):") }
            TextField {
                id: value
                placeholderText: "2M"
                Layout.fillWidth: true
                validator: RegularExpressionValidator { regularExpression: /^\s*(\d+(\.\d+)?\s*([kKmMgG]([iI]?[bB])?)?)?\s*$/ }
                onAccepted: custom.accept()
            }
        }
    }
}
