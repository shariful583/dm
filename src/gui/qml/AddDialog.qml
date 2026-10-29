// SPDX-License-Identifier: GPL-3.0-or-later
import QtQuick
import QtQuick.Controls
import QtQuick.Dialogs
import QtQuick.Layouts
import Lanes

// "Add download": paste one link (probed for name, size, resume support and category) or many
// (one per line, or a pattern like img[001-120].jpg). Start now, or put them in a queue.
ApplicationWindow {
    id: win
    property string initialText

    width: 600
    height: advanced.checked ? 690 : 430
    minimumWidth: 480
    minimumHeight: 380
    title: qsTr("Add download")
    flags: Qt.Dialog

    // Probe state for the single-link case.
    property string probedUrl
    property var probe: null
    property bool probing: false
    property bool nameEdited: false
    property bool folderEdited: false

    readonly property var expanded: App.expandUrls(urlField.text)
    readonly property var urls: expanded.urls
    readonly property bool batch: urls.length > 1
    readonly property bool single: urls.length === 1 && App.isUrl(urls[0])

    // From the browser extension: referer, cookies, userAgent, fileName, method + body (POST).
    property var initialRequest: ({})
    readonly property bool fromBrowser: (initialRequest.cookies || initialRequest.referer || "").length > 0

    function requestFields() {
        return {
            referer: referer.text, cookies: cookies.text, userAgent: userAgent.text,
            username: username.text, password: password.text, headers: headers.text,
            method: initialRequest.method || "GET", body: initialRequest.body || ""
        }
    }

    function startProbe() {
        if (!single || initialRequest.method === "POST") { // a form's reply cannot be checked first
            probe = null
            probing = false
            return
        }
        if (urls[0] === probedUrl && (probe !== null || probing))
            return
        probedUrl = urls[0]
        probe = null
        probing = true
        App.probe(probedUrl, requestFields())
    }

    function add(queue, ifDuplicate) {
        const common = requestFields()
        const limit = speedLimit.text.trim().length > 0 ? App.parseSize(speedLimit.text) : 0
        if (limit < 0) {
            error.text = qsTr("The speed limit is not a size (examples: 500K, 2M).")
            return
        }
        common.queue = queue
        common.folder = folder.text
        common.connections = connections.value
        common.speedLimit = limit
        common.checksum = checksum.text
        common.ifDuplicate = ifDuplicate
        let firstId = -1
        let failures = []
        for (const url of urls) {
            const d = Object.assign({ url: url, fileName: batch ? "" : fileName.text }, common)
            const r = App.add(d)
            if (r.ok) {
                if (firstId < 0)
                    firstId = r.id
            } else if (r.duplicate && !batch) {
                duplicate.queue = queue
                duplicate.text = r.error + "\n\n" + qsTr("What should Lanes do?")
                duplicate.open()
                return
            } else {
                failures.push(url + ": " + r.error)
            }
        }
        if (failures.length > 0) {
            error.text = failures.join("\n")
            return
        }
        if (!batch && queue.length === 0 && firstId >= 0 && App.settings["gui/showProgress"])
            App.requestProgress(firstId)
        win.close()
    }

    Component.onCompleted: {
        let text = initialText
        if (text.length === 0) {
            const clip = App.clipboardText().trim()
            if (App.isUrl(clip.split("\n")[0].trim()))
                text = clip
        }
        referer.text = initialRequest.referer || ""
        cookies.text = initialRequest.cookies || ""
        userAgent.text = initialRequest.userAgent || ""
        if ((initialRequest.fileName || "").length > 0) {
            fileName.text = initialRequest.fileName
            nameEdited = true
            folder.text = App.folderFor(fileName.text)
        }
        urlField.text = text
        urlField.forceActiveFocus()
        urlField.selectAll()
        if (fromBrowser) {
            raise()
            requestActivate()
        }
    }

    Connections {
        target: App
        function onProbeFinished(url, result) {
            if (url !== win.probedUrl)
                return // an older link
            win.probing = false
            win.probe = result
            if (result.ok) {
                if (!win.nameEdited)
                    fileName.text = result.fileName
                if (!win.folderEdited)
                    folder.text = result.folder
            }
        }
    }

    Timer {
        id: probeDelay
        interval: 450
        onTriggered: win.startProbe()
    }

    Shortcut { sequence: "Esc"; onActivated: win.close() }

    background: Rectangle { color: palette.window }

    ColumnLayout {
        anchors.fill: parent
        anchors.margins: 20
        spacing: 12

        Label {
            text: qsTr("Address")
            font.weight: Font.DemiBold
        }
        ScrollView {
            Layout.fillWidth: true
            Layout.preferredHeight: win.batch || urlField.lineCount > 1 ? 96 : urlField.implicitHeight
            TextArea {
                id: urlField
                placeholderText: qsTr("https://example.com/file.zip — or several links, one per line")
                wrapMode: TextEdit.NoWrap
                onTextChanged: {
                    error.text = ""
                    probeDelay.restart()
                }
                Keys.onReturnPressed: event => {
                    if (event.modifiers & Qt.ShiftModifier || win.batch) {
                        event.accepted = false
                    } else if (startButton.enabled) {
                        win.add("", "ask")
                    }
                }
            }
        }

        // What the server said (single link), or how many links (batch).
        RowLayout {
            Layout.fillWidth: true
            spacing: 8
            BusyIndicator {
                running: win.probing
                visible: running
                Layout.preferredWidth: 20
                Layout.preferredHeight: 20
            }
            Label {
                Layout.fillWidth: true
                wrapMode: Text.Wrap
                color: win.probe && !win.probe.ok ? "#c42b1c" : palette.placeholderText
                text: {
                    if (win.expanded.error.length > 0)
                        return win.expanded.error
                    if (win.batch)
                        return qsTr("%n links will be added.", "", win.urls.length)
                    if (win.urls.length === 1 && !win.single)
                        return qsTr("Only http, https, ftp and ftps links can be downloaded.")
                    if (win.initialRequest.method === "POST")
                        return qsTr("Sent by a form on the page; the size is known once it starts.")
                    if (win.probing)
                        return qsTr("Checking the link…")
                    if (!win.probe)
                        return ""
                    if (!win.probe.ok)
                        return win.probe.error
                    let parts = [win.probe.size >= 0 ? App.formatBytes(win.probe.size) : qsTr("Size unknown")]
                    parts.push(win.probe.resumable ? qsTr("Can resume") : qsTr("Cannot resume"))
                    parts.push(win.probe.category)
                    if (win.probe.type.length > 0)
                        parts.push(win.probe.type)
                    return parts.join("  ·  ")
                }
            }
        }

        // A web page rather than a file: maybe a video site.
        Button {
            visible: win.single && win.probe !== null && win.probe.ok === true
                     && (win.probe.type || "").startsWith("text/html")
            text: qsTr("This is a web page: find its video")
            highlighted: true
            onClicked: {
                App.requestVideo(win.urls[0], win.requestFields())
                win.close()
            }
        }

        GridLayout {
            columns: 3
            columnSpacing: 10
            rowSpacing: 10
            Layout.fillWidth: true

            Label { text: qsTr("File name"); visible: !win.batch }
            TextField {
                id: fileName
                visible: !win.batch
                Layout.fillWidth: true
                Layout.columnSpan: 2
                placeholderText: qsTr("From the server")
                onTextEdited: {
                    win.nameEdited = true
                    if (!win.folderEdited && text.length > 0)
                        folder.text = App.folderFor(text)
                }
            }

            Label { text: qsTr("Save to") }
            TextField {
                id: folder
                Layout.fillWidth: true
                placeholderText: qsTr("Folder for the file type")
                onTextEdited: win.folderEdited = true
            }
            Button {
                text: qsTr("Browse…")
                onClicked: folderDialog.open()
            }
        }

        CheckBox {
            id: advanced
            text: qsTr("More options")
        }

        GridLayout {
            visible: advanced.checked
            columns: 4
            columnSpacing: 10
            rowSpacing: 8
            Layout.fillWidth: true

            Label { text: qsTr("Connections") }
            SpinBox {
                id: connections
                from: 1; to: 32
                value: Number(App.settings["connections"]) || 8
                editable: true
            }
            Label { text: qsTr("Speed limit") }
            TextField { id: speedLimit; placeholderText: qsTr("none (e.g. 2M)"); Layout.fillWidth: true }

            Label { text: qsTr("Referer") }
            TextField { id: referer; Layout.fillWidth: true; Layout.columnSpan: 3 }

            Label { text: qsTr("User agent") }
            TextField { id: userAgent; placeholderText: qsTr("Lanes default"); Layout.fillWidth: true; Layout.columnSpan: 3 }

            Label { text: qsTr("Cookies") }
            TextField { id: cookies; placeholderText: "name=value; name2=value2"; Layout.fillWidth: true; Layout.columnSpan: 3 }

            Label { text: qsTr("User name") }
            TextField { id: username; Layout.fillWidth: true }
            Label { text: qsTr("Password") }
            TextField { id: password; echoMode: TextInput.Password; Layout.fillWidth: true }

            Label { text: qsTr("Headers"); Layout.alignment: Qt.AlignTop }
            TextArea {
                id: headers
                placeholderText: "Name: value"
                Layout.fillWidth: true
                Layout.columnSpan: 3
                Layout.preferredHeight: 56
            }

            Label { text: qsTr("Checksum") }
            TextField {
                id: checksum
                placeholderText: "sha256:<hex>  or  md5:<hex>"
                Layout.fillWidth: true
                Layout.columnSpan: 3
            }
        }

        Label {
            id: error
            visible: text.length > 0
            color: "#c42b1c"
            wrapMode: Text.Wrap
            Layout.fillWidth: true
        }

        Item { Layout.fillHeight: true }

        RowLayout {
            Layout.fillWidth: true
            spacing: 8
            Button {
                id: laterButton
                text: qsTr("Download later") + "  ▾"
                enabled: startButton.enabled
                onClicked: laterMenu.popup(laterButton, 0, laterButton.height)
                Menu {
                    id: laterMenu
                    Repeater {
                        model: App.queues
                        MenuItem {
                            required property var modelData
                            text: qsTr("Add to “%1”").arg(modelData.name)
                            onTriggered: win.add(modelData.name, "ask")
                        }
                    }
                    MenuItem {
                        visible: App.queues.length === 0
                        height: visible ? implicitHeight : 0
                        text: qsTr("Add to “main”")
                        onTriggered: win.add("main", "ask")
                    }
                }
            }
            Item { Layout.fillWidth: true }
            Button {
                text: qsTr("Cancel")
                onClicked: win.close()
            }
            Button {
                id: startButton
                text: win.batch ? qsTr("Download %n files", "", win.urls.length) : qsTr("Download")
                highlighted: true
                enabled: (win.single || win.batch) && win.expanded.error.length === 0
                onClicked: win.add("", "ask")
            }
        }
    }

    FolderDialog {
        id: folderDialog
        title: qsTr("Save downloads to")
        onAccepted: {
            let path = decodeURIComponent(selectedFolder.toString().replace(/^file:\/{3}/, ""))
            folder.text = path.replace(/\//g, "\\")
            win.folderEdited = true
        }
    }

    MessageDialog {
        id: duplicate
        property string queue
        title: qsTr("Already in the list")
        choices: [
            { key: "", text: qsTr("Cancel") },
            { key: "rename", text: qsTr("Add again") },
            { key: "overwrite", text: qsTr("Replace") },
            { key: "resume", text: qsTr("Resume existing"), accent: true },
        ]
        onChosen: key => {
            if (key.length > 0)
                win.add(duplicate.queue, key)
        }
    }
}
