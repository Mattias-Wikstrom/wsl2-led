#include "framework.h"
#include "LEDController.h"
#include <shellapi.h>
#include <string>
#include <iostream>
#include <sstream>
#include <thread>
#include <algorithm>
#include <map>
#include <functional>
#include <winsock.h>

#define MAX_LOADSTRING 100
#define ID_TRAY_ICON 1
#define WM_TRAY_CALLBACK (WM_USER + 1)

constexpr int default_udp_port = 8888;

// Global Variables:
HINSTANCE hInst;                                // current instance
WCHAR szTitle[MAX_LOADSTRING];                  // The title bar text
WCHAR szWindowClass[MAX_LOADSTRING];            // the main window class name
HMENU m_contextMenu = nullptr;

enum class LedColor { RED, GREEN, BLUE, UNKNOWN };

LedColor to_led_color(const std::string& s) {
    if (s == "R") return LedColor::RED;
    if (s == "G") return LedColor::GREEN;
    if (s == "B") return LedColor::BLUE;
    return LedColor::UNKNOWN;
}

std::string led_color_to_string(LedColor led_color)
{
    switch (led_color)
    {
    case LedColor::RED:
        return "R";
    case LedColor::GREEN:
        return "G";
    case LedColor::BLUE:
        return "B";
    default:
        return "UNKNOWN";
    }
}

enum class OnOff { ON, OFF, UNKNOWN };

OnOff to_on_off(const std::string& s) {
    if (s == "ON") return OnOff::ON;
    if (s == "OFF") return OnOff::OFF;
    return OnOff::UNKNOWN;
}

struct LEDData {
    LedColor name;
    COLORREF color;
    int brightness; // Ranges from 0 to 255
};

LEDData led[] = {
    {
        LedColor::RED,
        RGB(255, 0, 0),
        255
    },
    {
        LedColor::GREEN,
        RGB(0, 255, 0),
        255
    },
    {
        LedColor::BLUE,
        RGB(0, 0, 255),
        255
    },
};

const int num_leds = sizeof(led) / sizeof(led[0]);

NOTIFYICONDATA trayData;
HICON hIcon;

// Forward declarations of functions included in this code module:
ATOM              MyRegisterClass(HINSTANCE hInstance);
BOOL              InitInstance(HINSTANCE, int);
LRESULT CALLBACK  WndProc(HWND, UINT, WPARAM, LPARAM);
INT_PTR CALLBACK  About(HWND, UINT, WPARAM, LPARAM);

COLORREF MultiplyColor(COLORREF color, float factor)
{
    // Extract RGB components
    BYTE r = GetRValue(color);
    BYTE g = GetGValue(color);
    BYTE b = GetBValue(color);

    // Multiply and clamp
    r = (BYTE)min(255, (int)(r * factor));
    g = (BYTE)min(255, (int)(g * factor));
    b = (BYTE)min(255, (int)(b * factor));

    // Recombine
    return RGB(r, g, b);
}

void UpdateTrayIcon();

class UdpListener
{
public:
    UdpListener(int udp_port, std::function<void(const std::string&)> callback) : m_udp_port(udp_port), m_callback{ callback } {}

    bool Start();
    void Stop();

private:
    int m_udp_port;
    SOCKET m_socket = INVALID_SOCKET;
    std::thread m_listenerThread;
    std::function<void(const std::string&)> m_callback;
    bool m_listening = false;
};

