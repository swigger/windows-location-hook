#include "map_control.hpp"
#include <windowsx.h>
#include <wininet.h>
#include <shellapi.h>
#include <shlwapi.h>
#include <string>

namespace lfh {
namespace {
std::vector<BYTE> Fetch(HINTERNET session,TileKey key) {
    wchar_t url[180]{}; swprintf_s(url,L"https://tile.openstreetmap.org/%d/%d/%d.png",key.z,key.x,key.y);
    // WinINet performs persistent HTTP caching and conditional revalidation.
    auto request=InternetOpenUrlW(session,url,nullptr,0,INTERNET_FLAG_NO_UI|INTERNET_FLAG_NO_COOKIES,0);
    if(!request) return {};
    DWORD code{},size=sizeof(code);
    std::vector<BYTE> bytes;
    if(HttpQueryInfoW(request,HTTP_QUERY_STATUS_CODE|HTTP_QUERY_FLAG_NUMBER,&code,&size,nullptr) && code==200) {
        BYTE buffer[8192]; DWORD count{};
        while(true) {
            if(!InternetReadFile(request,buffer,sizeof(buffer),&count)) { bytes.clear(); break; }
            if(!count) break;
            if(bytes.size()+count>1024*1024) { bytes.clear(); break; }
            bytes.insert(bytes.end(),buffer,buffer+count);
        }
    }
    InternetCloseHandle(request); return bytes;
}
int Wrap(int n,int width) { return (n%width+width)%width; }
}
MapControl::MapControl() { model.Center(model.selected); }
MapControl::~MapControl() {
    { std::lock_guard lock(mutex); stopping=true; work.clear(); } wake.notify_all();
    if(worker.joinable()) worker.join();
}
void MapControl::Create(HWND parent,HINSTANCE instance) {
    WNDCLASSW wc{}; wc.lpfnWndProc=Proc; wc.hInstance=instance; wc.lpszClassName=L"LFHookMap";
    wc.hCursor=LoadCursorW(nullptr,IDC_CROSS); wc.style=CS_DBLCLKS;
    RegisterClassW(&wc);
    window=CreateWindowExW(0,wc.lpszClassName,L"地图：拖动平移，滚轮缩放，点击选点",WS_CHILD|WS_VISIBLE|WS_TABSTOP,
        0,0,100,100,parent,nullptr,instance,this);
    if(!fixture) worker=std::thread([this]{Run();});
}
void MapControl::Run() {
    CoInitializeEx(nullptr,COINIT_MULTITHREADED);
    auto internet=InternetOpenW(L"LFHookCfg/2.0 (Windows; privacy location picker)",INTERNET_OPEN_TYPE_PRECONFIG,nullptr,nullptr,0);
    if(internet) {
        DWORD timeout=4000; InternetSetOptionW(internet,INTERNET_OPTION_CONNECT_TIMEOUT,&timeout,sizeof(timeout));
        InternetSetOptionW(internet,INTERNET_OPTION_RECEIVE_TIMEOUT,&timeout,sizeof(timeout));
        InternetSetOptionW(internet,INTERNET_OPTION_SEND_TIMEOUT,&timeout,sizeof(timeout));
    }
    for(;;) {
        TileKey key;
        { std::unique_lock lock(mutex); wake.wait(lock,[&]{return stopping || !work.empty();});
          if(stopping) break; key=work.front(); work.pop_front(); }
        auto bytes=internet?Fetch(internet,key):std::vector<BYTE>{};
        { std::lock_guard lock(mutex); if(stopping) break; results.push_back({key,std::move(bytes)}); }
        PostMessageW(window,TileReady,0,0);
    }
    if(internet) InternetCloseHandle(internet); CoUninitialize();
}
void MapControl::RequestVisible() {
    if(fixture) return;
    RECT r{}; GetClientRect(window,&r); int count=1<<model.zoom;
    int left=static_cast<int>(std::floor((model.centerX-r.right/2.0)/256)),top=static_cast<int>(std::floor((model.centerY-r.bottom/2.0)/256));
    int right=static_cast<int>(std::floor((model.centerX+r.right/2.0)/256)),bottom=static_cast<int>(std::floor((model.centerY+r.bottom/2.0)/256));
    std::lock_guard lock(mutex);
    for(auto key:work) pending.erase(key); work.clear();
    for(int y=top;y<=bottom;++y) for(int x=left;x<=right;++x) {
        if(y<0 || y>=count) continue;
        TileKey key{model.zoom,Wrap(x,count),y};
        if(!tiles.contains(key) && pending.insert(key).second) work.push_back(key);
    }
    wake.notify_one();
}
void MapControl::Complete() {
    std::deque<TileResult> ready;
    { std::lock_guard lock(mutex); ready.swap(results); for(const auto& r:ready) pending.erase(r.key); }
    for(auto& result:ready) {
        std::unique_ptr<Gdiplus::Bitmap> bitmap;
        if(!result.bytes.empty()) {
            IStream* stream=SHCreateMemStream(result.bytes.data(),static_cast<UINT>(result.bytes.size()));
            if(stream) {
                { Gdiplus::Bitmap original(stream);
                  if(original.GetLastStatus()==Gdiplus::Ok && original.GetWidth()==256 && original.GetHeight()==256)
                      bitmap.reset(original.Clone(0,0,256,256,PixelFormat32bppPARGB)); }
                stream->Release();
            }
        }
        if(bitmap) ++loaded; else ++failed;
        tiles[result.key]=std::move(bitmap);
    }
    // Bound process memory. The persistent HTTP cache still handles revisits.
    if(tiles.size()>512) {
        for(auto i=tiles.begin();i!=tiles.end() && tiles.size()>256;)
            if(i->first.z!=model.zoom || std::abs((i->first.x+0.5)*256-model.centerX)>2048 || std::abs((i->first.y+0.5)*256-model.centerY)>2048) i=tiles.erase(i); else ++i;
    }
    InvalidateRect(window,nullptr,FALSE);
}
void MapControl::Refresh() { RequestVisible(); InvalidateRect(window,nullptr,FALSE); }
void MapControl::Center(GeoPoint p) { model.selected=p; model.Center(p); Refresh(); }
void MapControl::SelectCenter() { RECT r{}; GetClientRect(window,&r); Select(r.right/2,r.bottom/2); }
void MapControl::Zoom(int change) { model.Zoom(change); Refresh(); }
void MapControl::Select(int x,int y) {
    RECT r{}; GetClientRect(window,&r);
    model.selected=model.Geo(model.centerX+x-r.right/2.0,model.centerY+y-r.bottom/2.0);
    if(onSelect) onSelect(model.selected); InvalidateRect(window,nullptr,FALSE);
}
void MapControl::Draw(HDC dc,const RECT& rect) {
    using namespace Gdiplus;
    auto oldFont=fontHandle?SelectObject(dc,fontHandle):nullptr;
    Graphics g(dc); g.SetSmoothingMode(SmoothingModeAntiAlias);
    g.Clear(Color(237,241,243));
    double left=model.centerX-rect.right/2.0,top=model.centerY-rect.bottom/2.0;
    int count=1<<model.zoom;
    int x0=static_cast<int>(std::floor(left/256)),y0=static_cast<int>(std::floor(top/256));
    Pen grid(Color(217,226,230)); SolidBrush tileText(Color(112,131,142));
    Font font(L"Segoe UI",11,FontStyleRegular,UnitPixel);
    for(int y=y0;y<=static_cast<int>(std::floor((top+rect.bottom)/256));++y)
        for(int x=x0;x<=static_cast<int>(std::floor((left+rect.right)/256));++x) {
            RectF box(static_cast<REAL>(x*256-left),static_cast<REAL>(y*256-top),256,256);
            auto i=tiles.find({model.zoom,Wrap(x,count),y});
            if(i!=tiles.end() && i->second) g.DrawImage(i->second.get(),box);
            else { g.DrawRectangle(&grid,box); wchar_t text[64]{};
                swprintf_s(text,L"%d / %d / %d",model.zoom,Wrap(x,count),y);
                g.DrawString(text,-1,&font,PointF(box.X+12,box.Y+12),&tileText); }
        }
    auto [sx,sy]=model.Pixel(model.selected);
    double dx=sx-model.centerX; if(dx>model.World()/2) dx-=model.World(); if(dx< -model.World()/2) dx+=model.World();
    REAL px=static_cast<REAL>(rect.right/2.0+dx),py=static_cast<REAL>(rect.bottom/2.0+sy-model.centerY);
    SolidBrush halo(Color(55,0,112,138)),marker(Color(0,111,137)),white(Color(255,255,255));
    g.FillEllipse(&halo,px-23,py-23,46.0f,46.0f); g.FillEllipse(&white,px-11,py-11,22.0f,22.0f); g.FillEllipse(&marker,px-7,py-7,14.0f,14.0f);
    Pen cross(Color(155,39,57,70),1);
    REAL cx=rect.right/2.0f,cy=rect.bottom/2.0f;
    g.DrawLine(&cross,cx-9,cy,cx+9,cy); g.DrawLine(&cross,cx,cy-9,cx,cy+9);
    g.FillRectangle(&white,0,0,rect.right,32); SetBkMode(dc,TRANSPARENT); SetTextColor(dc,RGB(60,78,87));
    wchar_t hint[160]{}; swprintf_s(hint,L"拖动平移  ·  滚轮缩放  ·  点击选点     缩放 %d%s",model.zoom,failed?L"  ·  部分地图未加载，可继续输入坐标":L"");
    RECT header{12,5,rect.right-6,30}; DrawTextW(dc,hint,-1,&header,DT_SINGLELINE|DT_VCENTER|DT_END_ELLIPSIS);
    RECT attribution{std::max(0L,rect.right-248),rect.bottom-26,rect.right,rect.bottom};
    FillRect(dc,&attribution,static_cast<HBRUSH>(GetStockObject(WHITE_BRUSH)));
    DrawTextW(dc,L"© OpenStreetMap contributors",-1,&attribution,DT_CENTER|DT_VCENTER|DT_SINGLELINE);
    if(oldFont) SelectObject(dc,oldFont);
}
LRESULT CALLBACK MapControl::Proc(HWND h,UINT m,WPARAM w,LPARAM l) {
    auto self=reinterpret_cast<MapControl*>(GetWindowLongPtrW(h,GWLP_USERDATA));
    if(m==WM_NCCREATE) { self=static_cast<MapControl*>(reinterpret_cast<CREATESTRUCTW*>(l)->lpCreateParams); self->window=h; SetWindowLongPtrW(h,GWLP_USERDATA,reinterpret_cast<LONG_PTR>(self)); }
    if(!self) return DefWindowProcW(h,m,w,l);
    switch(m) {
    case WM_GETDLGCODE: return DLGC_WANTARROWS|DLGC_WANTCHARS|(w==VK_RETURN?DLGC_WANTMESSAGE:0);
    case WM_SETFONT: self->fontHandle=reinterpret_cast<HFONT>(w); InvalidateRect(h,nullptr,FALSE); return 0;
    case WM_SIZE: self->Refresh(); return 0;
    case WM_ERASEBKGND: return 1;
    case WM_PAINT: { PAINTSTRUCT ps{}; auto dc=BeginPaint(h,&ps); RECT r{}; GetClientRect(h,&r);
        auto mem=CreateCompatibleDC(dc); auto bitmap=CreateCompatibleBitmap(dc,std::max(1L,r.right),std::max(1L,r.bottom)); auto old=SelectObject(mem,bitmap);
        self->Draw(mem,r); BitBlt(dc,0,0,r.right,r.bottom,mem,0,0,SRCCOPY); SelectObject(mem,old); DeleteObject(bitmap); DeleteDC(mem); EndPaint(h,&ps); return 0; }
    case WM_PRINTCLIENT: { RECT r{}; GetClientRect(h,&r); self->Draw(reinterpret_cast<HDC>(w),r); return 0; }
    case TileReady: self->Complete(); return 0;
    case WM_LBUTTONDOWN: SetFocus(h); SetCapture(h); self->dragging=true; self->moved=false; self->last=self->down={GET_X_LPARAM(l),GET_Y_LPARAM(l)}; return 0;
    case WM_MOUSEMOVE: if(self->dragging) {
        POINT p{GET_X_LPARAM(l),GET_Y_LPARAM(l)};
        if(abs(p.x-self->down.x)+abs(p.y-self->down.y)>4) self->moved=true;
        self->model.Pan(p.x-self->last.x,p.y-self->last.y); self->last=p; InvalidateRect(h,nullptr,FALSE);
    } return 0;
    case WM_LBUTTONUP: if(self->dragging) {
        self->dragging=false; ReleaseCapture(); RECT r{}; GetClientRect(h,&r);
        if(!self->moved) {
            if(GET_X_LPARAM(l)>r.right-248 && GET_Y_LPARAM(l)>r.bottom-26) ShellExecuteW(h,L"open",L"https://www.openstreetmap.org/copyright",nullptr,nullptr,SW_SHOWNORMAL);
            else self->Select(GET_X_LPARAM(l),GET_Y_LPARAM(l));
        }
        self->Refresh();
    } return 0;
    case WM_CAPTURECHANGED: self->dragging=false; return 0;
    case WM_MOUSEWHEEL: { POINT p{GET_X_LPARAM(l),GET_Y_LPARAM(l)}; ScreenToClient(h,&p); RECT r{}; GetClientRect(h,&r);
        self->model.Zoom(GET_WHEEL_DELTA_WPARAM(w)>0?1:-1,p.x-r.right/2.0,p.y-r.bottom/2.0); self->Refresh(); return 0; }
    case WM_LBUTTONDBLCLK: self->Zoom(1); return 0;
    case WM_KEYDOWN:
        if(w==VK_ADD || w==VK_OEM_PLUS) self->Zoom(1);
        else if(w==VK_SUBTRACT || w==VK_OEM_MINUS) self->Zoom(-1);
        else if(w==VK_RETURN) self->SelectCenter();
        else { if(w==VK_LEFT) self->model.Pan(80,0); if(w==VK_RIGHT) self->model.Pan(-80,0); if(w==VK_UP) self->model.Pan(0,80); if(w==VK_DOWN) self->model.Pan(0,-80); self->Refresh(); }
        return 0;
    }
    return DefWindowProcW(h,m,w,l);
}
}
