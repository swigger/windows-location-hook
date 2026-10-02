#pragma once
#include <windows.h>
#include <array>
#include <cmath>
#include <string>
#include <stdexcept>

namespace lfh {
inline constexpr const wchar_t* Sources[]={L"WiFi",L"Default",L"IPAddress",L"Satellite",L"Cellular",L"Obfuscated"};
struct Configuration {
    double latitude=51.5074,longitude=-0.1278,altitude=15,accuracy=5;
    int source=0;
    bool enabled=true;
};
inline bool ParseNumber(const std::wstring& text,double& value) {
    wchar_t* end{}; value=wcstod(text.c_str(),&end);
    return end!=text.c_str() && *end==0 && std::isfinite(value);
}
inline std::wstring Number(double value,int precision=8) {
    wchar_t buffer[96]{}; swprintf_s(buffer,L"%.*f",precision,value); return buffer;
}
inline void Validate(const Configuration& c) {
    if(!std::isfinite(c.latitude) || c.latitude < -90 || c.latitude > 90 ||
       !std::isfinite(c.longitude) || c.longitude < -180 || c.longitude > 180 ||
       !std::isfinite(c.altitude) || !std::isfinite(c.accuracy) || c.accuracy<=0 ||
       c.source<0 || c.source>=static_cast<int>(std::size(Sources)))
        throw std::runtime_error("Invalid coordinates, accuracy, or source");
}
inline std::wstring ReadIni(const std::wstring& path,const wchar_t* key,const wchar_t* fallback) {
    wchar_t value[256]{}; GetPrivateProfileStringW(L"Location",key,fallback,value,256,path.c_str()); return value;
}
inline Configuration ReadConfig(const std::wstring& path) {
    Configuration c;
    if(!ParseNumber(ReadIni(path,L"Latitude",L"51.5074"),c.latitude) ||
       !ParseNumber(ReadIni(path,L"Longitude",L"-0.1278"),c.longitude) ||
       !ParseNumber(ReadIni(path,L"Altitude",L"15"),c.altitude) ||
       !ParseNumber(ReadIni(path,L"Accuracy",L"5"),c.accuracy)) throw std::runtime_error("Invalid INI number");
    auto source=ReadIni(path,L"PositionSource",L"WiFi"); c.source=-1;
    for(int i=0;i<std::size(Sources);++i) if(!_wcsicmp(source.c_str(),Sources[i])) c.source=i;
    c.enabled=GetPrivateProfileIntW(L"Location",L"Enabled",1,path.c_str())!=0;
    Validate(c); return c;
}
inline std::string Utf8(const std::wstring& value) {
    int size=WideCharToMultiByte(CP_UTF8,0,value.c_str(),static_cast<int>(value.size()),nullptr,0,nullptr,nullptr);
    std::string result(size,'\0'); WideCharToMultiByte(CP_UTF8,0,value.c_str(),static_cast<int>(value.size()),result.data(),size,nullptr,nullptr); return result;
}
}