bool UdpListener::Start()
{
    WSADATA wsaData;
    if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0)
        return false;

    m_listening = true;

    int port = m_udp_port;

    auto callback = m_callback;

    // Start listener thread
    m_listenerThread = std::thread([this, port, callback] {
        // Create socket inside the listener thread
        SOCKET socketHandle = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        if (socketHandle == INVALID_SOCKET) {
            OutputDebugString(L"Failed to create socket.");
            return;  // Exit the thread if socket creation fails
        }

        // Bind the socket to the UDP port
        sockaddr_in serverAddr = {};
        serverAddr.sin_family = AF_INET;
        serverAddr.sin_addr.s_addr = INADDR_ANY;
        serverAddr.sin_port = htons(port);

        if (bind(socketHandle, (sockaddr*)&serverAddr, sizeof(serverAddr)) == SOCKET_ERROR) {
            int error = WSAGetLastError();
            WCHAR errorMessage[256];
            swprintf_s(errorMessage, sizeof(errorMessage) / sizeof(WCHAR),
                L"Bind failed with error: %d", error);
            OutputDebugString(errorMessage);
            closesocket(socketHandle);
            return;  // Exit the thread if bind fails
        }

        // Log binding success
        sockaddr_in boundAddr;
        int boundAddrLen = sizeof(boundAddr);
        if (getsockname(socketHandle, (sockaddr*)&boundAddr, &boundAddrLen) == 0) {
            char* ip = inet_ntoa(boundAddr.sin_addr);
            unsigned short boundPort = ntohs(boundAddr.sin_port);
            WCHAR logMessage[256];
            swprintf_s(logMessage, sizeof(logMessage) / sizeof(WCHAR),
                L"Socket bound to IP: %S, Port: %d", ip, boundPort);
            OutputDebugString(logMessage);
        }

        // Listen for incoming UDP messages
        char buffer[1024] = "";
        sockaddr_in clientAddr;
        int clientAddrLen = sizeof(clientAddr);

        OutputDebugString(L"Listening");
        while (m_listening) {
            int bytesReceived = recvfrom(socketHandle, buffer, sizeof(buffer) - 1, 0,
                (sockaddr*)&clientAddr, &clientAddrLen);

            if (bytesReceived == SOCKET_ERROR) {
                int error = WSAGetLastError();
                WCHAR errorMessage[256];
                swprintf_s(errorMessage, sizeof(errorMessage) / sizeof(WCHAR),
                    L"recvfrom failed with error: %d", error);
                OutputDebugString(errorMessage);
                continue;
            }

            buffer[bytesReceived] = '\0'; // Make the string null-terminated
            callback(buffer);
        }

        // Clean up
        closesocket(socketHandle);
        });

    m_listenerThread.detach();  // Detach the listener thread to run in the background
    return true;
}

void UdpListener::Stop()
{
    m_listening = false;

    if (m_socket != INVALID_SOCKET) {
        closesocket(m_socket);
        m_socket = INVALID_SOCKET;
    }

    if (m_listenerThread.joinable()) {
        m_listenerThread.join();
    }

    WSACleanup();
}

// ================================================================================= //
// ICON CREATION (No changes, but relies on caller checking the return value)
// ================================================================================= //
HICON CreateLedIcon()
{
    // FIX: Using nullptr for GetDC is invalid in a production environment. 
    //      GetDC(NULL) returns a handle to the screen device context, but requires ReleaseDC(NULL, hdc). 
    //      We keep it for now but note the better practice is GetDC(hWnd).
    HDC hdc = GetDC(nullptr);
    HDC memDC = CreateCompatibleDC(hdc);

    // ... (Your existing BITMAPINFO and DIBSection creation code)

    BITMAPINFO bmi = {};
    bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth = 16;
    bmi.bmiHeader.biHeight = -16; // FIX: Use negative height to make it a top-down DIB
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;

    void* pixels;
    HBITMAP hBitmap = CreateDIBSection(hdc, &bmi, DIB_RGB_COLORS, &pixels, nullptr, 0);
    HBITMAP oldBitmap = (HBITMAP)SelectObject(memDC, hBitmap);

    float size = 12 / num_leds;

    for (int i = 0; i < num_leds; i++)
    {
        // Draw LED
        HBRUSH brush = CreateSolidBrush(MultiplyColor(led[i].color, led[i].brightness / 255.0));
        HPEN pen = CreatePen(PS_SOLID, 1, RGB(0, 0, 0));
        HBRUSH oldBrush = (HBRUSH)SelectObject(memDC, brush);
        HPEN oldPen = (HPEN)SelectObject(memDC, pen);

        float startX = 2 + i * size;

        Ellipse(memDC, startX, 8 - size / 2, startX + size, 8 + size / 2);

        SelectObject(memDC, oldBrush);
        SelectObject(memDC, oldPen);
        DeleteObject(brush);
        DeleteObject(pen);
    }

    ICONINFO iconInfo = {};
    iconInfo.fIcon = TRUE;
    iconInfo.hbmColor = hBitmap;

    // FIX: Mask should usually be a separate monochrome bitmap for transparency,
    //      but for simple opaque icons, this might work depending on the system. 
    iconInfo.hbmMask = hBitmap;

    HICON hIcon = CreateIconIndirect(&iconInfo);

    SelectObject(memDC, oldBitmap);
    DeleteObject(hBitmap);
    DeleteDC(memDC);
    ReleaseDC(nullptr, hdc);

    return hIcon;
}

