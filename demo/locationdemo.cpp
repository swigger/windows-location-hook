// 原生 Win32 窗口 + C++/WinRT。不需要 .NET、WinUI 或第三方依赖。
#include <windows.h>
#include <shellapi.h>
#include <CommCtrl.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Devices.Geolocation.h>
#include <chrono>
#include <iomanip>
#include <locale>
#include <sstream>
#include <string>

using namespace winrt;
using namespace Windows::Foundation;
using namespace Windows::Devices::Geolocation;
using namespace std::chrono_literals;

namespace
{
constexpr int LocateId = 1001, CancelId = 1002, SettingsId = 1003, MapId = 1004;
constexpr int ModeId = 1005;
constexpr UINT_PTR PollTimer = 1;
enum class LocationMode { AllowCoarse, RequestPrecise };

const wchar_t* SourceName(PositionSource source)
{
    switch (source)
    {
    case PositionSource::Satellite: return L"Satellite（卫星）";
    case PositionSource::WiFi: return L"WiFi（附近无线接入点）";
    case PositionSource::Cellular: return L"Cellular（蜂窝网络）";
    case PositionSource::IPAddress: return L"IPAddress（IP 地址）";
    case PositionSource::Default: return L"Default（用户设置的默认位置）";
    case PositionSource::Obfuscated: return L"Obfuscated（隐私降精度）";
    default: return L"Unknown（来源未知）";
    }
}

const wchar_t* StatusName(PositionStatus status)
{
    switch (status)
    {
    case PositionStatus::Ready: return L"Ready";
    case PositionStatus::Initializing: return L"Initializing";
    case PositionStatus::NoData: return L"NoData";
    case PositionStatus::Disabled: return L"Disabled";
    case PositionStatus::NotInitialized: return L"NotInitialized";
    default: return L"NotAvailable";
    }
}

std::wstring TimestampUtc(DateTime timestamp)
{
    // WinRT DateTime 与 FILETIME 均以 1601-01-01 UTC 为起点，单位为 100 ns。
    ULARGE_INTEGER ticks{};
    ticks.QuadPart = static_cast<ULONGLONG>(timestamp.time_since_epoch().count());
    FILETIME ft{ticks.LowPart, ticks.HighPart};
    SYSTEMTIME utc{};
    if (!FileTimeToSystemTime(&ft, &utc)) return L"无法转换时间";
    wchar_t buffer[40]{};
    swprintf_s(buffer, L"%04u-%02u-%02uT%02u:%02u:%02u.%03uZ",
        utc.wYear, utc.wMonth, utc.wDay, utc.wHour, utc.wMinute,
        utc.wSecond, utc.wMilliseconds);
    return buffer;
}

class App
{
public:
    HWND window{}, locateButton{}, cancelButton{}, mapButton{}, modeCombo{}, modeHint{}, statusText{}, resultText{};
    HFONT font{};
    LocationMode requestMode{LocationMode::AllowCoarse};
    std::wstring mapUrl;
    IAsyncOperation<GeolocationAccessStatus> accessOperation{nullptr};
    IAsyncOperation<Geoposition> positionOperation{nullptr};
    Geolocator locator{nullptr};

    HWND Control(const wchar_t* type, const wchar_t* text, DWORD style,
                 int x, int y, int width, int height, int id = 0)
    {
        auto control = CreateWindowExW(0, type, text, WS_CHILD | WS_VISIBLE | style,
            x, y, width, height, window, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),
            GetModuleHandleW(nullptr), nullptr);
        if (!control) throw_last_error();
        SendMessageW(control, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
        return control;
    }

