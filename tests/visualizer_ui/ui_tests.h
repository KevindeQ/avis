#ifndef AVIS_TESTS_VISUALIZER_UI_UI_TESTS_H
#define AVIS_TESTS_VISUALIZER_UI_UI_TESTS_H

// The Qt Test classes of AVIS_VISUALIZER_UI_TESTS (docs/tasks/12-visualizer.md), run one after another by main.cpp

#include <QObject>

#include <memory>

namespace avis::visualizer_ui_tests
{
    // 12.2: afterFrameEnd's timing and createFrom on a shared texture, on Qt's D3D12 device
    std::unique_ptr<QObject> make_qt_interop_test();

    // 12.5: ui::viewport_item and its renderer, fed by the shared frame ring
    std::unique_ptr<QObject> make_viewport_renderer_test();
} // namespace avis::visualizer_ui_tests

#endif