// ================================================================================= //
// APPLICATION INITIALIZATION
// ================================================================================= //

ATOM MyRegisterClass(HINSTANCE hInstance)
{
    // ... (MyRegisterClass body is unchanged)
    WNDCLASSEXW wcex;

    wcex.cbSize = sizeof(WNDCLASSEX);

    wcex.style = CS_HREDRAW | CS_VREDRAW;
    wcex.lpfnWndProc = WndProc;
    wcex.cbClsExtra = 0;
    wcex.cbWndExtra = 0;
    wcex.hInstance = hInstance;
    wcex.hIcon = NULL;
    wcex.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wcex.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    wcex.lpszMenuName = NULL;
    wcex.lpszClassName = szWindowClass;
    wcex.hIconSm = LoadIcon(wcex.hInstance, MAKEINTRESOURCE(IDI_SMALL));

    return RegisterClassExW(&wcex);
}

void UpdateTrayIcon()
{
    // Clean up old icon before setting new one (this is necessary when modifying later)
    if (hIcon) {
        DestroyIcon(hIcon);
    }

    hIcon = CreateLedIcon();
    trayData.hIcon = hIcon;

    // FIX: Shell_NotifyIcon NIM_MODIFY should use the updated m_trayData (which is global now)
    Shell_NotifyIcon(NIM_MODIFY, &trayData);
}

std::map<int, bool> m_ledStates;

void ShowStatus()
{
    std::wostringstream o;

    o << L"LED Status:\n";
    for (const auto& [name, color, brightness] : led)
    {
        o << led_color_to_string(name).c_str() << L":" << (brightness / 255.0) << L"\n";
    }

    MessageBox(NULL, o.str().c_str(), L"LED Controller", MB_OK | MB_ICONINFORMATION);
}

BOOL InitInstance(HINSTANCE hInstance, int nCmdShow)
{
    hInst = hInstance;

    // FIX: Use a global/member variable for the HWND if it's needed elsewhere.
    HWND hWnd = CreateWindowW(szWindowClass, szTitle, WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT, 0, CW_USEDEFAULT, 0, nullptr, nullptr, hInstance, nullptr);

    if (!hWnd)
    {
        return FALSE;
    }

    ShowWindow(hWnd, SW_HIDE);
    UpdateWindow(hWnd);

    // FIX: Use the global m_trayData and m_hIcon
    ZeroMemory(&trayData, sizeof(NOTIFYICONDATA));
    trayData.cbSize = sizeof(NOTIFYICONDATA);
    trayData.hWnd = hWnd;
    trayData.uID = ID_TRAY_ICON; // Use constant
    trayData.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    trayData.uCallbackMessage = WM_TRAY_CALLBACK; // Use constant

    // Check if icon creation succeeded
    hIcon = CreateLedIcon();
    if (hIcon == NULL) {
        return FALSE;
    }
    trayData.hIcon = hIcon;

    wcscpy_s(trayData.szTip, L"LED Controller");

    if (!Shell_NotifyIcon(NIM_ADD, &trayData)) {
        return FALSE;
    }

    // Context Menu Creation
    m_contextMenu = CreatePopupMenu();
    AppendMenu(m_contextMenu, MF_STRING, 1, L"LED Status");
    AppendMenu(m_contextMenu, MF_SEPARATOR, 0, nullptr);
    AppendMenu(m_contextMenu, MF_STRING, 2, L"Exit");

    UpdateTrayIcon();

    // Create UDP listener with callback to update the tray icon
    UdpListener udpListener(default_udp_port, [](const std::string& message) {
        OutputDebugStringA("\nReceived data:\n");
        OutputDebugStringA(message.c_str());
        OutputDebugStringA("\n");

        // Find the position of the colon
        size_t pos = message.find(':');
        if (pos != std::string::npos) {
            // Split into two substrings
            std::string nameOfLed = message.substr(0, pos);        // "G"
            std::string state = message.substr(pos + 1);     // "ON"

            //OutputDebugStringA(nameOfLed.c_str());
            //OutputDebugStringA(state.c_str());

            for (int i = 0; i < num_leds; i++) {
                if (led[i].name == to_led_color(nameOfLed)) {
                    switch (to_on_off(state))
                    {
                    case OnOff::ON:
                        led[i].brightness = 255;

                        break;
                    case OnOff::OFF:
                        led[i].brightness = 0;

                        break;
                    default:
                        led[i].brightness = 100;
                    }
                }
            }
        }
        else {
            std::cout << "No colon found in message!\n";
        }

        UpdateTrayIcon();

        });

    // Start UDP listener
    if (!udpListener.Start())
    {
        MessageBox(nullptr, L"Failed to start UDP listener", L"Error", MB_ICONERROR);
        return -1;
    }

    return TRUE;
}

