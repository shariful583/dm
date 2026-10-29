// SPDX-License-Identifier: GPL-3.0-or-later
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import Lanes

// Main window: toolbar, sidebar (views, categories, queues), the download list and a status bar.
ApplicationWindow {
    id: root
    property bool startHidden: false
    property bool mica: false // set from C++ when the Windows 11 backdrop is active

    width: 1120
    height: 680
    minimumWidth: 780
    minimumHeight: 420
    visible: !startHidden
    title: App.model.activeCount > 0
           ? qsTr("Lanes — %1").arg(App.formatSpeed(App.model.totalSpeed))
           : qsTr("Lanes")
    color: mica ? "transparent" : palette.window

    readonly property color layerColor: mica
        ? Qt.rgba(palette.base.r, palette.base.g, palette.base.b, 0.55) : palette.base
    readonly property color strokeColor: Qt.rgba(palette.text.r, palette.text.g, palette.text.b, 0.08)
    readonly property var columns: ({ size: 80, progress: 170, timeLeft: 70, speed: 90, added: 130 })

    // Selection: ids of the selected rows; `anchorRow` for shift-click ranges.
    property var selection: []
    property int anchorRow: -1
    function isSelected(id) { return selection.indexOf(id) >= 0 }
    function select(ids) { selection = ids }
    function selectedOrAll() { return selection.length > 0 ? selection : [] }

    // --- windows ---
    property var progressWindows: ({})
    property var singletons: ({})

    function bringUp() {
        if (root.visibility === Window.Minimized || !root.visible)
            root.showNormal()
        root.raise()
        root.requestActivate()
    }
    function openAdd(urls, request) {
        const w = addComponent.createObject(null, { initialText: (urls || []).join("\n"),
                                                    initialRequest: request || {} })
        w.closing.connect(() => w.destroy())
        w.show()
        w.raise()
        w.requestActivate()
        return w
    }
    function openVideo(url, request) {
        const w = videoComponent.createObject(null, { pageUrl: url, initialRequest: request || {} })
        w.closing.connect(() => w.destroy())
        w.show()
        return w
    }
    function openProgress(id) {
        let w = progressWindows[id]
        if (!w) {
            w = progressComponent.createObject(null, { downloadId: id })
            w.closing.connect(() => { delete progressWindows[id]; w.destroy() })
            progressWindows[id] = w
        }
        w.show()
        w.raise()
        w.requestActivate()
        return w
    }
    function openSingleton(name, component) {
        let w = singletons[name]
        if (!w) {
            w = component.createObject(null)
            w.closing.connect(() => { delete singletons[name]; w.destroy() })
            singletons[name] = w
        }
        w.show()
        w.raise()
        w.requestActivate()
        return w
    }

    // Development (lanes-gui --show): "add[:<url>]", "options", "queues", "progress:<id>".
    function devShow(what) {
        if (what === "add" || what.startsWith("add:"))
            openAdd(what.length > 4 ? [what.substring(4)] : [])
        else if (what === "options")
            openSingleton("options", optionsComponent)
        else if (what === "queues")
            openSingleton("queues", queuesComponent)
        else if (what.startsWith("video:"))
            openVideo(what.substring(6), {})
        else if (what.startsWith("progress:"))
            openProgress(Number(what.substring(9)))
    }

    // --- actions on the selection ---
    function forSelection(fn) {
        let errors = []
        for (const id of selection) {
            const e = fn(id)
            if (e && e.length > 0)
                errors.push(e)
        }
        if (errors.length > 0)
            showMessage(qsTr("Lanes"), errors.join("\n"))
    }
    function resumeSelected() { forSelection(id => App.resume(id)) }
    function pauseSelected() { forSelection(id => App.pause(id)) }
    function removeSelected(deleteFiles) {
        if (selection.length === 0)
            return
        confirm.text = deleteFiles
            ? (selection.length === 1 ? qsTr("Remove this download from the list and delete its file from disk?")
                                      : qsTr("Remove %1 downloads from the list and delete their files from disk?").arg(selection.length))
            : (selection.length === 1 ? qsTr("Remove this download from the list? The file on disk is kept.")
                                      : qsTr("Remove %1 downloads from the list? Files on disk are kept.").arg(selection.length))
        confirm.deleteFiles = deleteFiles
        confirm.open()
    }
    function showMessage(title, text) {
        message.title = title
        message.text = text
        message.open()
    }
    function pasteUrls() {
        const text = App.clipboardText().trim()
        openAdd(text.length > 0 ? [text] : [])
    }

    onClosing: close => {
        if (App.settings["gui/minimizeToTray"]) {
            close.accepted = false
            root.hide()
        } else {
            App.quit()
        }
    }

    Connections {
        target: App
        function onShowMainWindow() { root.bringUp() }
        function onAddUrlsRequested(urls, request) { root.openAdd(urls, request) }
        function onProgressWindowRequested(id) { root.openProgress(id) }
        function onVideoPageRequested(url, request) { root.openVideo(url, request) }
    }
    // Keep the selection to rows that still exist.
    Connections {
        target: App.downloads
        function onCountChanged() {
            const kept = root.selection.filter(id => App.downloads.rowOf(id) >= 0)
            if (kept.length !== root.selection.length)
                root.selection = kept
        }
    }

    Component { id: addComponent; AddDialog {} }
    Component { id: progressComponent; ProgressWindow {} }
    Component { id: videoComponent; VideoDialog {} }
    Component { id: optionsComponent; OptionsWindow {} }
    Component { id: queuesComponent; QueuesWindow {} }

    Shortcut { sequences: [StandardKey.New]; onActivated: root.openAdd([]) }
    Shortcut { sequences: [StandardKey.Paste]; enabled: !search.activeFocus; onActivated: root.pasteUrls() }
    Shortcut { sequences: [StandardKey.Find]; onActivated: search.forceActiveFocus() }
    Shortcut {
        sequences: [StandardKey.SelectAll]
        enabled: !search.activeFocus
        onActivated: root.select(App.downloads.allIds())
    }
    Shortcut { sequences: [StandardKey.Delete]; enabled: !search.activeFocus; onActivated: root.removeSelected(false) }
    Shortcut { sequence: "Shift+Del"; enabled: !search.activeFocus; onActivated: root.removeSelected(true) }

    // --- toolbar ---
    header: ToolBar {
        background: Item {}
        leftPadding: 8
        rightPadding: 12
        RowLayout {
            anchors.fill: parent
            spacing: 2
            ToolButtonIcon {
                text: qsTr("Add URL")
                glyph: ""
                glyphColor: palette.accent
                tip: qsTr("Add a download (Ctrl+N, or Ctrl+V to paste a link)")
                onClicked: root.openAdd([])
            }
            ToolSeparator {}
            ToolButtonIcon {
                text: qsTr("Resume")
                glyph: ""
                tip: qsTr("Resume the selected downloads")
                enabled: root.selection.length > 0
                onClicked: root.resumeSelected()
            }
            ToolButtonIcon {
                text: qsTr("Pause")
                glyph: ""
                tip: qsTr("Pause the selected downloads")
                enabled: root.selection.length > 0
                onClicked: root.pauseSelected()
            }
            ToolButtonIcon {
                text: qsTr("Pause all")
                glyph: ""
                tip: qsTr("Pause every running download")
                onClicked: App.pauseAll()
            }
            ToolButtonIcon {
                text: qsTr("Delete")
                glyph: ""
                tip: qsTr("Remove the selected downloads from the list (Shift+Del also deletes the files)")
                enabled: root.selection.length > 0
                onClicked: root.removeSelected(false)
            }
            ToolSeparator {}
            ToolButtonIcon {
                text: qsTr("Queues")
                glyph: ""
                tip: qsTr("Queues and scheduler")
                onClicked: root.openSingleton("queues", queuesComponent)
            }
            ToolButtonIcon {
                id: limitButton
                text: qsTr("Speed")
                glyph: ""
                tip: qsTr("Global speed limit")
                glyphColor: Number(App.settings["speed_limit"]) > 0 ? palette.accent : palette.buttonText
                onClicked: limitMenu.popup(limitButton, 0, limitButton.height)
            }
            ToolButtonIcon {
                text: qsTr("Options")
                glyph: ""
                tip: qsTr("Options")
                onClicked: root.openSingleton("options", optionsComponent)
            }
            Item { Layout.fillWidth: true }
            TextField {
                id: search
                placeholderText: qsTr("Search downloads")
                Layout.preferredWidth: 260
                rightPadding: 32
                onTextChanged: App.downloads.search = text
                Keys.onEscapePressed: text = ""
                Icon {
                    glyph: ""
                    size: 13
                    color: palette.placeholderText
                    anchors.right: parent.right
                    anchors.rightMargin: 10
                    anchors.verticalCenter: parent.verticalCenter
                }
            }
        }
    }

    SpeedLimitMenu { id: limitMenu }

    // --- body: sidebar + list ---
    RowLayout {
        anchors.fill: parent
        anchors.topMargin: 4
        spacing: 0

        ListView {
            id: sidebar
            Layout.preferredWidth: 220
            Layout.fillHeight: true
            clip: true
            boundsBehavior: Flickable.StopAtBounds
            leftMargin: 6
            rightMargin: 6
            model: {
                let items = [
                    { key: "all", text: qsTr("All downloads"), glyph: "", section: "" },
                    { key: "unfinished", text: qsTr("Unfinished"), glyph: "", section: "" },
                    { key: "finished", text: qsTr("Finished"), glyph: "", section: "" },
                ]
                const glyphs = { Compressed: "", Documents: "", Music: "",
                                 Programs: "", Video: "", General: "" }
                for (const c of App.categories.concat([{ name: "General" }]))
                    items.push({ key: "category:" + c.name, text: c.name,
                                 glyph: glyphs[c.name] || "", section: qsTr("Categories") })
                for (const q of App.queues)
                    items.push({ key: "queue:" + q.name, text: q.name,
                                 glyph: q.enabled ? "" : "", section: qsTr("Queues") })
                return items
            }
            section.property: "section"
            section.delegate: Label {
                required property string section
                text: section
                visible: section.length > 0
                height: section.length > 0 ? 34 : 0
                leftPadding: 12
                verticalAlignment: Text.AlignBottom
                bottomPadding: 4
                font.pixelSize: 12
                font.weight: Font.DemiBold
                color: palette.placeholderText
            }
            delegate: ItemDelegate {
                id: side
                required property var modelData
                width: ListView.view.width - 12
                height: 36
                highlighted: App.downloads.view === modelData.key
                onClicked: App.downloads.view = modelData.key
                background: Rectangle {
                    radius: 4
                    color: side.highlighted ? Qt.rgba(palette.text.r, palette.text.g, palette.text.b, 0.07)
                         : side.hovered ? Qt.rgba(palette.text.r, palette.text.g, palette.text.b, 0.04)
                         : "transparent"
                    Rectangle {
                        visible: side.highlighted
                        width: 3; radius: 1.5
                        height: 16
                        anchors.verticalCenter: parent.verticalCenter
                        color: palette.accent
                    }
                }
                contentItem: RowLayout {
                    spacing: 10
                    Icon { glyph: side.modelData.glyph; size: 15; Layout.leftMargin: 6 }
                    Label { text: side.modelData.text; elide: Text.ElideRight; Layout.fillWidth: true }
                    Label {
                        readonly property int n: App.model.counts[side.modelData.key] || 0
                        text: n
                        visible: n > 0
                        font.pixelSize: 11
                        color: palette.placeholderText
                    }
                }
            }
        }

        // The list sits on a "layer", as content does in WinUI apps.
        Rectangle {
            Layout.fillWidth: true
            Layout.fillHeight: true
            color: root.layerColor
            border.color: root.strokeColor
            topLeftRadius: 8

            ColumnLayout {
                anchors.fill: parent
                anchors.margins: 1
                spacing: 0

                // Column headers: click to sort, again to reverse.
                RowLayout {
                    id: headerRow
                    Layout.fillWidth: true
                    Layout.leftMargin: 12 + 24 + 12
                    Layout.rightMargin: 12 + 8
                    Layout.preferredHeight: 34
                    spacing: 12
                    component SortHeader: Label {
                        id: sh
                        property string key
                        property int align: Text.AlignRight
                        horizontalAlignment: align
                        font.pixelSize: 12
                        color: palette.placeholderText
                        text: base + (App.downloads.sortKey === key ? (App.downloads.descending ? "  ↓" : "  ↑") : "")
                        property string base
                        MouseArea {
                            anchors.fill: parent
                            cursorShape: Qt.PointingHandCursor
                            onClicked: {
                                if (App.downloads.sortKey === sh.key)
                                    App.downloads.descending = !App.downloads.descending
                                else {
                                    App.downloads.sortKey = sh.key
                                    App.downloads.descending = sh.key !== "name"
                                }
                            }
                        }
                    }
                    SortHeader { base: qsTr("Name"); key: "name"; align: Text.AlignLeft; Layout.fillWidth: true }
                    SortHeader { base: qsTr("Size"); key: "size"; Layout.preferredWidth: root.columns.size }
                    SortHeader { base: qsTr("Status"); key: "status"; align: Text.AlignLeft; Layout.preferredWidth: root.columns.progress }
                    SortHeader { base: qsTr("Time left"); key: "timeLeft"; Layout.preferredWidth: root.columns.timeLeft }
                    SortHeader { base: qsTr("Speed"); key: "speed"; Layout.preferredWidth: root.columns.speed }
                    SortHeader { base: qsTr("Added"); key: "added"; Layout.preferredWidth: root.columns.added }
                }
                Rectangle { Layout.fillWidth: true; height: 1; color: root.strokeColor }

                ListView {
                    id: list
                    Layout.fillWidth: true
                    Layout.fillHeight: true
                    clip: true
                    model: App.downloads
                    boundsBehavior: Flickable.StopAtBounds
                    topMargin: 4
                    bottomMargin: 4
                    leftMargin: 4
                    rightMargin: 4
                    reuseItems: true
                    activeFocusOnTab: true
                    keyNavigationEnabled: true
                    ScrollBar.vertical: ScrollBar {}

                    delegate: DownloadRow {
                        id: delegateRow
                        columns: root.columns
                        width: list.width - 8
                        selected: root.isSelected(downloadId)
                        onRowPressed: mouse => {
                            if (mouse.modifiers & Qt.ShiftModifier && root.anchorRow >= 0) {
                                root.select(App.downloads.idsBetween(root.anchorRow, index))
                            } else if (mouse.modifiers & Qt.ControlModifier) {
                                const s = root.selection.slice()
                                const at = s.indexOf(downloadId)
                                if (at >= 0) s.splice(at, 1); else s.push(downloadId)
                                root.select(s)
                                root.anchorRow = index
                            } else {
                                root.select([downloadId])
                                root.anchorRow = index
                            }
                            list.currentIndex = index
                        }
                        onMenuRequested: (x, y) => rowMenu.popup(delegateRow, x, y)
                        onOpened: status === "completed" ? App.openFile(downloadId) : root.openProgress(downloadId)
                    }

                    Keys.onUpPressed: moveSelection(-1)
                    Keys.onDownPressed: moveSelection(1)
                    Keys.onReturnPressed: {
                        if (root.selection.length === 1) {
                            const id = root.selection[0]
                            App.details(id).state === "completed" ? App.openFile(id) : root.openProgress(id)
                        }
                    }
                    Keys.onSpacePressed: {
                        for (const id of root.selection) {
                            const s = App.details(id).state
                            s === "running" || s === "queued" ? App.pause(id) : App.resume(id)
                        }
                    }
                    function moveSelection(step) {
                        const row = Math.max(0, Math.min(count - 1, currentIndex + step))
                        currentIndex = row
                        positionViewAtIndex(row, ListView.Contain)
                        root.select([App.downloads.idAt(row)])
                        root.anchorRow = row
                    }

                    // Empty state.
                    ColumnLayout {
                        anchors.centerIn: parent
                        visible: list.count === 0
                        spacing: 12
                        Icon {
                            glyph: App.downloads.search.length > 0 ? "" : ""
                            size: 40
                            color: palette.placeholderText
                            Layout.alignment: Qt.AlignHCenter
                        }
                        Label {
                            text: App.downloads.search.length > 0
                                  ? qsTr("Nothing matches “%1”.").arg(App.downloads.search)
                                  : qsTr("No downloads here yet.\nPaste a link with Ctrl+V or drop it on this window.")
                            horizontalAlignment: Text.AlignHCenter
                            color: palette.placeholderText
                            Layout.alignment: Qt.AlignHCenter
                        }
                        Button {
                            visible: App.downloads.search.length === 0
                            text: qsTr("Add URL")
                            highlighted: true
                            Layout.alignment: Qt.AlignHCenter
                            onClicked: root.openAdd([])
                        }
                    }

                    // Click on empty space clears the selection.
                    MouseArea {
                        anchors.fill: parent
                        z: -1
                        onClicked: root.select([])
                    }
                }
            }
        }
    }

    // --- status bar ---
    footer: Item {
        implicitHeight: 30
        RowLayout {
            anchors.fill: parent
            anchors.leftMargin: 14
            anchors.rightMargin: 14
            spacing: 18
            Label {
                readonly property int n: App.model.counts["all"] || 0
                text: n === 1 ? qsTr("1 download") : qsTr("%1 downloads").arg(n)
                color: palette.placeholderText
                font.pixelSize: 12
            }
            Label {
                visible: App.model.activeCount > 0
                text: qsTr("%n active", "", App.model.activeCount) + "  ·  " + App.formatSpeed(App.model.totalSpeed)
                font.pixelSize: 12
            }
            Item { Layout.fillWidth: true }
            Label {
                readonly property real limit: Number(App.settings["speed_limit"])
                text: limit > 0 ? qsTr("Speed limit: %1").arg(App.formatSpeed(limit)) : qsTr("No speed limit")
                color: limit > 0 ? palette.accent : palette.placeholderText
                font.pixelSize: 12
                MouseArea {
                    anchors.fill: parent
                    cursorShape: Qt.PointingHandCursor
                    onClicked: limitMenu.popup()
                }
            }
        }
    }

    // --- context menu for the selected rows ---
    Menu {
        id: rowMenu
        readonly property var first: root.selection.length > 0 ? App.details(root.selection[0]) : ({})
        readonly property bool one: root.selection.length === 1
        MenuItem {
            text: qsTr("Open")
            enabled: rowMenu.one && rowMenu.first.state === "completed"
            onTriggered: App.openFile(root.selection[0])
        }
        MenuItem {
            text: qsTr("Show in folder")
            enabled: rowMenu.one
            onTriggered: App.openFolder(root.selection[0])
        }
        MenuItem {
            text: qsTr("Progress and details")
            enabled: rowMenu.one
            onTriggered: root.openProgress(root.selection[0])
        }
        MenuSeparator {}
        MenuItem { text: qsTr("Resume"); onTriggered: root.resumeSelected() }
        MenuItem { text: qsTr("Pause"); onTriggered: root.pauseSelected() }
        MenuItem {
            text: qsTr("Refresh download address…")
            enabled: rowMenu.one && rowMenu.first.state !== "completed"
            onTriggered: {
                refresh.downloadId = root.selection[0]
                refresh.open()
            }
        }
        MenuItem {
            text: qsTr("Copy address")
            enabled: rowMenu.one
            onTriggered: App.copyText(rowMenu.first.url)
        }
        MenuSeparator {}
        MenuItem { text: qsTr("Remove from list"); onTriggered: root.removeSelected(false) }
        MenuItem { text: qsTr("Remove and delete file"); onTriggered: root.removeSelected(true) }
    }

    MessageDialog {
        id: confirm
        property bool deleteFiles: false
        title: qsTr("Remove downloads")
        choices: [{ key: "cancel", text: qsTr("Cancel") },
                  { key: "remove", text: confirm.deleteFiles ? qsTr("Delete") : qsTr("Remove"), accent: true }]
        onChosen: key => {
            if (key === "remove")
                root.forSelection(id => App.remove(id, confirm.deleteFiles))
        }
    }
    MessageDialog { id: message }

    // IDM's "Refresh download address": continue an expired download from a new link.
    Dialog {
        id: refresh
        property var downloadId: -1
        title: qsTr("Refresh download address")
        modal: true
        anchors.centerIn: Overlay.overlay
        width: 520
        standardButtons: Dialog.Ok | Dialog.Cancel
        onAboutToShow: newUrl.text = App.isUrl(App.clipboardText().trim()) ? App.clipboardText().trim() : ""
        onAccepted: {
            const e = App.resume(downloadId, newUrl.text)
            if (e.length > 0)
                root.showMessage(qsTr("Refresh download address"), e)
        }
        ColumnLayout {
            anchors.fill: parent
            spacing: 8
            Label {
                text: qsTr("Paste a new link to the same file. Lanes keeps what it already downloaded.")
                wrapMode: Text.Wrap
                Layout.fillWidth: true
            }
            TextField {
                id: newUrl
                placeholderText: "https://"
                Layout.fillWidth: true
            }
            Label {
                text: qsTr("Or let the browser find it: Lanes opens the page the download came from. Start the same download there and Lanes continues this one (needs the Lanes browser extension).")
                wrapMode: Text.Wrap
                color: palette.placeholderText
                Layout.fillWidth: true
                Layout.topMargin: 8
            }
            Button {
                text: qsTr("Open the page in the browser")
                onClicked: {
                    const e = App.startAutoRefresh(refresh.downloadId)
                    refresh.close()
                    if (e.length > 0)
                        root.showMessage(qsTr("Refresh download address"), e)
                }
            }
        }
    }

    // Links dropped on the window open the Add window.
    DropArea {
        anchors.fill: parent
        keys: ["text/uri-list", "text/plain"]
        onDropped: drop => {
            let urls = []
            if (drop.hasUrls)
                urls = drop.urls.map(u => u.toString()).filter(u => !u.startsWith("file:"))
            else if (drop.hasText)
                urls = [drop.text]
            if (urls.length > 0) {
                root.bringUp()
                root.openAdd(urls)
                drop.accept()
            }
        }
    }
}
