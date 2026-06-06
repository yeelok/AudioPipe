// Windows Service plumbing for the sender (spec §5.6). The service reads its
// config from <exe-dir>\config\sender.json and runs until the SCM stops it.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <winsock2.h>

#include <atomic>
#include <string>

#include "audiopipe/config.h"
#include "audiopipe/log.h"
#include "sender_app.h"

namespace audiopipe {
namespace win {

// Defined below; declared here so service_main() (in the anonymous namespace)
// can call it before its definition.
std::string default_config_path();

namespace {

const char* const         kServiceName = "AudioPipeSender";
SERVICE_STATUS            g_status{};
SERVICE_STATUS_HANDLE     g_status_handle = nullptr;
std::atomic<bool>         g_stop{false};

void set_state(DWORD state, DWORD wait_hint_ms = 0) {
    g_status.dwCurrentState = state;
    g_status.dwWaitHint     = wait_hint_ms;
    SetServiceStatus(g_status_handle, &g_status);
}

void WINAPI service_ctrl(DWORD ctrl) {
    if (ctrl == SERVICE_CONTROL_STOP || ctrl == SERVICE_CONTROL_SHUTDOWN) {
        set_state(SERVICE_STOP_PENDING, 5000);
        g_stop.store(true, std::memory_order_release);
    }
}

void WINAPI service_main(DWORD, LPSTR*) {
    g_status_handle = RegisterServiceCtrlHandlerA(kServiceName, service_ctrl);
    if (g_status_handle == nullptr) {
        return;
    }
    g_status.dwServiceType        = SERVICE_WIN32_OWN_PROCESS;
    g_status.dwControlsAccepted   = SERVICE_ACCEPT_STOP | SERVICE_ACCEPT_SHUTDOWN;
    set_state(SERVICE_START_PENDING, 3000);

    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
        set_state(SERVICE_STOPPED);
        return;
    }

    SenderConfig cfg;
    try {
        cfg = load_sender_config(default_config_path());
    } catch (...) {
        WSACleanup();
        set_state(SERVICE_STOPPED);
        return;
    }

    Logger log(cfg.log_file);
    log.info("service: starting");
    set_state(SERVICE_RUNNING);

    run_sender(cfg, g_stop, log);

    log.info("service: stopped");
    WSACleanup();
    set_state(SERVICE_STOPPED);
}

} // namespace

std::string default_config_path() {
    char  buf[MAX_PATH] = {0};
    const DWORD n = GetModuleFileNameA(nullptr, buf, MAX_PATH);
    std::string path(buf, n);
    const auto slash = path.find_last_of("\\/");
    const std::string dir = (slash == std::string::npos) ? std::string(".") : path.substr(0, slash);
    return dir + "\\config\\sender.json";
}

int service_dispatch() {
    SERVICE_TABLE_ENTRYA table[] = {
        {const_cast<LPSTR>(kServiceName), service_main},
        {nullptr, nullptr},
    };
    if (!StartServiceCtrlDispatcherA(table)) {
        return 1; // not launched as a service
    }
    return 0;
}

} // namespace win
} // namespace audiopipe
