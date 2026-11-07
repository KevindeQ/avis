// AVIS_VISUALIZER_UI_TESTS (docs/tasks/12-visualizer.md): the Visualizer's Qt Test classes in one executable. The
// arguments go to each class, so -functions, a function name or -o work as with a single Qt Test.

#include "ui_tests.h"

#include <QGuiApplication>
#include <QTest>

#include <memory>

int main(int argc, char* argv[])
{
    QGuiApplication application{ argc, argv };

    int status = 0;
    for (const auto make_test :
         { &avis::visualizer_ui_tests::make_qt_interop_test, &avis::visualizer_ui_tests::make_viewport_renderer_test })
    {
        const std::unique_ptr<QObject> test = make_test();
        status |= QTest::qExec(test.get(), argc, argv);
    }
    return status;
}
