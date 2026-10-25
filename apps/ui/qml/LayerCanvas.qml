import QtQuick
import DcUi.Params

// The layout of one scene on the mix's canvas: every layer as a box (no video here; capd composites).
// A tap selects the front-most layer under the finger, a double tap opens its settings. The selected
// layer moves with a drag and resizes from its corner; edges snap to the canvas edges and centre.
Item {
    id: root
    property int scene: 0
    property int mix: 0
    property var model: null       // ParamList of /video/scene/<scene>/layer, front first
    property int selected: 0
    property bool dragging: false
    // A finger is on the canvas: the page around it must not scroll (narrow layout) while a layer
    // is selected, or it would take the drag from the layer.
    readonly property bool touching: finger.active
    readonly property string sceneBase: "/video/scene/" + scene
    readonly property real snapPx: 10

    signal select(int layer)
    signal open(int layer)

    Param { id: mixWidth; path: root.mix > 0 ? "/video/mix/" + root.mix + "/width" : "" }
    Param { id: mixHeight; path: root.mix > 0 ? "/video/mix/" + root.mix + "/height" : "" }
    Param { id: mixBackground; path: root.mix > 0 ? "/video/mix/" + root.mix + "/background" : "" }
    readonly property real aspect: mixWidth.value > 0 && mixHeight.value > 0 ? mixWidth.value / mixHeight.value : 16 / 9

    // Moves v (a fraction) so that one of the edges (v + offsets[i]) lands on 0, 1/2 or 1 when it is
    // within snapPx of it.
    function snap(v, offsets, size) {
        const limit = snapPx / size
        let best = limit, delta = 0
        for (const o of offsets)
            for (const t of [0, 0.5, 1]) {
                const d = t - (v + o)
                if (Math.abs(d) < best) { best = Math.abs(d); delta = d }
            }
        return v + delta
    }
    PointHandler { id: finger }

    function layerAt(px, py) {
        if (!model) return 0
        const fx = px / stage.width, fy = py / stage.height
        for (let i = 0; i < model.count; ++i) {
            const b = sceneBase + "/layer/" + model.numAt(i)
            const x = params.value(b + "/x"), y = params.value(b + "/y")
            const w = params.value(b + "/width"), h = params.value(b + "/height")
            if (fx >= x && fx <= x + w && fy >= y && fy <= y + h) return model.numAt(i)
        }
        return 0
    }

    Rectangle {
        id: stage
        objectName: "stage"
        readonly property real fitWidth: Math.min(root.width, root.height * root.aspect)
        width: fitWidth
        height: fitWidth / root.aspect
        anchors.horizontalCenter: parent.horizontalCenter
        color: mixBackground.exists ? Video.argb(mixBackground.value) : "black"
        border.color: Theme.border
        clip: true

        // Thirds and centre lines, faint, to help placing things.
        Repeater {
            model: [1 / 2]
            delegate: Item {
                required property var modelData
                anchors.fill: parent
                Rectangle { x: stage.width * modelData; width: 1; height: stage.height; color: "#30ffffff" }
                Rectangle { y: stage.height * modelData; height: 1; width: stage.width; color: "#30ffffff" }
            }
        }

        TapHandler {
            onTapped: (point) => root.select(root.layerAt(point.position.x, point.position.y))
            onDoubleTapped: (point) => { const n = root.layerAt(point.position.x, point.position.y); if (n > 0) root.open(n) }
        }

        Repeater {
            model: root.model
            delegate: Rectangle {
                id: box
                required property int num
                required property string base
                required property int index
                readonly property bool isSelected: root.selected === num
                objectName: "box_" + num
                Param { id: lx; path: box.base + "/x" }
                Param { id: ly; path: box.base + "/y" }
                Param { id: lw; path: box.base + "/width" }
                Param { id: lh; path: box.base + "/height" }
                Param { id: lVisible; path: box.base + "/visible" }
                Param { id: lOpacity; path: box.base + "/opacity" }
                Param { id: lName; path: box.base + "/name" }
                Param { id: lSource; path: box.base + "/source" }
                Param { id: sKind; path: lSource.value > 0 ? "/video/source/" + lSource.value + "/kind" : "" }
                Param { id: sColor; path: sKind.value === "color" ? "/video/source/" + lSource.value + "/color/value" : "" }
                Param { id: sText; path: sKind.value === "text" ? "/video/source/" + lSource.value + "/text/content" : "" }

                x: stage.width * lx.value
                y: stage.height * ly.value
                width: Math.max(2, stage.width * lw.value)
                height: Math.max(2, stage.height * lh.value)
                z: root.model ? root.model.count - index : 0
                opacity: lVisible.value ? 1 : 0.3
                color: sKind.value === "color" ? Video.argb(sColor.value) : Qt.rgba(0.31, 0.76, 0.97, 0.12 + 0.18 * lOpacity.value)
                border.width: isSelected ? 2 : 1
                border.color: isSelected ? Theme.accent : "#80ffffff"

                Text {
                    anchors { fill: parent; margins: 6 }
                    color: "white"
                    style: Text.Outline
                    styleColor: "black"
                    font.pixelSize: Math.max(10, Math.min(16, box.height / 5))
                    elide: Text.ElideRight
                    wrapMode: Text.Wrap
                    text: (lSource.value, sKind.value, lName.value || Video.sourceName(lSource.value))
                          + (sKind.value === "text" && sText.value ? "\n" + sText.value : "")
                }

                DragHandler {
                    id: move
                    target: null
                    enabled: box.isSelected
                    property real x0
                    property real y0
                    onActiveChanged: {
                        root.dragging = active
                        if (active) { x0 = lx.value; y0 = ly.value }
                    }
                    onTranslationChanged: {
                        if (!active) return
                        const w = lw.value, h = lh.value
                        lx.set(root.snap(x0 + translation.x / stage.width, [0, w / 2, w], stage.width))
                        ly.set(root.snap(y0 + translation.y / stage.height, [0, h / 2, h], stage.height))
                    }
                }

                // Resize from the bottom-right corner (a 44 px touch area around a small knob).
                Item {
                    objectName: "resize_" + box.num
                    visible: box.isSelected
                    width: Theme.touch
                    height: Theme.touch
                    x: box.width - width / 2
                    y: box.height - height / 2
                    z: 2
                    Rectangle {
                        anchors.centerIn: parent
                        width: 18; height: 18; radius: 3
                        color: Theme.accent
                        border.color: "white"
                    }
                    DragHandler {
                        target: null
                        property real w0
                        property real h0
                        onActiveChanged: {
                            root.dragging = active
                            if (active) { w0 = lw.value; h0 = lh.value }
                        }
                        onTranslationChanged: {
                            if (!active) return
                            const x = lx.value, y = ly.value
                            const right = root.snap(x + w0 + translation.x / stage.width, [0], stage.width)
                            const bottom = root.snap(y + h0 + translation.y / stage.height, [0], stage.height)
                            lw.set(Math.max(0.02, right - x))
                            lh.set(Math.max(0.02, bottom - y))
                        }
                    }
                }
            }
        }
    }

    Text {
        anchors.centerIn: stage
        visible: !root.model || root.model.count === 0
        color: Theme.subText
        font.pixelSize: 14
        text: root.scene > 0 ? "レイヤーがありません（「+ レイヤー」で追加）" : ""
    }
}
