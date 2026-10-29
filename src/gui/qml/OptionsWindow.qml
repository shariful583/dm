// SPDX-License-Identifier: GPL-3.0-or-later
import QtQuick
import QtQuick.Controls
import QtQuick.Dialogs
import QtQuick.Layouts
import Lanes

// Options: general behaviour, connections and limits, file categories, site logins, about.
// Changes apply at once.
ApplicationWindow {
    id: win
    width: 700
    height: 560
    minimumWidth: 560
    minimumHeight: 440
    title: qsTr("Options")
    flags: Qt.Dialog

    property string error

    function apply(key, value) {
        error = App.setSetting(key, value)
    }
    function folderPath(url) {
        return decodeURIComponent(url.toString().replace(/^file:\/{3}/, "")).replace(/\//g, "\\")
    }

    Shortcut { sequence: "Esc"; onActivated: win.close() }
    background: Rectangle { color: palette.window }

    header: TabBar {
        id: tabs
        TabButton { text: qsTr("General") }
        TabButton { text: qsTr("Connection") }
        TabButton { text: qsTr("Categories") }
        TabButton { text: qsTr("Site logins") }
        TabButton { text: qsTr("About") }
    }

    footer: Label {
        visible: win.error.length > 0
        text: win.error
        color: "#c42b1c"
        padding: 12
        wrapMode: Text.Wrap
    }

    StackLayout {
        anchors.fill: parent
        anchors.margins: 20
        currentIndex: tabs.currentIndex

        // --- General ---
        ColumnLayout {
            spacing: 10
            Label { text: qsTr("Download folder"); font.weight: Font.DemiBold }
            RowLayout {
                Layout.fillWidth: true
                TextField {
                    id: downloadDir
                    text: App.settings["download_dir"] || ""
                    Layout.fillWidth: true
                    onEditingFinished: if (text !== App.settings["download_dir"]) win.apply("download_dir", text)
                }
                Button { text: qsTr("Browse…"); onClicked: dirDialog.open() }
            }
            CheckBox {
                text: qsTr("Sort files into folders by type (Video, Music, Documents, …)")
                checked: App.settings["category_folders"] === "1"
                onToggled: win.apply("category_folders", checked ? "1" : "0")
            }
            Label { text: qsTr("Windows"); font.weight: Font.DemiBold; Layout.topMargin: 12 }
            CheckBox {
                text: qsTr("Show the progress window when a download starts")
                checked: App.settings["gui/showProgress"] === true
                onToggled: win.apply("gui/showProgress", checked)
            }
            CheckBox {
                text: qsTr("Show “Download complete” in the progress window")
                checked: App.settings["gui/showComplete"] === true
                onToggled: win.apply("gui/showComplete", checked)
            }
            CheckBox {
                text: qsTr("Notify when downloads finish or fail")
                checked: App.settings["gui/notify"] === true
                onToggled: win.apply("gui/notify", checked)
            }
            CheckBox {
                text: qsTr("Closing the window keeps Lanes running in the notification area")
                checked: App.settings["gui/minimizeToTray"] === true
                onToggled: win.apply("gui/minimizeToTray", checked)
            }
            Item { Layout.fillHeight: true }

            FolderDialog {
                id: dirDialog
                title: qsTr("Download folder")
                onAccepted: win.apply("download_dir", win.folderPath(selectedFolder))
            }
        }

        // --- Connection ---
        GridLayout {
            columns: 2
            columnSpacing: 16
            rowSpacing: 12
            Label { text: qsTr("Connections per download") }
            SpinBox {
                from: 1; to: 32
                editable: true
                value: Number(App.settings["connections"]) || 8
                onValueModified: win.apply("connections", String(value))
            }
            Label { text: qsTr("Downloads at the same time") }
            SpinBox {
                from: 1; to: 64
                editable: true
                value: Number(App.settings["max_active"]) || 4
                onValueModified: win.apply("max_active", String(value))
            }
            Label { text: qsTr("Speed limit for all downloads") }
            RowLayout {
                TextField {
                    id: globalLimit
                    Layout.preferredWidth: 120
                    placeholderText: qsTr("none")
                    text: Number(App.settings["speed_limit"]) > 0
                          ? App.formatSpeed(Number(App.settings["speed_limit"])).replace("/s", "") : ""
                    onEditingFinished: {
                        const bytes = text.trim().length === 0 ? 0 : App.parseSize(text)
                        if (bytes < 0)
                            win.error = qsTr("The speed limit is not a size (examples: 500K, 2M).")
                        else
                            win.apply("speed_limit", String(Math.round(bytes)))
                    }
                }
                Label { text: qsTr("per second (e.g. 800K, 2M; empty = none)"); color: palette.placeholderText }
            }
            Label { text: qsTr("Proxy") }
            CheckBox {
                text: qsTr("Use the Windows proxy settings (manual, PAC script or automatic)")
                checked: App.settings["system_proxy"] !== "0"
                onToggled: win.apply("system_proxy", checked ? "1" : "0")
            }
            Item { Layout.fillHeight: true; Layout.columnSpan: 2 }
        }

        // --- Categories ---
        ColumnLayout {
            spacing: 8
            property var rows: App.categories
            Label {
                text: qsTr("Files are sorted by extension into these categories. Folders are relative to the download folder unless absolute.")
                wrapMode: Text.Wrap
                color: palette.placeholderText
                Layout.fillWidth: true
            }
            ListView {
                id: catList
                Layout.fillWidth: true
                Layout.fillHeight: true
                clip: true
                spacing: 6
                model: parent.rows
                ScrollBar.vertical: ScrollBar {}
                delegate: RowLayout {
                    id: cat
                    required property var modelData
                    required property int index
                    width: catList.width - 12
                    spacing: 8
                    function save(field, value) {
                        let list = App.categories.slice()
                        list[index] = Object.assign({}, list[index])
                        list[index][field] = value
                        win.error = App.setCategories(list)
                    }
                    TextField {
                        text: cat.modelData.name
                        Layout.preferredWidth: 110
                        onEditingFinished: if (text !== cat.modelData.name) cat.save("name", text)
                    }
                    TextField {
                        text: cat.modelData.extensions
                        Layout.fillWidth: true
                        onEditingFinished: if (text !== cat.modelData.extensions) cat.save("extensions", text)
                    }
                    TextField {
                        text: cat.modelData.folder
                        placeholderText: cat.modelData.name
                        Layout.preferredWidth: 150
                        onEditingFinished: if (text !== cat.modelData.folder) cat.save("folder", text)
                    }
                    ToolButton {
                        text: ""
                        font.family: App.iconFont
                        ToolTip.visible: hovered
                        ToolTip.text: qsTr("Remove category")
                        onClicked: {
                            let list = App.categories.slice()
                            list.splice(cat.index, 1)
                            win.error = App.setCategories(list)
                        }
                    }
                }
            }
            RowLayout {
                Button {
                    text: qsTr("Add category")
                    onClicked: win.error = App.setCategories(App.categories.concat(
                        [{ name: qsTr("New category"), extensions: "", folder: "" }]))
                }
                Button {
                    text: qsTr("Restore defaults")
                    onClicked: win.error = App.setCategories(App.defaultCategories())
                }
            }
        }

        // --- Site logins ---
        ColumnLayout {
            spacing: 8
            property var rows: App.logins()
            function reload() { rows = App.logins() }
            Label {
                text: qsTr("User names and passwords for sites that ask for them. They are kept in Windows Credential Manager, never in Lanes' files.")
                wrapMode: Text.Wrap
                color: palette.placeholderText
                Layout.fillWidth: true
            }
            ListView {
                id: loginList
                Layout.fillWidth: true
                Layout.fillHeight: true
                clip: true
                model: parent.rows
                delegate: RowLayout {
                    required property var modelData
                    width: loginList.width
                    Label { text: modelData.host; Layout.preferredWidth: 260; elide: Text.ElideRight }
                    Label { text: modelData.username; Layout.fillWidth: true; elide: Text.ElideRight }
                    Button {
                        text: qsTr("Remove")
                        onClicked: {
                            win.error = App.removeLogin(modelData.host)
                            loginList.parent.reload()
                        }
                    }
                }
                Label {
                    anchors.centerIn: parent
                    visible: loginList.count === 0
                    text: qsTr("No saved logins.")
                    color: palette.placeholderText
                }
            }
            RowLayout {
                Layout.fillWidth: true
                TextField { id: host; placeholderText: qsTr("files.example.com"); Layout.fillWidth: true }
                TextField { id: user; placeholderText: qsTr("User name"); Layout.preferredWidth: 140 }
                TextField { id: pass; placeholderText: qsTr("Password"); echoMode: TextInput.Password; Layout.preferredWidth: 140 }
                Button {
                    text: qsTr("Save")
                    enabled: host.text.trim().length > 0 && user.text.length > 0
                    onClicked: {
                        win.error = App.saveLogin(host.text, user.text, pass.text)
                        if (win.error.length === 0) {
                            host.text = ""; user.text = ""; pass.text = ""
                        }
                        loginList.parent.reload()
                    }
                }
            }
        }

        // --- About ---
        ColumnLayout {
            spacing: 10
            RowLayout {
                spacing: 14
                Icon { glyph: ""; size: 40; color: palette.accent }
                ColumnLayout {
                    spacing: 2
                    Label { text: "Lanes Download Manager"; font.pixelSize: 18; font.weight: Font.DemiBold }
                    Label { text: qsTr("Version %1").arg(App.version); color: palette.placeholderText }
                }
            }
            Label {
                text: qsTr("Free software under the GNU General Public License, version 3 or later. You are responsible for what you download; some sites' terms restrict downloading.")
                wrapMode: Text.Wrap
                Layout.fillWidth: true
            }
            RowLayout {
                Button { text: qsTr("Open-source licenses"); onClicked: licenses.text = App.licenseText("THIRD_PARTY_NOTICES.md") }
                Button { text: qsTr("GNU GPL v3"); onClicked: licenses.text = App.licenseText("LICENSE") }
            }
            ScrollView {
                Layout.fillWidth: true
                Layout.fillHeight: true
                TextArea {
                    id: licenses
                    readOnly: true
                    wrapMode: TextEdit.Wrap
                    font.family: "Consolas"
                    font.pixelSize: 12
                    text: App.licenseText("THIRD_PARTY_NOTICES.md")
                }
            }
        }
    }
}
