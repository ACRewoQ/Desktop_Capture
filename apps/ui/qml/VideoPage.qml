import QtQuick
import DcUi.Params

// The video mixer (protocol.md 4.5): scenes and their layers, the sources, and which mix goes to
// which output. capd does the capture and compositing; this page only edits the tree.
Item {
    id: page
    // The canvas takes sideways drags, and so do sliders in the sheets.
    readonly property bool swipeable: view !== "scenes" && !layerSheet.shown && !sourceSheet.shown
    property string view: "scenes"
    property int mix: 1

    ParamList { id: mixes; prefix: "/video/mix" }
    // Keep a mix that exists selected (the first one when ours is removed).
    Connections {
        target: mixes
        function onCountChanged() { page.fixMix() }
    }
    Component.onCompleted: fixMix()
    function fixMix() {
        if (mixes.count === 0) mix = 0
        else if (mixes.rowOf(mix) < 0) mix = mixes.numAt(0)
    }

    function openDemo(name) {
        if (name === "layer") { view = "scenes"; layerSheet.open(1, 1) }
        else if (name === "source") { view = "sources"; sourceSheet.open(1) }
        else if (name === "outputs") view = "outputs"
        else if (name === "sources") view = "sources"
        else if (name === "textsource") {
            view = "sources"
            params.call("/video/source/add", { kind: "text", name: "テロップ" }, (ok, n) => { if (ok) sourceSheet.open(n) })
        }
    }

    Item {
        id: header
        anchors { top: parent.top; left: parent.left; right: parent.right; margins: 12 }
        height: Theme.touch
        SegmentTabs {
            id: tabs
            objectName: "videoTabs"
            model: [{ key: "scenes", label: "シーン" }, { key: "sources", label: "ソース" }, { key: "outputs", label: "出力先" }]
            current: page.view
            onSelected: (key) => page.view = key
        }
        MenuButton {
            objectName: "mixPick"
            visible: page.view === "scenes" && (mixes.count > 1 || page.width > 760)
            anchors { right: parent.right; left: tabs.right; leftMargin: 12 }
            height: Theme.touch
            fontSize: 15
            label: (mixes.count, mixName.value, "ミックス: " + Video.mixName(page.mix)) + (mixes.count > 1 ? "  ▾" : "")
            active: mixes.count > 1
            onTapped: Sheets.choice.open("編集するミックス",
                                         Video.mixChoices(false).map(c => Object.assign({ selected: c.value === page.mix }, c)),
                                         c => page.mix = c.value)
            Param { id: mixName; path: page.mix > 0 ? "/video/mix/" + page.mix + "/name" : "" }
        }
    }

    Loader {
        id: body
        anchors { top: header.bottom; topMargin: 12; left: parent.left; right: parent.right; bottom: parent.bottom }
        sourceComponent: page.view === "scenes" ? scenesView : page.view === "sources" ? sourcesView : outputsView
    }
    Component {
        id: scenesView
        VideoScenes {
            mix: page.mix
            onLayerRequested: (scene, layer) => layerSheet.open(scene, layer)
        }
    }
    Component {
        id: sourcesView
        VideoSources { onSourceRequested: (n) => sourceSheet.open(n) }
    }
    Component {
        id: outputsView
        VideoOutputs { onMixPicked: (n) => { page.mix = n; page.view = "scenes" } }
    }

    LayerSheet { id: layerSheet; objectName: "layerSheet" }
    SourceSheet { id: sourceSheet; objectName: "sourceSheet" }
}
