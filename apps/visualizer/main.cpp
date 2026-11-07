#include "log_location.h"
#include "visualizer_configuration.h"
#include "visualizer_driver.h"

#include "avis/log.h"
#include "avis/runtime.h"

#include <QGuiApplication>
#include <QQmlApplicationEngine>
#include <QQuickWindow>
#include <QSGRendererInterface>

#include <cstdio>
#include <filesystem>
#include <format>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// The command line (CommandLineToArgvW) and the startup message box
#ifndef NOMINMAX
#    define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>

// After Windows.h
#include <shellapi.h>

namespace
{
    namespace visualizer = avis::visualizer;

    // The command line's arguments as UTF-16, without the program name: argv is in the ANSI code page, which can't
    // hold every path
    std::vector<std::wstring> command_line_arguments()
    {
        int count = 0;
        wchar_t** const arguments = CommandLineToArgvW(GetCommandLineW(), &count);
        std::vector<std::wstring> result;
        if (arguments != nullptr)
        {
            for (int index = 1; index < count; ++index)
            {
                result.emplace_back(arguments[index]);
            }
            LocalFree(arguments);
        }
        return result;
    }

    std::filesystem::path executable_directory()
    {
        std::wstring path(MAX_PATH, L'\0');
        for (;;)
        {
            const DWORD length = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
            if (length == 0)
            {
                return std::filesystem::current_path();
            }
            if (length < path.size())
            {
                path.resize(length);
                return std::filesystem::path{ path }.parent_path();
            }
            path.resize(path.size() * 2);
        }
    }

    std::string utf8(const std::filesystem::path& path)
    {
        const std::u8string text = path.u8string();
        return std::string{ reinterpret_cast<const char*>(text.data()), text.size() };
    }

    // An error before the QML engine exists: on stderr, and in a message box since the Visualizer has no console
    void report_startup_error(const std::string_view message)
    {
        std::fputs(std::format("avis: {}\n", message).c_str(), stderr);

        const int length =
            MultiByteToWideChar(CP_UTF8, 0, message.data(), static_cast<int>(message.size()), nullptr, 0);
        std::wstring text(static_cast<std::size_t>(length), L'\0');
        MultiByteToWideChar(CP_UTF8, 0, message.data(), static_cast<int>(message.size()), text.data(), length);
        MessageBoxW(nullptr, text.c_str(), L"The Visualizer could not start", MB_OK | MB_ICONERROR);
    }
} // namespace

int main(int argc, char* argv[])
{
    // The command line and visualizer.yaml come first: the configuration sets up the runtime (01 §8)
    const std::vector<std::wstring> arguments = command_line_arguments();
    const avis::result<visualizer::command_line_options> options = visualizer::parse_command_line(arguments);
    if (!options)
    {
        report_startup_error(options.error().message);
        return 1;
    }
    const std::filesystem::path configuration_path =
        options->configuration.value_or(visualizer::default_configuration_path());

    avis::result<visualizer::loaded_configuration> loaded =
        visualizer::load_visualizer_configuration(configuration_path);
    if (!loaded)
    {
        report_startup_error(std::format("the configuration could not be loaded: {}", loaded.error().message));
        return 1;
    }

    // Logging, thread pool, IO and the parallel pool, for the whole of main
    avis::runtime_configuration runtime_configuration = loaded->configuration.runtime;
    runtime_configuration.application_threads = 1; // simulation; the renderer is a sender chain
    visualizer_log::file_path() = visualizer::resolve_log_file(loaded->configuration, executable_directory());
    runtime_configuration.logging.file = visualizer_log::file_path();

    std::optional<avis::runtime> runtime;
    try
    {
        runtime.emplace(runtime_configuration);
    }
    catch (const avis::runtime_error& error)
    {
        report_startup_error(std::format("failed to start the runtime: {}", error.error()));
        return 1;
    }

    const auto log = avis::log::get("ui");
    if (loaded->created)
    {
        if (loaded->save_failure)
        {
            log->warn(
                "no configuration at {}: using the defaults, which could not be written: {}",
                utf8(configuration_path),
                loaded->save_failure->message);
        }
        else
        {
            log->info("no configuration at {}: wrote the defaults there", utf8(configuration_path));
        }
    }
    else
    {
        log->info("loaded the configuration {}", utf8(configuration_path));
    }

    // Force usage of Direct3D12 for rendering
    QQuickWindow::setGraphicsApi(QSGRendererInterface::GraphicsApi::Direct3D12);

    QGuiApplication app(argc, argv);

    // Where QSettings keeps the window geometry and panel layout (12 §8): HKCU\Software\Avis\Visualizer
    QCoreApplication::setOrganizationName("Avis");
    QCoreApplication::setApplicationName("Visualizer");

    // The controller, which QML reaches as the VisualizerDriver singleton; it outlives the engine
    visualizer_driver driver{ std::move(loaded->configuration), configuration_path };

    QQmlApplicationEngine engine;
    QObject::connect(
        &engine,
        &QQmlApplicationEngine::objectCreationFailed,
        &app,
        []() { QCoreApplication::exit(-1); },
        Qt::QueuedConnection);
    engine.load(QUrl{ "qrc:///avis/imports/Avis/Visualizer/main.qml" });

    return app.exec();
}
