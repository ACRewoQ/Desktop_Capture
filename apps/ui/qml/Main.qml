import QtQuick

// The window: top bar (connection, recording), page buttons on the left, the current page, and the
// shared sheets. Plain items and pointer handlers only; no QtQuick.Controls (V5). Pages other than
// the mixer are loaded only while shown, so that the remote controller stays light.
Window {
    id: window
    width: 1280
    height: 800
    minimumWidth: 640
    minimumHeight: 480
    visible: true
    visibility: startFullScreen ? Window.FullScreen : Window.Windowed
    color: Theme.background
    title: "Desktop Capture ミキサー"

    // key: used by --page and the self test; keep: stays loaded while hidden.
    readonly property var pages: [
        { key: "mixer", label: "ミキサー", file: "MixerPage.qml", keep: true },
        { key: "settings", label: "設定", file: "SettingsPage.qml", keep: false },
    ]
    readonly property var pageKeys: pages.map(p => p.key)
    property string currentPage: "mixer"
    readonly property Item currentItem: {
        const l = pageRepeater.count > 0 ? pageRepeater.itemAt(pageKeys.indexOf(currentPage)) : null
        return l ? l.item : null
    }
    readonly property bool sheetShown: numberSheet.shown || choiceSheet.shown || textSheet.shown

    function showPage(key) { if (pageKeys.indexOf(key) >= 0) currentPage = key }
    function stepPage(d) {
        const i = pageKeys.indexOf(currentPage) + d
        if (i >= 0 && i < pageKeys.length) currentPage = pageKeys[i]
    }

    TopBar {
        id: topBar
        anchors { top: parent.top; left: parent.left; right: parent.right }
    }

    NavRail {
        id: nav
        anchors { top: topBar.bottom; bottom: parent.bottom; left: parent.left }
        pages: window.pages
        current: window.currentPage
        onSelected: (key) => window.showPage(key)
    }

    Item {
        id: pageArea
        anchors { top: topBar.bottom; bottom: parent.bottom; left: nav.right; right: parent.right }

        Repeater {
            id: pageRepeater
            model: window.pages
            delegate: Loader {
                required property var modelData
                objectName: "page_" + modelData.key
                anchors.fill: parent
                active: modelData.keep || window.currentPage === modelData.key
                visible: window.currentPage === modelData.key
                source: Qt.resolvedUrl(modelData.file)
            }
        }

        // Sideways swipe to the next / previous page, on pages that do not scroll sideways
        // themselves. The large threshold lets sliders and lists take the finger first.
        Item {
            anchors.fill: parent
            z: 10
            DragHandler {
                id: swipe
                target: null
                yAxis.enabled: false
                dragThreshold: 40
                grabPermissions: PointerHandler.CanTakeOverFromItems
                enabled: !window.sheetShown && window.currentItem !== null && window.currentItem.swipeable === true
                onActiveChanged: {
                    if (active) return
                    if (translation.x < -100) window.stepPage(1)
                    else if (translation.x > 100) window.stepPage(-1)
                }
            }
        }
    }

    // Shown while the engine is not reachable; the last known values stay visible but dimmed.
    Rectangle {
        anchors { top: topBar.bottom; bottom: parent.bottom; left: parent.left; right: parent.right }
        visible: !connection.ready
        z: 50
        color: "#a0101216"
        MouseArea { anchors.fill: parent } // swallow touches while disconnected
        Text {
            anchors.centerIn: parent
            color: Theme.text
            font.pixelSize: 20
            text: connection.statusText + "\n" + connection.address
            horizontalAlignment: Text.AlignHCenter
        }
    }

    NumberSheet { id: numberSheet; objectName: "numberSheet" }
    ChoiceSheet { id: choiceSheet; objectName: "choiceSheet" }
    TextSheet { id: textSheet; objectName: "textSheet" }
    Toast { id: toast; z: 200 }

    Component.onCompleted: {
        Sheets.number = numberSheet
        Sheets.choice = choiceSheet
        Sheets.text = textSheet
        Sheets.toast = toast
        if (startPage) showPage(startPage)
        if (startDemo) demoTimer.start()
    }

    // --demo NAME (screenshots and manual checks): opens a sheet with sample content once the
    // first snapshot is in (a snapshot closes the track menus).
    Timer {
        id: demoTimer
        interval: 300
        repeat: true
        onTriggered: if (connection.ready) { stop(); window.openDemo(startDemo) }
    }
    function openDemo(name) {
        if (name === "number")
            numberSheet.open({ title: "Mic の音量", value: -6, unit: "dB", min: -144, max: 12, decimals: 1 }, null)
        else if (name === "text")
            textSheet.open("トラックの名前", "Mic", null)
        else if (currentItem && currentItem.openDemo)
            currentItem.openDemo(name)
    }

    Connections {
        target: tracks
        function onFailed(message) { toast.show(message) }
    }
    Connections {
        target: master
        function onFailed(message) { toast.show(message) }
    }
    Connections {
        target: params
        function onFailed(message) { toast.show(message) }
    }
    Connections {
        target: recorder
        function onFailed(message) { toast.show("録画: " + message) }
    }
}
