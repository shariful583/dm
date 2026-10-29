// SPDX-License-Identifier: GPL-3.0-or-later
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

// In-window question or message. `choices`: [{key, text, accent}]; chosen(key) when one is
// clicked ("" when dismissed).
Dialog {
    id: dialog
    property string text
    property var choices: [{ key: "ok", text: qsTr("OK"), accent: true }]
    signal chosen(string key)
    property bool answered: false

    modal: true
    anchors.centerIn: Overlay.overlay
    width: Math.min(460, (parent ? parent.width : 460) - 40)
    closePolicy: Popup.CloseOnEscape

    onAboutToShow: answered = false
    onClosed: if (!answered) chosen("")

    contentItem: Label {
        text: dialog.text
        wrapMode: Text.Wrap
    }
    footer: DialogButtonBox {
        alignment: Qt.AlignRight
        Repeater {
            model: dialog.choices
            Button {
                required property var modelData
                text: modelData.text
                highlighted: modelData.accent === true
                DialogButtonBox.buttonRole: DialogButtonBox.ActionRole
                onClicked: {
                    dialog.answered = true
                    dialog.close()
                    dialog.chosen(modelData.key)
                }
            }
        }
    }
}
