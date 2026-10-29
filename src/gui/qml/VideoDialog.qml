// SPDX-License-Identifier: GPL-3.0-or-later
import QtQuick
import QtQuick.Controls
import QtQuick.Dialogs
import QtQuick.Layouts
import Lanes

// Video quality window: yt-dlp reads the page; pick the quality, audio only,
// subtitles and the thumbnail, or the videos of a playlist.
ApplicationWindow {
    id: win
    property string pageUrl
    property var initialRequest: ({})

    property var info: null
    property bool loading: true
    property string choice: ""
    property var subtitles: []
    property var selectedEntries: []
    property string updateMessage

    width: 620
    height: 600
    minimumWidth: 480
    minimumHeight: 420
    title: info && info.ok ? info.title : qsTr("Download video")
    flags: Qt.Dialog

    function load() {
        loading = true
        info = null
        App.extractVideo(pageUrl, Object.assign({ playlist: false }, initialRequest))
    }

    function add(queue) {
        const pages = info.isPlaylist
            ? info.entries.filter((e, i) => selectedEntries.indexOf(i) >= 0).map(e => e.url)
            : [pageUrl]
        let firstId = -1
        let failures = []
        for (const url of pages) {
            const r = App.add({
                url: url,
                video: { format: choice, subtitles: subtitles, thumbnail: thumbnail.checked },
                folder: folder.text,
                queue: queue,
                referer: initialRequest.referer || "",
                cookies: initialRequest.cookies || "",
                userAgent: initialRequest.userAgent || "",
                ifDuplicate: "rename",
            })
            if (r.ok) {
                if (firstId < 0)
                    firstId = r.id
            } else {
                failures.push(r.error)
            }
        }
        if (failures.length > 0) {
            error.text = failures.join("\n")
            return
        }
        if (queue.length === 0 && pages.length === 1 && App.settings["gui/showProgress"])
            App.requestProgress(firstId)
        win.close()
    }

    Component.onCompleted: {
        folder.text = App.folderFor("video.mp4")
        load()
        raise()
        requestActivate()
    }

    Connections {
        target: App
        function onVideoInfoReady(url, result) {
            if (url !== win.pageUrl)
                return
            win.loading = false
            win.info = result
            if (result.ok) {
                win.choice = result.choices.length > 0 ? result.choices[0].key : "best"
                win.selectedEntries = result.isPlaylist ? result.entries.map((e, i) => i) : []
            }
        }
        function onExtractorUpdated(ok, message) {
            win.updateMessage = message
            if (ok)
                win.load()
        }
    }

    Shortcut { sequence: "Esc"; onActivated: win.close() }
    background: Rectangle { color: palette.window }

    ColumnLayout {
        anchors.fill: parent
        anchors.margins: 20
        spacing: 12

        // Reading the page.
        ColumnLayout {
            visible: win.loading
            Layout.fillWidth: true
            Layout.fillHeight: true
            spacing: 12
            Item { Layout.fillHeight: true }
            BusyIndicator { running: win.loading; Layout.alignment: Qt.AlignHCenter }
            Label {
                text: qsTr("Reading the video page…")
                color: palette.placeholderText
                Layout.alignment: Qt.AlignHCenter
            }
            Label {
                text: win.pageUrl
                elide: Text.ElideMiddle
                color: palette.placeholderText
                font.pixelSize: 11
                Layout.fillWidth: true
                horizontalAlignment: Text.AlignHCenter
            }
            Item { Layout.fillHeight: true }
        }

        // It did not work.
        ColumnLayout {
            visible: !win.loading && win.info !== null && !win.info.ok
            Layout.fillWidth: true
            Layout.fillHeight: true
            spacing: 10
            RowLayout {
                spacing: 12
                Icon { glyph: ""; size: 26; color: "#c42b1c" }
                Label {
                    text: qsTr("Lanes could not read this page")
                    font.pixelSize: 16
                    font.weight: Font.DemiBold
                }
            }
            Label {
                text: win.info && !win.info.ok ? win.info.error : ""
                wrapMode: Text.Wrap
                Layout.fillWidth: true
            }
            Label {
                text: qsTr("Video sites change often. Updating the extractor (yt-dlp) usually helps.")
                wrapMode: Text.Wrap
                color: palette.placeholderText
                Layout.fillWidth: true
            }
            Label {
                visible: win.updateMessage.length > 0
                text: win.updateMessage
                wrapMode: Text.Wrap
                Layout.fillWidth: true
            }
            RowLayout {
                Button {
                    text: qsTr("Update extractor")
                    highlighted: true
                    onClicked: {
                        win.updateMessage = qsTr("Updating…")
                        App.updateExtractor()
                    }
                }
                Button { text: qsTr("Try again"); onClicked: win.load() }
            }
            Item { Layout.fillHeight: true }
        }

        // The video (or playlist).
        RowLayout {
            visible: !win.loading && win.info !== null && win.info.ok
            spacing: 14
            Layout.fillWidth: true
            Rectangle {
                Layout.preferredWidth: 160
                Layout.preferredHeight: 90
                radius: 6
                clip: true
                color: Qt.rgba(palette.text.r, palette.text.g, palette.text.b, 0.06)
                visible: thumb.status === Image.Ready || thumb.status === Image.Loading
                Image {
                    id: thumb
                    anchors.fill: parent
                    source: win.info && win.info.ok ? win.info.thumbnail : ""
                    fillMode: Image.PreserveAspectCrop
                    asynchronous: true
                }
            }
            ColumnLayout {
                spacing: 4
                Layout.fillWidth: true
                Label {
                    text: win.info && win.info.ok ? win.info.title : ""
                    font.pixelSize: 16
                    font.weight: Font.DemiBold
                    wrapMode: Text.Wrap
                    maximumLineCount: 3
                    elide: Text.ElideRight
                    Layout.fillWidth: true
                }
                Label {
                    text: {
                        if (!win.info || !win.info.ok)
                            return ""
                        if (win.info.isPlaylist)
                            return qsTr("Playlist · %1 videos").arg(win.info.entries.length)
                        let parts = []
                        if (win.info.uploader.length > 0)
                            parts.push(win.info.uploader)
                        if (win.info.duration > 0)
                            parts.push(App.formatDuration(win.info.duration))
                        return parts.join("  ·  ")
                    }
                    color: palette.placeholderText
                }
            }
        }

        // Playlist entries.
        ListView {
            id: entries
            visible: !win.loading && win.info !== null && win.info.ok && win.info.isPlaylist
            Layout.fillWidth: true
            Layout.preferredHeight: 150
            clip: true
            model: win.info && win.info.ok && win.info.isPlaylist ? win.info.entries : []
            ScrollBar.vertical: ScrollBar {}
            delegate: CheckDelegate {
                required property var modelData
                required property int index
                width: entries.width - 12
                text: (index + 1) + ".  " + (modelData.title || modelData.url)
                checked: win.selectedEntries.indexOf(index) >= 0
                onToggled: {
                    const s = win.selectedEntries.filter(i => i !== index)
                    if (checked)
                        s.push(index)
                    win.selectedEntries = s
                }
            }
        }

        // Quality.
        Label {
            visible: !win.loading && win.info !== null && win.info.ok
            text: qsTr("Quality")
            font.weight: Font.DemiBold
        }
        ListView {
            id: choices
            visible: !win.loading && win.info !== null && win.info.ok
            Layout.fillWidth: true
            Layout.fillHeight: true
            Layout.minimumHeight: 120
            clip: true
            model: win.info && win.info.ok ? win.info.choices : []
            ScrollBar.vertical: ScrollBar {}
            delegate: ItemDelegate {
                id: option
                required property var modelData
                width: choices.width - 12
                highlighted: win.choice === modelData.key
                onClicked: win.choice = modelData.key
                contentItem: RowLayout {
                    spacing: 10
                    RadioButton {
                        checked: win.choice === option.modelData.key
                        onClicked: win.choice = option.modelData.key
                        padding: 0
                    }
                    Label { text: option.modelData.label; Layout.preferredWidth: 140 }
                    Label {
                        text: option.modelData.detail
                        color: palette.placeholderText
                        Layout.fillWidth: true
                    }
                }
            }
        }

        // Subtitles, thumbnail, folder.
        Flow {
            visible: !win.loading && win.info !== null && win.info.ok && win.info.subtitles.length > 0
            Layout.fillWidth: true
            spacing: 6
            Label { text: qsTr("Subtitles:"); topPadding: 8 }
            Repeater {
                model: win.info && win.info.ok ? win.info.subtitles : []
                CheckBox {
                    required property string modelData
                    text: modelData
                    onToggled: win.subtitles = checked ? win.subtitles.concat([modelData])
                                                       : win.subtitles.filter(l => l !== modelData)
                }
            }
        }
        CheckBox {
            id: thumbnail
            visible: !win.loading && win.info !== null && win.info.ok
            text: qsTr("Also save the thumbnail")
        }
        RowLayout {
            visible: !win.loading && win.info !== null && win.info.ok
            Layout.fillWidth: true
            Label { text: qsTr("Save to") }
            TextField { id: folder; Layout.fillWidth: true }
            Button { text: qsTr("Browse…"); onClicked: folderDialog.open() }
        }

        Label {
            id: error
            visible: text.length > 0
            color: "#c42b1c"
            wrapMode: Text.Wrap
            Layout.fillWidth: true
        }

        RowLayout {
            Layout.fillWidth: true
            spacing: 8
            Button {
                id: laterButton
                visible: win.info !== null && win.info.ok
                text: qsTr("Download later") + "  ▾"
                onClicked: laterMenu.popup(laterButton, 0, laterButton.height)
                Menu {
                    id: laterMenu
                    Repeater {
                        model: App.queues
                        MenuItem {
                            required property var modelData
                            text: qsTr("Add to “%1”").arg(modelData.name)
                            onTriggered: win.add(modelData.name)
                        }
                    }
                }
            }
            Item { Layout.fillWidth: true }
            Button { text: qsTr("Cancel"); onClicked: win.close() }
            Button {
                visible: win.info !== null && win.info.ok
                text: win.info && win.info.ok && win.info.isPlaylist
                      ? qsTr("Download %1 videos").arg(win.selectedEntries.length) : qsTr("Download")
                highlighted: true
                enabled: win.choice.length > 0 && (!win.info.isPlaylist || win.selectedEntries.length > 0)
                onClicked: win.add("")
            }
        }
    }

    FolderDialog {
        id: folderDialog
        title: qsTr("Save videos to")
        onAccepted: folder.text = decodeURIComponent(selectedFolder.toString().replace(/^file:\/{3}/, "")).replace(/\//g, "\\")
    }
}
