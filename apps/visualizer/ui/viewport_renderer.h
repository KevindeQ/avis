#ifndef AVIS_VISUALIZER_UI_VIEWPORT_RENDERER_H
#define AVIS_VISUALIZER_UI_VIEWPORT_RENDERER_H

// The Qt side of the viewport (docs/design/12-visualizer.md §3.2, §3.3). The one place where the Visualizer touches
// Qt's D3D12 objects (12 §3.2, 12.16's grep check): the debug layer before Qt's device, Qt's adapter (step 2), and the
// renderer of ui/viewport_item, which opens the shared frame ring's handles on Qt's device and draws its frames
// (step 5).

#include "avis/rhi/types.h"

#include <QString>

#include <cstdint>
#include <optional>

class QQuickRhiItemRenderer;
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

    // What the viewport shows before Avis has published a frame, as 0xAARRGGBB
    inline constexpr std::uint32_t viewport_clear_colour = 0xFF20'2020;

    // The renderer of viewport_item, on Qt's render thread (12 §3.3 rules 3-8). Each render():
    // - takes the ring of shared_frame_rings::qt_current(). A new generation is attached (qt_attach), its handles
    //   opened on Qt's device with OpenSharedHandle and its textures wrapped with QRhiTexture::createFrom in COMMON.
    //   The previous ring stays on screen until the new one has a frame; then Qt detaches from it (qt_detach) and
    //   signals that use on its `consumed`.
    // - picks the newest frame (qt_pick), makes Qt's queue wait on `produced` for it, draws it with the qt/ blit
    //   shaders, and hands the texture back in COMMON (a native transition, then setNativeLayout)
    // - signals the pick's use on `consumed` from QQuickWindow::afterFrameEnd, after Qt has submitted the frame (rule
    // 4) Without a ring or a frame, it draws viewport_clear_colour. Destroyed with the scene graph, it detaches and
    // waits for Qt's queue to finish with the rings it opened.
    QQuickRhiItemRenderer* make_viewport_renderer();
} // namespace avis::visualizer::ui

#endif