    void CreateControls()
    {
        font = CreateFontW(-18, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
            DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
            CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Microsoft YaHei UI");
        Control(L"STATIC", L"Windows 定位服务 · C++/WinRT", 0, 24, 20, 700, 28);
        Control(L"STATIC", L"定位模式：", 0, 24, 62, 110, 28);
        modeCombo = Control(L"COMBOBOX", L"", WS_TABSTOP | CBS_DROPDOWNLIST | WS_VSCROLL,
            134, 58, 610, 160, ModeId);
        SendMessageW(modeCombo, CB_ADDSTRING, 0,
            reinterpret_cast<LPARAM>(L"允许模糊位置（不请求精确授权）"));
        SendMessageW(modeCombo, CB_ADDSTRING, 0,
            reinterpret_cast<LPARAM>(L"请求精确位置（需要位置权限）"));
        SendMessageW(modeCombo, CB_SETCURSEL, 0, 0);
        modeHint = Control(L"STATIC", L"", 0, 24, 102, 720, 60);
        ModeChanged();
        locateButton = Control(L"BUTTON", L"获取当前位置", WS_TABSTOP | BS_DEFPUSHBUTTON,
            24, 174, 160, 38, LocateId);
        cancelButton = Control(L"BUTTON", L"取消", WS_TABSTOP, 196, 174, 100, 38, CancelId);
        Control(L"BUTTON", L"打开 Windows 位置设置", WS_TABSTOP, 308, 174, 260, 38, SettingsId);
        mapButton = Control(L"BUTTON", L"在 Google 地图中打开", WS_TABSTOP,
            24, 224, 250, 38, MapId);
        statusText = Control(L"STATIC", L"尚未请求定位。", 0, 24, 278, 720, 48);
        resultText = Control(L"EDIT", L"结果将显示经纬度、定位来源、估计误差和时间戳。",
            WS_TABSTOP | WS_BORDER | WS_VSCROLL | ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL,
            24, 336, 720, 330);
        EnableWindow(cancelButton, FALSE);
        EnableWindow(mapButton, FALSE);
    }

    void ModeChanged()
    {
        const bool coarse = SendMessageW(modeCombo, CB_GETCURSEL, 0, 0) == 0;
        SetWindowTextW(modeHint, coarse
            ? L"无精确权限时尝试获取系统模糊位置；已有精确权限时仍可能返回更精确位置。\r\n此选项不强制模糊化。系统位置总开关仍需开启。"
            : L"请求位置权限并优先获取高精度结果，可能出现系统授权框。\r\n实际精度取决于硬件、网络和系统权限，不保证米级定位。");
        mapUrl.clear();
        if (mapButton) EnableWindow(mapButton, FALSE);
        if (resultText) SetWindowTextW(resultText, L"模式已更改，请重新获取位置。");
        if (statusText) SetWindowTextW(statusText, L"尚未按新模式请求定位。");
    }

    void BeginPositionRequest()
    {
        locator = Geolocator{};
        if (requestMode == LocationMode::AllowCoarse)
        {
            // Default 是精度/功耗偏好，不是隐私开关。
            locator.DesiredAccuracy(PositionAccuracy::Default);
            // 允许无精确授权时回退；不是强制 coarse-only。
            locator.AllowFallbackToConsentlessPositions();
        }
        else
        {
            locator.DesiredAccuracy(PositionAccuracy::High);
        }
        positionOperation = locator.GetGeopositionAsync(0s, 60s);
        SetWindowTextW(statusText, L"正在定位（最多 60 秒）……");
    }

    // 取消不需要等待后台操作结束；没有捕获窗口指针的异步回调。
    void Finish() noexcept
    {
        KillTimer(window, PollTimer);
        try { if (accessOperation) accessOperation.Cancel(); } catch (...) {}
        try { if (positionOperation) positionOperation.Cancel(); } catch (...) {}
        accessOperation = nullptr;
        positionOperation = nullptr;
        locator = nullptr;
        EnableWindow(locateButton, TRUE);
        EnableWindow(cancelButton, FALSE);
        EnableWindow(modeCombo, TRUE);
    }

    void Start()
    {
        if (accessOperation || positionOperation) return;
        EnableWindow(locateButton, FALSE);
        EnableWindow(cancelButton, TRUE);
        EnableWindow(modeCombo, FALSE);
        requestMode = SendMessageW(modeCombo, CB_GETCURSEL, 0, 0) == 0
            ? LocationMode::AllowCoarse : LocationMode::RequestPrecise;
        SetWindowTextW(resultText, L"");
        mapUrl.clear();
        EnableWindow(mapButton, FALSE);
        if (requestMode == LocationMode::AllowCoarse)
        {
            // 微软对 fallback 的文档明确允许不调用 RequestAccessAsync。
            BeginPositionRequest();
        }
        else
        {
            SetWindowTextW(statusText, L"等待 Windows 精确位置权限……");
            // 此函数由按钮消息调用：在前台窗口的 STA/UI 线程请求权限。
            accessOperation = Geolocator::RequestAccessAsync();
        }
        if (!SetTimer(window, PollTimer, 100, nullptr)) throw_last_error();
    }

    void ShowPosition(const Geoposition& position)
    {
        const auto c = position.Coordinate();
        const auto point = c.Point().Position();
        const double age = std::chrono::duration<double>(clock::now() - c.Timestamp()).count();
        std::wostringstream text;
        text << L"请求模式: " << (requestMode == LocationMode::AllowCoarse
                 ? L"允许模糊位置（未请求精确授权）" : L"请求精确位置")
             << L"\r\n返回级别: " << (c.PositionSource() == PositionSource::Obfuscated
                 ? L"系统模糊位置（Obfuscated）"
                 : L"系统未标记为模糊位置；实际精度见 Accuracy")
             << std::fixed << std::setprecision(6)
             << L"\r\n纬度 Latitude: " << point.Latitude
             << L"\r\n经度 Longitude: " << point.Longitude
             << std::setprecision(1)
             << L"\r\n估计误差 Accuracy: " << c.Accuracy() << L" 米"
             << L"\r\n来源 PositionSource: " << SourceName(c.PositionSource())
             << L"\r\n测量时间（UTC）: " << TimestampUtc(c.Timestamp())
             << L"\r\n结果年龄: " << age << L" 秒"
             << L"\r\n服务状态: " << StatusName(locator.LocationStatus())
             << L"\r\n\r\nAccuracy 是估计水平误差，不是绝对保证；小数位多不表示准确。"
             << L"\r\nHigh 是精度偏好，不能让没有 GPS 的电脑获得卫星定位。"
             << L"\r\n时间戳新也不代表 Wi-Fi/IP 数据库没有过期。"
             << L"\r\nDefault 是默认位置，不能当作当前实测位置。";
        SetWindowTextW(resultText, text.str().c_str());
        // Google Maps URLs：用经纬度搜索，默认浏览器打开，无需 API Key。
        // 固定使用小数点，避免区域设置把小数点变成逗号。
        std::wostringstream url;
        url.imbue(std::locale::classic());
        url << L"https://www.google.com/maps/search/?api=1&query="
            << std::fixed << std::setprecision(6)
            << point.Latitude << L"%2C" << point.Longitude;
        mapUrl = url.str();
        EnableWindow(mapButton, TRUE);
        SetWindowTextW(statusText, L"已获取一次定位。");
    }

    void OpenMap() const
    {
        if (mapUrl.empty()) return;
        if (reinterpret_cast<INT_PTR>(ShellExecuteW(window, L"open",
            mapUrl.c_str(), nullptr, nullptr, SW_SHOWNORMAL)) <= 32)
        {
            const auto text = L"无法启动默认浏览器，可手动打开：\r\n" + mapUrl;
            MessageBoxW(window, text.c_str(), L"无法打开地图", MB_OK | MB_ICONERROR);
        }
    }

    void Poll()
    {
        // 只轮询异步操作的状态；没有重复发起定位请求。
        // 不在 UI 线程调用阻塞的 .get()；只有操作结束后才 GetResults()。
        if (accessOperation)
        {
            if (accessOperation.Status() == AsyncStatus::Started) return;
            const auto access = accessOperation.GetResults();
            accessOperation = nullptr;
            if (access != GeolocationAccessStatus::Allowed)
            {
                SetWindowTextW(statusText, access == GeolocationAccessStatus::Denied
                    ? L"位置权限被拒绝（Denied）。" : L"位置权限未确定（Unspecified）。");
                SetWindowTextW(resultText, L"请检查 Windows 位置服务、应用/桌面应用访问权限及组织策略，再重试。系统不一定会重复弹出权限框。");
                Finish();
                return;
            }
            BeginPositionRequest();
        }
        if (positionOperation)
        {
            if (positionOperation.Status() == AsyncStatus::Started)
            {
                std::wstring status = L"正在定位（最多 60 秒），服务状态：";
                status += StatusName(locator.LocationStatus());
                SetWindowTextW(statusText, status.c_str());
                return;
            }
            ShowPosition(positionOperation.GetResults());
            Finish();
        }
    }

    void ShowError(const hresult_error& error)
    {
        Finish();
        SetWindowTextW(statusText, L"未能获取位置或执行操作。");
        std::wostringstream text;
        text << error.message().c_str() << L"\r\nHRESULT: 0x"
             << std::hex << std::uppercase << std::setw(8) << std::setfill(L'0')
             << static_cast<unsigned long>(error.code().value)
             << L"\r\n\r\n请检查位置权限、网络和系统服务。超时或无定位来源时也可能失败。";
        SetWindowTextW(resultText, text.str().c_str());
    }
};

LRESULT CALLBACK WindowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam)
{
    auto* app = reinterpret_cast<App*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    if (message == WM_NCCREATE)
    {
        app = static_cast<App*>(reinterpret_cast<CREATESTRUCTW*>(lParam)->lpCreateParams);
        app->window = window;
        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(app));
    }
    if (!app) return DefWindowProcW(window, message, wParam, lParam);
    try
    {
        switch (message)
        {
        case WM_CREATE: app->CreateControls(); return 0;
        case WM_COMMAND:
            if (LOWORD(wParam) == ModeId && HIWORD(wParam) == CBN_SELCHANGE)
            {
                app->ModeChanged();
                return 0;
            }
            if (HIWORD(wParam) != BN_CLICKED) break;
            switch (LOWORD(wParam))
            {
            case LocateId: app->Start(); break;
            case MapId: app->OpenMap(); break;
            case CancelId:
                app->Finish();
                SetWindowTextW(app->statusText, L"已取消定位。");
                break;
            case SettingsId:
                if (reinterpret_cast<INT_PTR>(ShellExecuteW(window, L"open",
                    L"ms-settings:privacy-location", nullptr, nullptr, SW_SHOWNORMAL)) <= 32)
                    MessageBoxW(window, L"请手动打开 设置 → 隐私和安全性 → 位置。", L"无法打开位置设置", MB_OK);
                break;
            }
            return 0;
        case WM_TIMER: if (wParam == PollTimer) app->Poll(); return 0;
        case WM_DESTROY:
            app->Finish();
            if (app->font) DeleteObject(app->font);
            PostQuitMessage(0);
            return 0;
        }
    }
    catch (const hresult_error& error)
    {
        if (message == WM_CREATE) return -1;
        app->ShowError(error);
    }
    catch (...)
    {
        app->Finish();
        if (message == WM_CREATE) return -1;
        MessageBoxW(window, L"发生意外错误。", L"LocationDemo", MB_OK | MB_ICONERROR);
    }
    return DefWindowProcW(window, message, wParam, lParam);
}
} // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int showCommand)
{
    INITCOMMONCONTROLSEX InitCtrls = { sizeof(InitCtrls), 0xffff};
    InitCommonControlsEx(&InitCtrls);

    try
    {
        init_apartment(apartment_type::single_threaded);
        // App 及其 WinRT 对象在 apartment 退出前释放。
        {
            App app;
            WNDCLASSW wc{};
            wc.lpfnWndProc = WindowProc;
            wc.hInstance = instance;
            wc.lpszClassName = L"WindowsLocationCppDemo";
            wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
            wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
            if (!RegisterClassW(&wc)) throw_last_error();
            // 固定大小，便于保持 demo 的 Win32 布局代码简单。
            constexpr DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX;
            RECT rect{0, 0, 780, 700};
            AdjustWindowRect(&rect, style, FALSE);
            auto window = CreateWindowExW(WS_EX_CONTROLPARENT, wc.lpszClassName,
                L"Windows 定位服务 Demo — C++", style, CW_USEDEFAULT, CW_USEDEFAULT,
                rect.right - rect.left, rect.bottom - rect.top, nullptr, nullptr, instance, &app);
            if (!window) throw_last_error();
            ShowWindow(window, showCommand);
            MSG message{};
            BOOL result;
            while ((result = GetMessageW(&message, nullptr, 0, 0)) > 0)
            {
                if (!IsDialogMessageW(window, &message))
                {
                    TranslateMessage(&message);
                    DispatchMessageW(&message);
                }
            }
            if (result == -1) throw_last_error();
        }
        uninit_apartment();
        return 0;
    }
    catch (const hresult_error& error)
    {
        MessageBoxW(nullptr, error.message().c_str(), L"启动失败", MB_OK | MB_ICONERROR);
        return 1;
    }
}

#pragma comment(linker,"/manifestdependency:\"type='win32' name='Microsoft.Windows.Common-Controls' version='6.0.0.0' processorArchitecture='*' publicKeyToken='6595b64144ccf1df' language='*'\"")
#pragma comment(lib,"windowsapp") // C++/WinRT
#pragma comment(lib,"comctl32") // Common Controls
