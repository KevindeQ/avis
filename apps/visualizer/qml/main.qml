import QtCore
import QtQuick
import QtQuick.Dialogs

Window {
    id: root

    title: "Autonomous Vehicle Visualizer"

    visible: true
    width: 1600
    height: 900
    minimumWidth: 250
    minimumHeight: 250

    // Set when graphics fail to start, which happens once Qt's scene graph is up (12 §3.2)
    readonly property string startupError: VisualizerDriver.startupError

    onStartupErrorChanged: {
        if (root.startupError !== "") {
            startup_error_dialog.open()
        }
    }

    // Window geometry is per-user UI state, kept with QSettings (12 §8); the panel layout joins it with the panels
    // (12.13). Restored when the window is created, saved when it closes. A maximised window keeps the normal
    // geometry it had before, so un-maximising after a restart goes back to it.
    Settings {
        id: window_settings

        category: "window"

        property int x: 0
        property int y: 0
        property int width: 0 // 0 = never saved
        property int height: 0
        property bool maximized: false
    }

    onClosing: {
        window_settings.maximized = root.visibility === Window.Maximized
        if (!window_settings.maximized && root.visibility !== Window.Minimized) {
            window_settings.x = root.x
            window_settings.y = root.y
            window_settings.width = root.width
            window_settings.height = root.height
        }
    }

    Viewport {
        id: viewport
        anchors.fill: parent
    }

    // A startup error (12 §7, 12a §5): shown modally, then the Visualizer exits
    MessageDialog {
        id: startup_error_dialog

        title: "The Visualizer could not start"
        text: root.startupError
        informativeText: "The log has the details: " + VisualizerDriver.logFile
        buttons: MessageDialog.Ok
        modality: Qt.ApplicationModal

        onAccepted: Qt.exit(1)
        onRejected: Qt.exit(1)
    }

    Component.onCompleted: {
        if (window_settings.width > 0 && window_settings.height > 0) {
            root.x = window_settings.x
            root.y = window_settings.y
            root.width = window_settings.width
            root.height = window_settings.height
        }
        if (window_settings.maximized) {
            root.showMaximized()
        }
        if (root.startupError !== "") {
            startup_error_dialog.open()
        }
    }
}
