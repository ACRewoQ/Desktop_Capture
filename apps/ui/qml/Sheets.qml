pragma Singleton
import QtQuick

// The shared sheets and the toast, set by Main.qml so that every page can open them:
// Sheets.number.open({...}, callback), Sheets.choice.open(title, choices, callback),
// Sheets.text.open(title, text, callback), Sheets.toast.show(message).
QtObject {
    property Item number: null
    property Item choice: null
    property Item text: null
    property Item toast: null
}
