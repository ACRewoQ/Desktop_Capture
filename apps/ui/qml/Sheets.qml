pragma Singleton
import QtQuick

// The shared sheets and the toast, set by Main.qml so that every page can open them:
// Sheets.number.open({...}, callback), Sheets.choice.open(title, choices, callback),
// Sheets.text.open(title, text, callback[, multiline]), Sheets.color.open({...}, callback),
// Sheets.toast.show(message).
QtObject {
    property Item number: null
    property Item choice: null
    property Item text: null
    property Item color: null
    property Item toast: null

    // Sheets shown right now, in any page (the page swipe waits until there are none).
    property int openCount: 0

    // What a shown sheet covers is disabled (Sheet.qml): the sheet's MouseAreas take the press, but
    // pointer handlers below (TapHandler, DragHandler, PointHandler) still see it and can react
    // too. Counted per item, so that two sheets can cover the same item; the Binding puts the
    // item's own enabled (value or binding) back when the last of them closes.
    property var blocked: new Map()
    property Component blocker: Component { Binding { property: "enabled"; value: false } }

    function block(target) {
        const b = blocked.get(target)
        if (b) { b.count++; return }
        blocked.set(target, { count: 1, binding: blocker.createObject(target, { target: target }) })
    }
    function unblock(target) {
        const b = blocked.get(target)
        if (!b || --b.count > 0) return
        blocked.delete(target)
        if (b.binding) { b.binding.when = false; b.binding.destroy() }
    }
}
