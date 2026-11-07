#ifndef AVIS_VISUALIZER_UI_VIEWPORT_RENDERER_H
#define AVIS_VISUALIZER_UI_VIEWPORT_RENDERER_H

// The Qt side of the viewport (docs/design/12-visualizer.md §3.2). The one place where the Visualizer touches Qt's
// D3D12 objects (12 §3.2, 12.16's grep check). For now it reads Qt's adapter (12 §3.2 step 2); the
// QQuickRhiItemRenderer that imports the shared frame ring joins in 12.5.

#include "avis/rhi/types.h"

#include <QString>

#include <optional>

class QRhi;

namespace avis::visualizer::ui
{
    // The adapter Qt Quick renders on
    struct qt_adapter
    {
        rhi::adapter_luid luid;
        QString name; // QRhi::driverInfo().deviceName, for the log
    };

    // In main(), before Qt creates its device. D3D12 removes every device in the process when the debug layer or
    // GPU-based validation is turned on after one exists, and D3D12CreateDevice returns the process's existing device
    // for an adapter. So the settings the Avis RHI asks for later (render.debug_layer, render.gpu_validation) are
    // applied here first, and the RHI's own calls then change nothing. Returns why the debug layer is unavailable,
    // when it is; the RHI then fails with the same reason.
    std::optional<QString> enable_d3d12_debugging(bool debug_layer, bool gpu_based_validation);

    // On the render thread, in QQuickWindow::sceneGraphInitialized: Qt's adapter, from QRhiD3D12NativeHandles. Empty
    // when Qt's QRhi isn't the D3D12 backend.
    std::optional<qt_adapter> read_qt_adapter(QRhi& rhi);
} // namespace avis::visualizer::ui

#endif
