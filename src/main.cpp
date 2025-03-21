#include "TSentry.h"

#include "ArgsParser.h"
#include "Common.h"
#include "CustomAssert.h"
#include "Http.h"
#include "LuaAPI.h"
#include "SignalHandling.h"
#include "TConfig.h"
#include "THeartbeatThread.h"
#include "TLuaEngine.h"
#include "TNetwork.h"
#include "TPPSMonitor.h"
#include "TResourceManager.h"
#include "TScopedTimer.h"
#include "TServer.h"

#include <iostream>
#include <thread>
#define CPPHTTPLIB_OPENSSL_SUPPORT 1
static const std::string sCommandlineArguments = R"(
USAGE: 
    BeamMP-Server [arguments]
    
ARGUMENTS:
    --help              
                        Displays this help and exits.
    --config=/path/to/ServerConfig.toml
                        Absolute or relative path to the 
                        Server Config file, including the
                        filename. For paths and filenames with 
                        spaces, put quotes around the path.
    --working-directory=/path/to/folder
                        Sets the working directory of the Server.
                        All paths are considered relative to this,
                        including the path given in --config.
    --version
                        Prints version info and exits.

EXAMPLES:
    BeamMP-Server --config=../MyWestCoastServerConfig.toml
        Runs the BeamMP-Server and uses the server config file 
        which is one directory above it and is named
        'MyWestCoastServerConfig.toml'.
)";

// this is provided by the build system, leave empty for source builds
// global, yes, this is ugly, no, it cant be done another way
TSentry Sentry {};

struct MainArguments {
    int argc {};
    char** argv {};
    std::vector<std::string_view> List;
    std::string InvokedAs;
};

int BeamMPServerMain(MainArguments Arguments);

int main(int argc, char** argv) {
    MainArguments Args { argc, argv, {}, argv[0] };
    Args.List.reserve(argc);
    for (int i = 1; i < argc; ++i) {
        Args.List.push_back(argv[i]);
    }
    int MainRet = 0;
    try {
        MainRet = BeamMPServerMain(std::move(Args));
    } catch (const std::exception& e) {
        beammp_error("A fatal exception has occurred and the server is forcefully shutting down.");
        beammp_error(e.what());
        Sentry.LogException(e, _file_basename, _line);
        MainRet = -1;
    }
    return MainRet;
}

int BeamMPServerMain(MainArguments Arguments) {
    setlocale(LC_ALL, "C");
    Application::InitializeConsole();
    ArgsParser Parser;
    Parser.RegisterArgument({ "help" }, ArgsParser::NONE);
    Parser.RegisterArgument({ "version" }, ArgsParser::NONE);
    Parser.RegisterArgument({ "config" }, ArgsParser::HAS_VALUE);
    Parser.RegisterArgument({ "working-directory" }, ArgsParser::HAS_VALUE);
    Parser.Parse(Arguments.List);
    if (!Parser.Verify()) {
        return 1;
    }
    if (Parser.FoundArgument({ "help" })) {
        Application::Console().Internal().set_prompt("");
        Application::Console().WriteRaw(sCommandlineArguments);
        return 0;
    }
    if (Parser.FoundArgument({ "version" })) {
        Application::Console().Internal().set_prompt("");
        Application::Console().WriteRaw("BeamMP-Server v" + Application::ServerVersionString());
        return 0;
    }

    std::string ConfigPath = "ServerConfig.toml";
    if (Parser.FoundArgument({ "config" })) {
        auto MaybeConfigPath = Parser.GetValueOfArgument({ "config" });
        if (MaybeConfigPath.has_value()) {
            ConfigPath = MaybeConfigPath.value();
            beammp_info("Custom config requested via commandline arguments: '" + ConfigPath + "'");
        }
    }
    if (Parser.FoundArgument({ "working-directory" })) {
        auto MaybeWorkingDirectory = Parser.GetValueOfArgument({ "working-directory" });
        if (MaybeWorkingDirectory.has_value()) {
            beammp_info("Custom working directory requested via commandline arguments: '" + MaybeWorkingDirectory.value() + "'");
            try {
                fs::current_path(fs::path(MaybeWorkingDirectory.value()));
            } catch (const std::exception& e) {
                beammp_error("Could not set working directory to '" + MaybeWorkingDirectory.value() + "': " + e.what());
            }
        }
    }

    Application::SetSubsystemStatus("Main", Application::Status::Starting);
    bool Success = Application::Console().Internal().enable_write_to_file("Server.log");
    if (!Success) {
        beammp_error("unable to open file for writing: \"Server.log\"");
    }

    SetupSignalHandlers();

    bool Shutdown = false;
    Application::RegisterShutdownHandler([&Shutdown] {
        Application::SetSubsystemStatus("Main", Application::Status::ShuttingDown);
        Shutdown = true;
    });
    Application::RegisterShutdownHandler([] {
        auto Futures = LuaAPI::MP::Engine->TriggerEvent("onShutdown", "");
        TLuaEngine::WaitForAll(Futures);
    });

    TServer Server(Arguments.List);
    TConfig Config(ConfigPath);
    TLuaEngine LuaEngine;
    LuaEngine.SetServer(&Server);
    Application::Console().InitializeLuaConsole(LuaEngine);

    if (Config.Failed()) {
        beammp_info("Closing in 10 seconds");
        // loop to make it possible to ctrl+c instead
        for (size_t i = 0; i < 20; ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
        }
        return 1;
    }

    RegisterThread("Main");

    beammp_trace("Running in debug mode on a debug build");
    Sentry.SetupUser();
    Sentry.PrintWelcome();
    TResourceManager ResourceManager;
    TPPSMonitor PPSMonitor(Server);
    THeartbeatThread Heartbeat(ResourceManager, Server);
    TNetwork Network(Server, PPSMonitor, ResourceManager);
    LuaEngine.SetNetwork(&Network);
    PPSMonitor.SetNetwork(Network);
    Application::CheckForUpdates();

    if (Application::Settings.HTTPServerEnabled) {
        Http::Server::SetupEnvironment();
        Http::Server::THttpServerInstance HttpServerInstance {};
    }

    Application::SetSubsystemStatus("Main", Application::Status::Good);
    RegisterThread("Main(Waiting)");

    bool FullyStarted = false;
    while (!Shutdown) {
        if (!FullyStarted) {
            FullyStarted = true;
            bool WithErrors = false;
            std::string SystemsBadList {};
            auto Statuses = Application::GetSubsystemStatuses();
            for (const auto& NameStatusPair : Statuses) {
                if (NameStatusPair.second == Application::Status::Starting) {
                    FullyStarted = false;
                } else if (NameStatusPair.second == Application::Status::Bad) {
                    SystemsBadList += NameStatusPair.first + ", ";
                    WithErrors = true;
                }
            }
            // remove ", "
            SystemsBadList = SystemsBadList.substr(0, SystemsBadList.size() - 2);
            if (FullyStarted) {
                if (!WithErrors) {
                    beammp_info("ALL SYSTEMS STARTED SUCCESSFULLY, EVERYTHING IS OKAY");
                } else {
                    beammp_error("STARTUP NOT SUCCESSFUL, SYSTEMS " + SystemsBadList + " HAD ERRORS. THIS MAY OR MAY NOT CAUSE ISSUES.");
                }
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    Application::SetSubsystemStatus("Main", Application::Status::Shutdown);
    beammp_info("Shutdown.");
    return 0;
}
