// SPDX-License-Identifier: GPL-3.0-or-later
import QtQuick
import Lanes

// A glyph of the Windows icon font (Segoe Fluent Icons; Segoe MDL2 Assets on Windows 10).
Text {
    property string glyph
    property real size: 16
    text: glyph
    font.family: App.iconFont
    font.pixelSize: size
    color: palette.text
    horizontalAlignment: Text.AlignHCenter
    verticalAlignment: Text.AlignVCenter
    Accessible.ignored: true
}
