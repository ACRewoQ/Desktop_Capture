import QtQuick
import DcUi.Params

// Scenes of one mix: the scene list (tap = next scene, i.e. preview), TAKE with the transition,
// the canvas of the scene being edited and its layers. Wide screens show the three side by side;
// narrow ones (remote held upright) stack them in one scrolling column.
Item {
    id: root
    property int mix: 0
    property int selected: 0      // scene chosen while there is no mix
    property int selectedLayer: 0
    readonly property string mixBase: mix > 0 ? "/video/mix/" + mix : ""
    readonly property bool wide: width >= 900
    readonly property int editScene: {
        if (preview.value > 0 && scenes.rowOf(preview.value) >= 0) return preview.value
        if (selected > 0 && scenes.rowOf(selected) >= 0) return selected
        if (program.value > 0 && scenes.rowOf(program.value) >= 0) return program.value
        return scenes.count > 0 ? scenes.numAt(0) : 0
    }

    signal layerRequested(int scene, int layer)

    onEditSceneChanged: selectedLayer = 0

    Param { id: program; path: root.mixBase ? root.mixBase + "/program_scene" : "" }
    Param { id: preview; path: root.mixBase ? root.mixBase + "/preview_scene" : "" }
    Param { id: transition; path: root.mixBase ? root.mixBase + "/transition/type" : "" }
    Param { id: duration; path: root.mixBase ? root.mixBase + "/transition/duration_ms" : "" }
    ParamList { id: scenes; prefix: "/video/scene"; orderBy: "position" }
    ParamList { id: layers; prefix: root.editScene > 0 ? "/video/scene/" + root.editScene + "/layer" : ""; orderBy: "position" }

    function pickScene(n) {
        if (preview.exists) preview.set(n)
        selected = n
    }
    function sceneMenu(n, row) {
        const name = Video.sceneName(n)
        const list = [
            { label: "本番に出す", detail: "トランジションで切り替えます", value: "program", disabled: !program.exists || program.value === n },
            { label: "名前を変える", value: "rename" },
            { label: "上へ", value: "up", disabled: row === 0 },
            { label: "下へ", value: "down", disabled: row === scenes.count - 1 },
            { label: "削除", detail: "レイヤーも消えます", value: "remove" },
        ]
        Sheets.choice.open(name, list, c => {
            if (c.value === "program") program.set(n)
            else if (c.value === "rename")
                Sheets.text.open("シーンの名前", params.value("/video/scene/" + n + "/name"),
                                 t => params.set("/video/scene/" + n + "/name", t))
            else if (c.value === "up") params.call("/video/scene/move", { scene: n, position: row })
            else if (c.value === "down") params.call("/video/scene/move", { scene: n, position: row + 2 })
            else if (c.value === "remove")
                Sheets.choice.open(name + " を削除しますか",
                                   [{ label: "削除する", value: true }, { label: "やめる", value: false }],
                                   d => { if (d.value) params.call("/video/scene/remove", { scene: n }) })
        })
    }
    function addLayer() {
        const s = editScene
        const list = Video.sourceChoices()
        if (list.length === 0) list.push({ label: "ソースがありません", detail: "「ソース」タブで追加できます", disabled: true })
        Sheets.choice.open("レイヤーに出すソース", list, c => {
            if (!c.value) return
            params.call("/video/scene/" + s + "/layer/add", { source: c.value }, (ok, n) => { if (ok) root.selectedLayer = n })
        })
    }

    // TAKE and the transition.
    Row {
        id: takeBar
        anchors { top: parent.top; left: parent.left; leftMargin: 12 }
        spacing: 8
        height: Theme.touch
        visible: root.mix > 0
        MenuButton {
            objectName: "take"
            width: 132
            height: Theme.touch
            color: Theme.record
            label: "TAKE"
            active: preview.value > 0
            onTapped: params.call(root.mixBase + "/take")
        }
        ParamSegments {
            width: 160
            path: root.mixBase ? root.mixBase + "/transition/type" : ""
            labels: ({ cut: "カット", fade: "フェード" })
        }
        ParamNumber {
            width: 110
            visible: transition.value === "fade"
            path: root.mixBase ? root.mixBase + "/transition/duration_ms" : ""
            title: "フェードの長さ"
            unit: "ms"
        }
    }

    Flickable {
        id: flick
        objectName: "scenesFlick"
        anchors { top: takeBar.visible ? takeBar.bottom : parent.top; topMargin: takeBar.visible ? 12 : 0
                  left: parent.left; right: parent.right; bottom: parent.bottom }
        clip: true
        boundsBehavior: Flickable.StopAtBounds
        interactive: !root.wide && !canvas.dragging && !(canvas.touching && root.selectedLayer > 0)
        contentHeight: root.wide ? height : layerPane.y + layerPane.height + 16

        // Scene list.
        Item {
            id: scenePane
            x: 12
            y: root.wide ? 0 : canvasPane.height + 12
            width: root.wide ? 220 : flick.width - 24
            height: root.wide ? flick.height : sceneFlow.height + addScene.height + 8
            Flickable {
                id: sceneScroll
                anchors { top: parent.top; left: parent.left; right: parent.right; bottom: addScene.top; bottomMargin: 8 }
                contentHeight: sceneFlow.height
                interactive: root.wide && contentHeight > height
                boundsBehavior: Flickable.StopAtBounds
                clip: true
                Flow {
                    id: sceneFlow
                    width: sceneScroll.width
                    spacing: 6
                    Repeater {
                        model: scenes
                        delegate: Rectangle {
                            id: tile
                            required property int num
                            required property int index
                            readonly property bool onAir: program.value === num
                            readonly property bool next: preview.value === num
                            readonly property bool editing: root.editScene === num
                            objectName: "scene_" + num
                            width: root.wide ? sceneFlow.width : Math.max(150, (sceneFlow.width - 6 * 2) / 3)
                            height: 56
                            radius: Theme.radius
                            color: tileTap.pressed ? Theme.controlPressed : Theme.control
                            border.width: onAir || next || editing ? 2 : 0
                            border.color: onAir ? Theme.record : next ? Theme.ok : Theme.accent
                            Param { id: sceneName; path: "/video/scene/" + tile.num + "/name" }
                            Column {
                                anchors { left: parent.left; right: more.left; leftMargin: 10; verticalCenter: parent.verticalCenter }
                                Text {
                                    width: parent.width
                                    elide: Text.ElideRight
                                    color: "white"
                                    font.pixelSize: 15
                                    text: sceneName.value || "シーン " + tile.num
                                }
                                Text {
                                    width: parent.width
                                    visible: text.length > 0
                                    font.pixelSize: 11
                                    font.bold: true
                                    color: tile.onAir ? Theme.record : Theme.ok
                                    text: tile.onAir && tile.next ? "本番・次" : tile.onAir ? "本番" : tile.next ? "次" : ""
                                }
                            }
                            TapHandler { id: tileTap; onTapped: root.pickScene(tile.num); onLongPressed: root.sceneMenu(tile.num, tile.index) }
                            MenuButton {
                                id: more
                                objectName: "sceneMenu_" + tile.num
                                anchors { right: parent.right; top: parent.top; bottom: parent.bottom; margins: 4 }
                                width: 40
                                height: 48
                                color: "transparent"
                                label: "…"
                                onTapped: root.sceneMenu(tile.num, tile.index)
                            }
                        }
                    }
                }
            }
            MenuButton {
                id: addScene
                objectName: "addScene"
                anchors { left: parent.left; right: parent.right; bottom: parent.bottom }
                height: Theme.touch
                label: "+ シーン"
                active: scenes.count < 32
                onTapped: params.call("/video/scene/add", {}, (ok, n) => { if (ok) root.pickScene(n) })
            }
        }

        // The canvas of the scene being edited.
        Item {
            id: canvasPane
            x: root.wide ? scenePane.x + scenePane.width + 12 : 12
            y: 0
            width: root.wide ? flick.width - x - layerPane.width - 24 : flick.width - 24
            height: root.wide ? flick.height : Math.min(width * 9 / 16, 420) + 30
            Text {
                id: canvasTitle
                width: parent.width
                elide: Text.ElideRight
                color: Theme.subText
                font.pixelSize: 13
                text: root.editScene > 0 ? (scenes.count, "編集中: " + Video.sceneName(root.editScene)
                                            + (program.value === root.editScene ? "（本番に出ています）" : ""))
                                         : "シーンがありません"
            }
            LayerCanvas {
                id: canvas
                objectName: "layerCanvas"
                anchors { top: canvasTitle.bottom; topMargin: 6; left: parent.left; right: parent.right; bottom: parent.bottom }
                scene: root.editScene
                mix: root.mix
                model: layers
                selected: root.selectedLayer
                onSelect: (layer) => root.selectedLayer = layer
                onOpen: (layer) => root.layerRequested(root.editScene, layer)
            }
        }

        // Layers of that scene, front first.
        Item {
            id: layerPane
            x: root.wide ? flick.width - width - 12 : 12
            y: root.wide ? 0 : scenePane.y + scenePane.height + 16
            width: root.wide ? 300 : flick.width - 24
            height: root.wide ? flick.height : layerHead.height + layerColumn.height + addLayerButton.height + 16
            Text {
                id: layerHead
                color: Theme.subText
                font.pixelSize: 13
                text: "レイヤー（上ほど手前）"
            }
            Flickable {
                id: layerScroll
                anchors { top: layerHead.bottom; topMargin: 6; left: parent.left; right: parent.right; bottom: addLayerButton.top; bottomMargin: 8 }
                contentHeight: layerColumn.height
                interactive: root.wide && contentHeight > height
                boundsBehavior: Flickable.StopAtBounds
                clip: true
                Column {
                    id: layerColumn
                    width: layerScroll.width
                    spacing: 6
                    Repeater {
                        model: layers
                        delegate: Rectangle {
                            id: layerRow
                            required property int num
                            required property string base
                            required property int index
                            objectName: "layer_" + num
                            width: layerColumn.width
                            height: 52
                            radius: Theme.radius
                            color: rowTap.pressed ? Theme.controlPressed : Theme.control
                            border.width: root.selectedLayer === num ? 2 : 0
                            border.color: Theme.accent
                            Param { id: lVisible; path: layerRow.base + "/visible" }
                            Param { id: lName; path: layerRow.base + "/name" }
                            Param { id: lSource; path: layerRow.base + "/source" }
                            Param { id: lState; path: lSource.value > 0 ? "/video/source/" + lSource.value + "/state" : "" }
                            ToggleButton {
                                id: eye
                                objectName: "layerVisible_" + layerRow.num
                                anchors { left: parent.left; top: parent.top; bottom: parent.bottom; margins: 4 }
                                width: 52
                                height: 44
                                fontSize: 12
                                label: lVisible.value ? "表示" : "隠す"
                                on: lVisible.value === true
                                onColor: Theme.accentDim
                                onTapped: lVisible.set(!lVisible.value)
                            }
                            Column {
                                anchors { left: eye.right; right: buttons.left; leftMargin: 8; rightMargin: 4; verticalCenter: parent.verticalCenter }
                                opacity: lVisible.value ? 1 : 0.5
                                Text {
                                    width: parent.width
                                    elide: Text.ElideRight
                                    color: "white"
                                    font.pixelSize: 14
                                    text: lName.value || (lSource.value, lState.value, Video.sourceName(lSource.value))
                                }
                                Text {
                                    width: parent.width
                                    elide: Text.ElideRight
                                    font.pixelSize: 11
                                    color: Video.stateColor(lState.value)
                                    text: lSource.value > 0 ? Video.stateText(lState.value) : "ソースなし"
                                }
                            }
                            TapHandler { id: rowTap; onTapped: root.selectedLayer = layerRow.num }
                            Row {
                                id: buttons
                                anchors { right: parent.right; top: parent.top; bottom: parent.bottom; margins: 4 }
                                spacing: 2
                                MenuButton {
                                    width: 38; height: 44; fontSize: 14
                                    label: "↑"
                                    active: layerRow.index > 0
                                    onTapped: params.call("/video/scene/" + root.editScene + "/layer/move",
                                                          { layer: layerRow.num, position: layerRow.index })
                                }
                                MenuButton {
                                    width: 38; height: 44; fontSize: 14
                                    label: "↓"
                                    active: layerRow.index < layers.count - 1
                                    onTapped: params.call("/video/scene/" + root.editScene + "/layer/move",
                                                          { layer: layerRow.num, position: layerRow.index + 2 })
                                }
                                MenuButton {
                                    objectName: "layerOpen_" + layerRow.num
                                    width: 44; height: 44; fontSize: 14
                                    label: "設定"
                                    onTapped: root.layerRequested(root.editScene, layerRow.num)
                                }
                            }
                        }
                    }
                }
            }
            MenuButton {
                id: addLayerButton
                objectName: "addLayer"
                anchors { left: parent.left; right: parent.right; bottom: parent.bottom }
                height: Theme.touch
                label: "+ レイヤー"
                active: root.editScene > 0 && layers.count < 32
                onTapped: root.addLayer()
            }
        }
    }
}
