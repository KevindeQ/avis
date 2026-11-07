import QtQuick
import QtQuick.Dialogs

Window {
    id: root

    title: "Autonomous Vehicle Visualizer"

    visible: true
    minimumWidth: 250
    minimumHeight: 250

    // Reading it creates the driver singleton, which starts graphics and the renderer
    readonly property string startupError: VisualizerDriver.startupError

    Viewport {
        id: viewport
        anchors.fill: parent
    }

    Timer {
        id: heartbeat_timer

        running: root.startupError === ""
        repeat: true
        interval: 16

        onTriggered: VisualizerDriver.tick()
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
        if (root.startupError !== "") {
            startup_error_dialog.open()
        }
    }
}
