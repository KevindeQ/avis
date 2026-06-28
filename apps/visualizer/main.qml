import Avis

import QtQuick

Window {
    id: root

    title: "Autonomous Vehicle Visualizer"

    visible: true
    minimumWidth: 250
    minimumHeight: 250

    SceneProvider { }

    SimulatorDriver {
        id: sim_driver
    }

    Timer {
        id: timer_simulation_tick

        running: true
        repeat: true
        // Set the interval to 0 so it is triggered once the event queue is
        // empty. This allows that is called in onTriggered to have control
        // over the timing.
        interval: 0

        onTriggered: sim_driver.update()
    }
}