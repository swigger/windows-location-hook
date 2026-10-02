#include "deployment.hpp"
#include "map_control.hpp"
#include <commctrl.h>
#include <shellapi.h>
#include <windowsx.h>
#include <filesystem>
#include <fstream>
#include <sstream>

#pragma comment(linker,"/manifestdependency:\"type='win32' name='Microsoft.Windows.Common-Controls' version='6.0.0.0' processorArchitecture='amd64' publicKeyToken='6595b64144ccf1df' language='*'\"")

namespace {
using namespace lfh;
constexpr UINT OperationDone=WM_APP+1;
enum Id { Lat=101,Lon,Altitude,Accuracy,Source,Enabled,Center,PickCenter,Save,InstallHook,UninstallHook,ZoomIn,ZoomOut };
std::wstring Wide(const std::string& s) {
    int n=MultiByteToWideChar(CP_UTF8,0,s.data(),static_cast<int>(s.size()),nullptr,0);
    std::wstring out(n,L'\0'); MultiByteToWideChar(CP_UTF8,0,s.data(),static_cast<int>(s.size()),out.data(),n); return out;
}
std::wstring Text(HWND h) { int n=GetWindowTextLengthW(h); std::wstring s(n+1,L'\0'); GetWindowTextW(h,s.data(),n+1); s.resize(n); return s; }
void WriteResult(const std::wstring& path,const std::string& value) {
    if(!path.empty()) { std::ofstream out{std::filesystem::path(path),std::ios::binary}; if(!out) throw std::runtime_error("Cannot write result file"); out<<value; }
    else { DWORD written{}; WriteFile(GetStdHandle(STD_OUTPUT_HANDLE),value.data(),static_cast<DWORD>(value.size()),&written,nullptr); }
}
struct Options {
    std::wstring action,output,testPath;
    std::vector<std::pair<std::wstring,std::wstring>> overrides;
};
Options ParseOptions() {
    int count{}; auto argv=CommandLineToArgvW(GetCommandLineW(),&count); Options o;
    try {
        for(int i=1;i<count;++i) {
            std::wstring arg=argv[i];
            if(arg==L"--install" || arg==L"--apply" || arg==L"--uninstall" || arg==L"--status") {
                if(!o.action.empty()) throw std::runtime_error("Choose one action"); o.action=arg;
            } else if(arg==L"--output" || arg==L"--preview" || arg==L"--self-test" || arg==L"--lat" || arg==L"--lon" || arg==L"--altitude" || arg==L"--accuracy" || arg==L"--source") {
                if(++i>=count) throw std::runtime_error("Missing option value");
                if(arg==L"--output") o.output=argv[i];
                else if(arg==L"--preview" || arg==L"--self-test") { if(!o.action.empty()) throw std::runtime_error("Choose one action"); o.action=arg; o.testPath=argv[i]; }
                else o.overrides.emplace_back(arg,argv[i]);
            } else throw std::runtime_error("Unknown command line option");
        }
    } catch(...) { LocalFree(argv); throw; }
    LocalFree(argv); return o;
}
void Override(Configuration& c,const Options& options) {
    for(const auto& [key,value]:options.overrides) {
        if(key==L"--source") {
            c.source=-1; for(int i=0;i<std::size(Sources);++i) if(!_wcsicmp(value.c_str(),Sources[i])) c.source=i;
        } else {
            double v{}; if(!ParseNumber(value,v)) throw std::runtime_error("Invalid number");
            if(key==L"--lat") c.latitude=v; if(key==L"--lon") c.longitude=v;
            if(key==L"--altitude") c.altitude=v; if(key==L"--accuracy") c.accuracy=v;
        }
    }
    Validate(c);
}
void Capture(HWND window,const std::wstring& path) {
    RECT r{}; GetWindowRect(window,&r); int width=r.right-r.left,height=r.bottom-r.top;
    HDC screen=GetDC(window),mem=CreateCompatibleDC(screen);
    HBITMAP bitmap=CreateCompatibleBitmap(screen,width,height); auto old=SelectObject(mem,bitmap);
    PrintWindow(window,mem,0);
    CLSID png{0x557cf406,0x1a04,0x11d3,{0x9a,0x73,0x00,0x00,0xf8,0x1e,0xf3,0x2e}};
    Gdiplus::Status status;
    { Gdiplus::Bitmap image(bitmap,nullptr); status=image.Save(path.c_str(),&png,nullptr); }
    SelectObject(mem,old); DeleteObject(bitmap); DeleteDC(mem); ReleaseDC(window,screen);
    if(status!=Gdiplus::Ok) throw std::runtime_error("Screenshot save failed");
}
struct App {
    HWND window{},status{},hint{};
    HINSTANCE instance{};
    HFONT normal{},titleFont{},smallFont{};
    HBRUSH background=CreateSolidBrush(RGB(247,249,250));
    std::map<int,HWND> controls;
    std::vector<HWND> labels;
    MapControl map;
    Configuration config;
    Options options;
    std::thread operation;
    std::wstring operationMessage;
    bool busy=false,operationOK=false;
    int exitCode=0,testStage=0;
    ULONGLONG started{};
    double scale=1;
    std::ostringstream testLog;
    ~App() { if(operation.joinable()) operation.join(); DeleteObject(normal); DeleteObject(titleFont); DeleteObject(smallFont); DeleteObject(background); }
    int S(int value) const { return static_cast<int>(value*scale+0.5); }
    HWND Control(const wchar_t* klass,const wchar_t* text,DWORD style,int id) {
        auto h=CreateWindowExW(wcscmp(klass,L"EDIT")==0?WS_EX_CLIENTEDGE:0,klass,text,WS_CHILD|WS_VISIBLE|style,0,0,1,1,window,reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),instance,nullptr);
        SendMessageW(h,WM_SETFONT,reinterpret_cast<WPARAM>(normal),TRUE); if(id) controls[id]=h; return h;
    }
    HWND Label(const wchar_t* text) { auto h=Control(L"STATIC",text,SS_LEFT,0); labels.push_back(h); return h; }
    void Fill() {
        SetWindowTextW(controls[Lat],Number(config.latitude).c_str()); SetWindowTextW(controls[Lon],Number(config.longitude).c_str());
        SetWindowTextW(controls[Altitude],Number(config.altitude,2).c_str()); SetWindowTextW(controls[Accuracy],Number(config.accuracy,2).c_str());
        SendMessageW(controls[Source],CB_SETCURSEL,config.source,0); SendMessageW(controls[Enabled],BM_SETCHECK,config.enabled?BST_CHECKED:BST_UNCHECKED,0);
    }
    Configuration Gather() {
        Configuration c;
        if(!ParseNumber(Text(controls[Lat]),c.latitude) || !ParseNumber(Text(controls[Lon]),c.longitude) ||
           !ParseNumber(Text(controls[Altitude]),c.altitude) || !ParseNumber(Text(controls[Accuracy]),c.accuracy))
            throw std::runtime_error("请输入有效数字；经纬度使用小数点。");
        c.source=static_cast<int>(SendMessageW(controls[Source],CB_GETCURSEL,0,0)); c.enabled=SendMessageW(controls[Enabled],BM_GETCHECK,0,0)==BST_CHECKED;
        Validate(c); return c;
    }
    void SetStatus(const std::wstring& text) { SetWindowTextW(status,text.c_str()); }
    void Create() {
        scale=GetDpiForWindow(window)/96.0;
        normal=CreateFontW(-S(15),0,0,0,FW_NORMAL,FALSE,FALSE,FALSE,DEFAULT_CHARSET,0,0,CLEARTYPE_QUALITY,0,L"Microsoft YaHei UI");
        titleFont=CreateFontW(-S(24),0,0,0,FW_SEMIBOLD,FALSE,FALSE,FALSE,DEFAULT_CHARSET,0,0,CLEARTYPE_QUALITY,0,L"Microsoft YaHei UI");
        smallFont=CreateFontW(-S(12),0,0,0,FW_NORMAL,FALSE,FALSE,FALSE,DEFAULT_CHARSET,0,0,CLEARTYPE_QUALITY,0,L"Microsoft YaHei UI");
        Label(L"隐私位置"); SendMessageW(labels.back(),WM_SETFONT,reinterpret_cast<WPARAM>(titleFont),TRUE);
        Label(L"选择应用将收到的位置与来源");
        status=Label(DeploymentStatus().c_str());
        Label(L"纬度 Latitude"); Control(L"EDIT",L"",WS_TABSTOP|ES_AUTOHSCROLL,Lat);
        Label(L"经度 Longitude"); Control(L"EDIT",L"",WS_TABSTOP|ES_AUTOHSCROLL,Lon);
        Label(L"高度（米）"); Control(L"EDIT",L"",WS_TABSTOP|ES_AUTOHSCROLL,Altitude);
        Label(L"精度（米）"); Control(L"EDIT",L"",WS_TABSTOP|ES_AUTOHSCROLL,Accuracy);
        Label(L"位置来源 PositionSource"); Control(WC_COMBOBOXW,L"",WS_TABSTOP|CBS_DROPDOWNLIST|WS_VSCROLL,Source);
        for(auto s:Sources) SendMessageW(controls[Source],CB_ADDSTRING,0,reinterpret_cast<LPARAM>(s));
        Control(L"BUTTON",L"启用指定位置",BS_AUTOCHECKBOX|WS_TABSTOP,Enabled);
        Control(L"BUTTON",L"定位到输入坐标",BS_PUSHBUTTON|WS_TABSTOP,Center);
        Control(L"BUTTON",L"选取地图中心",BS_PUSHBUTTON|WS_TABSTOP,PickCenter);
        Control(L"BUTTON",L"保存并应用",BS_DEFPUSHBUTTON|WS_TABSTOP,Save);
        Control(L"BUTTON",L"安装 / 更新 Hook",BS_PUSHBUTTON|WS_TABSTOP,InstallHook);
        Control(L"BUTTON",L"卸载 Hook",BS_PUSHBUTTON|WS_TABSTOP,UninstallHook);
        Control(L"BUTTON",L"+",BS_PUSHBUTTON|WS_TABSTOP,ZoomIn); Control(L"BUTTON",L"−",BS_PUSHBUTTON|WS_TABSTOP,ZoomOut);
        hint=Label(L"在线底图来自 OpenStreetMap；不读取真实位置。\r\nINI：C:\\ProgramData\\LocationHook\\location-hook.ini");
        SendMessageW(hint,WM_SETFONT,reinterpret_cast<WPARAM>(smallFont),TRUE);
        map.fixture=options.action==L"--self-test"; map.model.selected={config.latitude,config.longitude}; map.model.Center(map.model.selected);
        map.Create(window,instance); SendMessageW(map.window,WM_SETFONT,reinterpret_cast<WPARAM>(smallFont),TRUE);
        map.onSelect=[this](GeoPoint p) { SetWindowTextW(controls[Lat],Number(p.latitude).c_str()); SetWindowTextW(controls[Lon],Number(p.longitude).c_str()); SetStatus(L"已选取地图位置 · 点击“保存并应用”生效"); };
        Fill(); Layout(); started=GetTickCount64();
        if(!options.testPath.empty()) SetTimer(window,1,250,nullptr);
    }
    void Place(HWND h,int x,int y,int width,int height) { MoveWindow(h,S(x),S(y),S(width),S(height),TRUE); }
    void Layout() {
        if(labels.empty()) return;
        RECT r{}; GetClientRect(window,&r); int width=static_cast<int>(r.right/scale),height=static_cast<int>(r.bottom/scale);
        Place(labels[0],24,20,290,34); Place(labels[1],24,60,290,24); Place(status,24,98,292,65);
        Place(labels[3],24,174,290,24); Place(controls[Lat],24,202,292,32);
        Place(labels[4],24,246,290,24); Place(controls[Lon],24,274,292,32);
        Place(labels[5],24,318,138,24); Place(labels[6],178,318,138,24);
        Place(controls[Altitude],24,346,138,32); Place(controls[Accuracy],178,346,138,32);
        Place(labels[7],24,390,292,24); Place(controls[Source],24,418,292,220);
        Place(controls[Enabled],24,462,292,28);
        Place(controls[Center],24,504,202,34); Place(controls[PickCenter],24,548,292,34);
        Place(controls[Save],24,602,292,40);
        Place(controls[InstallHook],24,652,174,34); Place(controls[UninstallHook],208,652,108,34);
        Place(hint,346,height-57,width-370,48);
        Place(map.window,340,20,width-360,height-88);
        Place(controls[ZoomIn],234,504,36,34); Place(controls[ZoomOut],280,504,36,34);
        SetWindowPos(controls[ZoomIn],HWND_TOP,0,0,0,0,SWP_NOMOVE|SWP_NOSIZE); SetWindowPos(controls[ZoomOut],HWND_TOP,0,0,0,0,SWP_NOMOVE|SWP_NOSIZE);
    }
    void Lock(bool value) {
        busy=value; for(auto [id,h]:controls) EnableWindow(h,!value); EnableWindow(map.window,!value);
    }
    void Begin(int action) {
        if(busy) return;
        Configuration next=config; if(action!=UninstallHook) next=Gather();
        if(operation.joinable()) operation.join(); Lock(true); SetStatus(L"正在更新配置和定位服务，请稍候…");
        operation=std::thread([this,action,next] {
            try {
                if(action==InstallHook) lfh::Install(next); else if(action==UninstallHook) lfh::Uninstall(); else lfh::Apply(next);
                config=next; operationOK=true;
                operationMessage=action==UninstallHook?L"已卸载 · 原系统服务已恢复":(IsInstalled()?(next.enabled?L"配置已生效 · ":L"已暂停指定位置 · ")+(next.enabled?std::wstring(Sources[next.source]):L"使用系统定位"):L"INI 已保存 · 安装 Hook 后生效");
            } catch(const std::exception& e) { operationOK=false; operationMessage=L"操作失败："+Wide(e.what()); }
            PostMessageW(window,OperationDone,0,0);
        });
    }
    void Command(int id) {
        try {
            if(id==Center) { auto c=Gather(); map.Center({c.latitude,c.longitude}); }
            else if(id==PickCenter) map.SelectCenter();
            else if(id==ZoomIn) map.Zoom(1); else if(id==ZoomOut) map.Zoom(-1);
            else if(id==Save || id==InstallHook || id==UninstallHook) Begin(id);
        } catch(const std::exception& e) { SetStatus(L"操作失败："+Wide(e.what())); }
    }
    void Require(bool condition,const char* name) {
        testLog<<(condition?"PASS ":"FAIL ")<<name<<"\n"; if(!condition) throw std::runtime_error(name);
    }
    void SelfTest() {
        MapModel m; m.Center(m.selected); auto p=m.Geo(m.centerX,m.centerY);
        Require(std::abs(p.latitude-51.5074)<1e-9 && std::abs(p.longitude+0.1278)<1e-9,"mercator round trip");
        auto anchor=m.Geo(m.centerX+80,m.centerY-40); m.Zoom(2,80,-40); auto after=m.Geo(m.centerX+80,m.centerY-40);
        Require(std::abs(anchor.latitude-after.latitude)<1e-9 && std::abs(anchor.longitude-after.longitude)<1e-9,"pointer anchored zoom");
        m.Pan(m.World()*4,0); Require(m.centerX>=0 && m.centerX<m.World(),"longitude wrap");
        SetWindowTextW(controls[Lat],L"nan"); bool invalid=false; try{ Gather(); }catch(...){invalid=true;} Require(invalid,"reject invalid input"); Fill();
        int originalZoom=map.model.zoom; SendMessageW(window,WM_COMMAND,ZoomIn,0); Require(map.model.zoom==originalZoom+1,"zoom button");
        auto previousX=map.model.centerX;
        SendMessageW(map.window,WM_LBUTTONDOWN,MK_LBUTTON,MAKELPARAM(250,250));
        SendMessageW(map.window,WM_MOUSEMOVE,MK_LBUTTON,MAKELPARAM(370,280));
        SendMessageW(map.window,WM_LBUTTONUP,0,MAKELPARAM(370,280));
        Require(std::abs(map.model.centerX-(previousX-120))<1e-6,"drag map");
        SendMessageW(map.window,WM_LBUTTONDOWN,MK_LBUTTON,MAKELPARAM(420,330)); SendMessageW(map.window,WM_LBUTTONUP,0,MAKELPARAM(420,330));
        auto c=Gather(); Require(std::abs(c.latitude-map.model.selected.latitude)<1e-8 && std::abs(c.longitude-map.model.selected.longitude)<1e-8,"map click updates coordinates");
        SendMessageW(window,WM_COMMAND,PickCenter,0); c=Gather(); auto center=map.model.Geo(map.model.centerX,map.model.centerY);
        Require(std::abs(c.latitude-center.latitude)<0.0001 && std::abs(c.longitude-center.longitude)<0.0001,"pick viewport center");
        // Persist the point selected through the real map/control path.
        SendMessageW(controls[Source],CB_SETCURSEL,0,0); SendMessageW(controls[Enabled],BM_SETCHECK,BST_CHECKED,0);
        testLog<<"selected_lat="<<Utf8(Number(c.latitude))<<"\nselected_lon="<<Utf8(Number(c.longitude))<<"\n";
        testStage=1; SendMessageW(window,WM_COMMAND,Save,0); Require(busy,"save button starts operation");
    }
    void FinishTest(bool ok) {
        KillTimer(window,1); exitCode=ok?0:2;
        try { Capture(window,options.testPath+L".png"); } catch(const std::exception& e) { testLog<<"capture="<<e.what()<<"\n"; exitCode=2; }
        testLog<<"loaded_tiles="<<map.loaded<<"\nfailed_tiles="<<map.failed<<"\nexit="<<exitCode<<"\n";
        try { WriteResult(options.testPath+L".txt",testLog.str()); }catch(...){exitCode=2;}
        DestroyWindow(window);
    }
    void Timer() {
        try {
            if(options.action==L"--self-test") {
                if(testStage==0) SelfTest();
                else if(testStage==2) {
                    testLog<<"operation="<<Utf8(operationMessage)<<"\n";
                    Require(operationOK,"save and service restart"); auto saved=ReadConfig(ConfigPath()),expected=Gather();
                    Require(saved.latitude==expected.latitude && saved.longitude==expected.longitude && saved.source==0,"INI matches selected point and WiFi"); FinishTest(true);
                }
            } else if(options.action==L"--preview" && (GetTickCount64()-started>40000 || (map.loaded>0 && map.Idle() && GetTickCount64()-started>4000))) {
                Require(map.loaded>0,"live map tiles loaded"); FinishTest(true);
            }
        } catch(const std::exception& e) { testLog<<"ERROR "<<e.what()<<"\n"; FinishTest(false); }
    }
    static LRESULT CALLBACK Proc(HWND h,UINT message,WPARAM w,LPARAM l) {
        auto a=reinterpret_cast<App*>(GetWindowLongPtrW(h,GWLP_USERDATA));
        if(message==WM_NCCREATE) { a=static_cast<App*>(reinterpret_cast<CREATESTRUCTW*>(l)->lpCreateParams); a->window=h; SetWindowLongPtrW(h,GWLP_USERDATA,reinterpret_cast<LONG_PTR>(a)); }
        if(!a) return DefWindowProcW(h,message,w,l);
        switch(message) {
        case WM_SIZE: a->Layout(); return 0;
        case WM_GETMINMAXINFO: reinterpret_cast<MINMAXINFO*>(l)->ptMinTrackSize={a->S(1000),a->S(790)}; return 0;
        case WM_COMMAND: a->Command(LOWORD(w)); return 0;
        case WM_TIMER: a->Timer(); return 0;
        case OperationDone:
            if(a->operation.joinable()) a->operation.join(); a->Lock(false); a->SetStatus(a->operationMessage);
            if(a->testStage==1) a->testStage=2; return 0;
        case WM_CTLCOLORSTATIC: case WM_CTLCOLORBTN:
            SetBkColor(reinterpret_cast<HDC>(w),RGB(247,249,250)); SetTextColor(reinterpret_cast<HDC>(w),RGB(37,56,66)); return reinterpret_cast<LRESULT>(a->background);
        case WM_ERASEBKGND: { RECT r{}; GetClientRect(h,&r); FillRect(reinterpret_cast<HDC>(w),&r,a->background); return 1; }
        case WM_DPICHANGED: {
            // Windows scales the controls through the system-aware DPI context.
            return 0;
        }
        case WM_CLOSE: if(a->busy) { a->SetStatus(L"正在完成服务操作，请稍候…"); return 0; } DestroyWindow(h); return 0;
        case WM_DESTROY: PostQuitMessage(a->exitCode); return 0;
        }
        return DefWindowProcW(h,message,w,l);
    }
};
}