// ================================================================================= //
// WINDOW PROCEDURE
// ================================================================================= //

LRESULT CALLBACK WndProc(HWND hWnd, UINT message, WPARAM wParam, LPARAM lParam)
{

    switch (message)
    {
    case WM_TRAY_CALLBACK: // Tray icon message
        switch (lParam)
        {
        case WM_RBUTTONUP:
        {
            POINT pt;
            GetCursorPos(&pt);
            // FIX: SetForegroundWindow is critical for the context menu to disappear 
            // when the user clicks elsewhere.
            SetForegroundWindow(hWnd);
            TrackPopupMenu(m_contextMenu, TPM_RIGHTBUTTON, pt.x, pt.y, 0, hWnd, nullptr);

            // PostMessage(hWnd, WM_NULL, 0, 0); // Not strictly required, TrackPopupMenu does this
        }
        break;
        case WM_LBUTTONDBLCLK:
            ShowStatus();
            break;
        }
        break; // FIX: Break moved here to prevent fall-through

    case WM_COMMAND:
        // Command IDs 1 and 2 from the context menu
        switch (LOWORD(wParam))
        {
        case 1: // Status            
            ShowStatus(); // Call ShowStatus here
            break;
        case 2: // Exit
            DestroyWindow(hWnd); // Call DestroyWindow to trigger WM_DESTROY
            break;
        }
        break;

    case WM_DESTROY:
        // FIX: CLEANUP TRAY ICON AND RESOURCES

        for (int i = 0; i < num_leds; i++) {
            // 1. Delete the tray icon
            Shell_NotifyIcon(NIM_DELETE, &trayData);

            // 2. Destroy the Icon handle
            if (hIcon) {
                DestroyIcon(hIcon);
            }
        }

        // 3. Destroy the context menu
        if (m_contextMenu) {
            DestroyMenu(m_contextMenu);
        }

        PostQuitMessage(0);
        break;

    default:
        return DefWindowProc(hWnd, message, wParam, lParam);
    }

    return 0;
}

int APIENTRY wWinMain(_In_ HINSTANCE hInstance,
    _In_opt_ HINSTANCE hPrevInstance,
    _In_ LPWSTR    lpCmdLine,
    _In_ int       nCmdShow)
{
    UNREFERENCED_PARAMETER(hPrevInstance);
    UNREFERENCED_PARAMETER(lpCmdLine);

    // Initialize global strings
    LoadStringW(hInstance, IDS_APP_TITLE, szTitle, MAX_LOADSTRING); // Requires IDS_APP_TITLE to be defined in resources
    wcscpy_s(szWindowClass, L"LEDControllerWndClass");

    MyRegisterClass(hInstance);

    // Perform application initialization:
    if (!InitInstance(hInstance, nCmdShow))
    {
        return FALSE;
    }

    MSG msg;

    // Main message loop:
    while (GetMessage(&msg, nullptr, 0, 0))
    {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }

    return (int)msg.wParam;
}