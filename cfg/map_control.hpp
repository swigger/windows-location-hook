#pragma once
#include <windows.h>
#include <objidl.h>
#include <gdiplus.h>
#include <cmath>
#include <algorithm>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <condition_variable>
#include <deque>
#include <set>
#include <thread>
#include <vector>
#include <compare>

namespace lfh {
struct GeoPoint { double latitude,longitude; };
struct MapModel {
    static constexpr double Pi=3.14159265358979323846;
    int zoom=12;
    double centerX{},centerY{};
    GeoPoint selected{51.5074,-0.1278};
    double World() const { return std::ldexp(256.0,zoom); }
    std::pair<double,double> Pixel(GeoPoint p) const {
        double s=std::sin(std::clamp(p.latitude,-85.05112878,85.05112878)*Pi/180.0);
        return {(p.longitude+180)/360*World(),(0.5-std::log((1+s)/(1-s))/(4*Pi))*World()};
    }
    GeoPoint Geo(double x,double y) const {
        x=std::fmod(x,World()); if(x<0) x+=World(); y=std::clamp(y,0.0,World());
        return {std::atan(std::sinh(Pi*(1-2*y/World())))*180/Pi,x/World()*360-180};
    }
    void Normalize() { centerX=std::fmod(centerX,World()); if(centerX<0) centerX+=World(); centerY=std::clamp(centerY,0.0,World()); }
    void Center(GeoPoint p) { auto [x,y]=Pixel(p); centerX=x; centerY=y; Normalize(); }
    void Pan(double dx,double dy) { centerX-=dx; centerY-=dy; Normalize(); }
    void Zoom(int change,double dx=0,double dy=0) {
        int next=std::clamp(zoom+change,1,18); double factor=std::ldexp(1.0,next-zoom);
        centerX=(centerX+dx)*factor-dx; centerY=(centerY+dy)*factor-dy; zoom=next; Normalize();
    }
};
struct TileKey { int z,x,y; auto operator<=>(const TileKey&) const=default; };
struct TileResult { TileKey key; std::vector<BYTE> bytes; };
class MapControl {
public:
    static constexpr UINT TileReady=WM_APP+20;
    HWND window{};
    HFONT fontHandle{};
    MapModel model;
    std::function<void(GeoPoint)> onSelect;
    unsigned loaded{},failed{};
    bool fixture=false;
    MapControl();
    ~MapControl();
    void Create(HWND parent,HINSTANCE instance);
    void Center(GeoPoint p);
    void SelectCenter();
    void Zoom(int change);
    void Refresh();
    bool Idle() { std::lock_guard lock(mutex); return pending.empty(); }
    void Draw(HDC dc,const RECT& rect);
    static LRESULT CALLBACK Proc(HWND,UINT,WPARAM,LPARAM);
private:
    std::mutex mutex;
    std::condition_variable wake;
    std::deque<TileKey> work;
    std::deque<TileResult> results;
    std::set<TileKey> pending;
    std::map<TileKey,std::unique_ptr<Gdiplus::Bitmap>> tiles;
    std::thread worker;
    bool stopping=false,dragging=false,moved=false;
    POINT down{},last{};
    void Run();
    void RequestVisible();
    void Complete();
    void Select(int x,int y);
};
}