int WINAPI wWinMain(HINSTANCE instance,HINSTANCE,PWSTR,int show) {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_SYSTEM_AWARE);
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    Options options; int result=1;
    HANDLE single=CreateMutexW(nullptr,TRUE,L"Global\\LFHookCfg-Configuration");
    bool already=single && GetLastError()==ERROR_ALREADY_EXISTS;
    ULONG_PTR gdiplus{}; Gdiplus::GdiplusStartupInput input; Gdiplus::GdiplusStartup(&gdiplus,&input,nullptr);
    try {
        options=ParseOptions();
        if(!single || already) throw std::runtime_error("Another lfhookcfg instance is running.");
        Configuration config;
        if(options.action!=L"--uninstall") { EnsureConfig(); config=ReadConfig(ConfigPath()); Override(config,options); }
        if(!options.action.empty() && options.testPath.empty()) {
            if(options.action==L"--install") lfh::Install(config);
            else if(options.action==L"--apply") lfh::Apply(config);
            else if(options.action==L"--uninstall") lfh::Uninstall();
            auto state=IsInstalled();
            WriteResult(options.output,"OK\ninstalled="+std::to_string(state)+"\nlatitude="+Utf8(Number(config.latitude))+"\nlongitude="+Utf8(Number(config.longitude))+"\nsource="+Utf8(Sources[config.source])+"\nini="+Utf8(ConfigPath())+"\n"); result=0;
        } else {
            INITCOMMONCONTROLSEX common{sizeof(common),ICC_STANDARD_CLASSES}; InitCommonControlsEx(&common);
            App app; app.instance=instance; app.config=config; app.options=options;
            WNDCLASSW wc{}; wc.lpfnWndProc=App::Proc; wc.hInstance=instance; wc.lpszClassName=L"LFHookConfiguration"; wc.hCursor=LoadCursorW(nullptr,IDC_ARROW); wc.hIcon=LoadIconW(nullptr,IDI_APPLICATION);
            RegisterClassW(&wc);
            UINT dpi=GetDpiForSystem(); int width=MulDiv(1180,dpi,96),height=MulDiv(820,dpi,96);
            auto window=CreateWindowExW(WS_EX_CONTROLPARENT,wc.lpszClassName,L"LFHook · 隐私位置配置",WS_OVERLAPPEDWINDOW|WS_CLIPCHILDREN,CW_USEDEFAULT,CW_USEDEFAULT,width,height,nullptr,nullptr,instance,&app);
            if(!window) throw std::runtime_error("Create window failed");
            app.Create(); ShowWindow(window,show); UpdateWindow(window);
            MSG msg{}; while(GetMessageW(&msg,nullptr,0,0)>0) {
                if(!IsDialogMessageW(window,&msg)) { TranslateMessage(&msg); DispatchMessageW(&msg); }
            }
            result=app.exitCode;
        }
    } catch(const std::exception& e) {
        if(options.action.empty()) MessageBoxW(nullptr,Wide(e.what()).c_str(),L"LFHook 配置错误",MB_OK|MB_ICONERROR);
        else { try { WriteResult(options.output,"ERROR "+std::string(e.what())+"\n"); } catch(...) {} }
    }
    if(gdiplus) Gdiplus::GdiplusShutdown(gdiplus);
    if(single) { if(!already) ReleaseMutex(single); CloseHandle(single); }
    CoUninitialize(); return result;
}
