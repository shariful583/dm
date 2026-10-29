// SPDX-License-Identifier: GPL-3.0-or-later
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import Lanes

// Toolbar button: icon glyph above a short label, with the full action as tooltip.
ToolButton {
    id: control
    property string glyph
    property string tip
    property color glyphColor: palette.buttonText
    implicitWidth: Math.max(64, content.implicitWidth + 16)
    implicitHeight: 52
    hoverEnabled: true
    ToolTip.visible: hovered && tip.length > 0
    ToolTip.delay: 600
    ToolTip.text: tip
    Accessible.name: text
    contentItem: ColumnLayout {
        id: content
        spacing: 2
        opacity: control.enabled ? 1 : 0.4
        Icon {
            glyph: control.glyph
            size: 18
            color: control.glyphColor
            Layout.alignment: Qt.AlignHCenter
        }
        Label {
            text: control.text
            font.pixelSize: 11
            Layout.alignment: Qt.AlignHCenter
        }
    }
}
