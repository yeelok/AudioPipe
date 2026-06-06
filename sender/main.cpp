// AudioPipe Windows sender entry point. Runs as a Windows Service by default
// (spec §5.6), or in the foreground with --console for debugging.
//
//   audiopipe_sender --console [--config PATH]   # foreground, Ctrl-C to stop
//   audiopipe_sender                             # launched by the SCM as a service
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <winsock2.h>

#include <atomic>
#include <cstdio>
#include <string>

#include "audiopipe/config.h"
#include "audiopipe/log.h"
#include "sender_app.h"

using namespace audiopipe;

namespace {
std::atomic<bool> g_console_stop{false};

BOOL WINAPI console_ctrl(DWORD type) {
    if (type == CTRL_C_EVENT || type == CTRL_BREAK_EVENT || type == CTRL_CLOSE_EVENT) {
        g_console_stop.store(true);
        return TRUE;
    }
    return FALSE;
}
} // namespace

namespace audiopipe {
namespace win {
// Implemented in service.cpp.
int service_dispatch();
std::string default_config_path();
} // namespace win
} // namespace audiopipe

static int run_console(const std::string& cfg_path) {
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
        std::fprintf(stderr, "WSAStartup failed\n");
        return 1;
    }
    SenderConfig cfg;
    try {
        cfg = load_sender_config(cfg_path);
    } catch (const std::exception& e) {
        std::fprintf(stderr, "config error: %s\n", e.what());
        WSACleanup();
        return 1;
    }
    Logger log(cfg.log_file);
    SetConsoleCtrlHandler(console_ctrl, TRUE);
    std::fprintf(stderr, "AudioPipe sender -> %s:%u  (Ctrl-C to stop)\n", cfg.dest_ip.c_str(),
                 cfg.dest_port);
    const int rc = win::run_sender(cfg, g_console_stop, log);
    WSACleanup();
    return rc;
}

int main(int argc, char** argv) {
    bool        console  = false;
    std::string cfg_path = win::default_config_path();
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--console") {
            console = true;
        } else if (a == "--config" && i + 1 < argc) {
            cfg_path = argv[++i];
        }
    }

    if (console) {
        return run_console(cfg_path);
    }
    // No --console: hand off to the Service Control Manager. If not launched as
    // a service, service_dispatch() returns non-zero and we hint the user.
    const int rc = win::service_dispatch();
    if (rc != 0) {
        std::fprintf(stderr, "Not started by the Service Control Manager.\n"
                             "Run with --console to test in the foreground.\n");
    }
    return rc;
}
