#include "deployment.hpp"
#include <shlobj.h>
#include <sddl.h>
#include <aclapi.h>
#include <bcrypt.h>
#include <filesystem>
#include <vector>
#include <memory>
#include <fstream>

namespace lfh {
namespace {
constexpr wchar_t Parameters[]=L"SYSTEM\\CurrentControlSet\\Services\\lfsvc\\Parameters";
constexpr wchar_t BackupKey[]=L"SOFTWARE\\LFHookCfg\\OriginalService";
constexpr wchar_t DllName[]=L"LocationServiceHook.dll";
[[noreturn]] void Fail(const char* message,DWORD code=GetLastError()) {
    throw std::runtime_error(std::string(message)+" (Win32 "+std::to_string(code)+")");
}
struct Key { HKEY h{}; Key()=default; Key(const Key&)=delete; Key& operator=(const Key&)=delete;
    Key(Key&& other) noexcept:h(other.h){other.h=nullptr;} ~Key(){if(h) RegCloseKey(h);} };
struct Service { SC_HANDLE h{}; ~Service(){if(h) CloseServiceHandle(h);} };
struct Handle { HANDLE h=INVALID_HANDLE_VALUE; ~Handle(){if(h!=INVALID_HANDLE_VALUE && h) CloseHandle(h);} };
struct DebugPrivilege {
    Handle token; TOKEN_PRIVILEGES previous{}; DWORD size=sizeof(previous); bool adjusted=false;
    DebugPrivilege() {
        if(!OpenProcessToken(GetCurrentProcess(),TOKEN_ADJUST_PRIVILEGES|TOKEN_QUERY,&token.h)) return;
        TOKEN_PRIVILEGES privilege{}; privilege.PrivilegeCount=1;
        if(!LookupPrivilegeValueW(nullptr,SE_DEBUG_NAME,&privilege.Privileges[0].Luid)) return;
        privilege.Privileges[0].Attributes=SE_PRIVILEGE_ENABLED;
        adjusted=AdjustTokenPrivileges(token.h,FALSE,&privilege,sizeof(previous),&previous,&size) && GetLastError()==ERROR_SUCCESS;
    }
    ~DebugPrivilege(){if(adjusted) AdjustTokenPrivileges(token.h,FALSE,&previous,0,nullptr,nullptr);}
};
struct Value {
    DWORD type{}; std::vector<BYTE> data;
};

Value ReadValue(HKEY key,const wchar_t* name) {
    Value v; DWORD size{};
    auto err=RegQueryValueExW(key,name,nullptr,&v.type,nullptr,&size); if(err) Fail("Read registry",err);
    v.data.resize(size); err=RegQueryValueExW(key,name,nullptr,&v.type,v.data.data(),&size); if(err) Fail("Read registry",err);
    return v;
}
void SetValue(HKEY key,const wchar_t* name,const Value& v) {
    auto err=RegSetValueExW(key,name,0,v.type,v.data.data(),static_cast<DWORD>(v.data.size())); if(err) Fail("Write registry",err);
    RegFlushKey(key);
}
Value StringValue(const std::wstring& value,DWORD type=REG_EXPAND_SZ) {
    auto p=reinterpret_cast<const BYTE*>(value.c_str()); return {type,{p,p+(value.size()+1)*sizeof(wchar_t)}};
}
std::wstring String(const Value& value) {
    if((value.type!=REG_SZ && value.type!=REG_EXPAND_SZ) || value.data.size()<sizeof(wchar_t) || value.data.size()%sizeof(wchar_t)) Fail("Invalid ServiceDll",ERROR_INVALID_DATA);
    auto p=reinterpret_cast<const wchar_t*>(value.data.data());
    if(p[value.data.size()/sizeof(wchar_t)-1]) Fail("Unterminated ServiceDll",ERROR_INVALID_DATA);
    return p;
}
std::wstring Expand(const std::wstring& value) {
    DWORD size=ExpandEnvironmentStringsW(value.c_str(),nullptr,0); std::wstring out(size,L'\0');
    ExpandEnvironmentStringsW(value.c_str(),out.data(),size); out.resize(wcslen(out.c_str())); return out;
}
Key OpenParameters(REGSAM access) {
    Key k; auto err=RegOpenKeyExW(HKEY_LOCAL_MACHINE,Parameters,0,access|KEY_WOW64_64KEY,&k.h); if(err) Fail("Open lfsvc registry",err); return k;
}
bool SamePath(const std::wstring& a,const std::wstring& b) {
    return !_wcsicmp(std::filesystem::path(Expand(a)).lexically_normal().c_str(),std::filesystem::path(Expand(b)).lexically_normal().c_str());
}
std::wstring SystemDll(const wchar_t* file) {
    wchar_t path[MAX_PATH]{}; GetSystemDirectoryW(path,MAX_PATH); return std::wstring(path)+L"\\"+file;
}
void SecureDirectory() {
    auto path=InstallDirectory();
    if(!CreateDirectoryW(path.c_str(),nullptr) && GetLastError()!=ERROR_ALREADY_EXISTS)
        Fail("Create install directory");
    if(GetFileAttributesW(path.c_str())&FILE_ATTRIBUTE_REPARSE_POINT)
        Fail("Install directory cannot be a reparse point",ERROR_INVALID_NAME);

    PSECURITY_DESCRIPTOR descriptor{};
    if (ConvertStringSecurityDescriptorToSecurityDescriptorW(L"D:P(A;OICI;FA;;;SY)(A;OICI;FA;;;BA)(A;OICI;GRGX;;;BU)", SDDL_REVISION_1, &descriptor, nullptr)) {
        PACL dacl{};
        BOOL present{}, defaulted{};
        GetSecurityDescriptorDacl(descriptor, &present, &dacl, &defaulted);
        auto err = SetNamedSecurityInfoW(path.data(), SE_FILE_OBJECT, DACL_SECURITY_INFORMATION | PROTECTED_DACL_SECURITY_INFORMATION, nullptr, nullptr, dacl, nullptr);
        LocalFree(descriptor);
        // if (err) Log("Protect install directory", err);
    }
}

void AtomicConfig(const Configuration& c) {
    Validate(c); SecureDirectory();
    auto text=std::wstring(L"[Location]\r\nEnabled=")+(c.enabled?L"1":L"0")+L"\r\nLatitude="+Number(c.latitude)+L"\r\nLongitude="+Number(c.longitude)+
        L"\r\nAltitude="+Number(c.altitude)+L"\r\nAccuracy="+Number(c.accuracy)+L"\r\nPositionSource="+Sources[c.source]+L"\r\n";
    auto path=ConfigPath(),temp=path+L".tmp";
    auto bytes=Utf8(text);
    { Handle f{CreateFileW(temp.c_str(),GENERIC_WRITE,0,nullptr,CREATE_ALWAYS,FILE_ATTRIBUTE_NORMAL,nullptr)};
      if(f.h==INVALID_HANDLE_VALUE) Fail("Create INI"); DWORD written{};
      if(!WriteFile(f.h,bytes.data(),static_cast<DWORD>(bytes.size()),&written,nullptr) || written!=bytes.size() || !FlushFileBuffers(f.h)) Fail("Write INI"); }
    if(!MoveFileExW(temp.c_str(),path.c_str(),MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH)) Fail("Commit INI");
    WritePrivateProfileStringW(nullptr,nullptr,nullptr,path.c_str());
}
struct Host {
    Service scm,svc;
    Host() {
        scm.h=OpenSCManagerW(nullptr,nullptr,SC_MANAGER_CONNECT); if(!scm.h) Fail("Open service manager");
        svc.h=OpenServiceW(scm.h,L"lfsvc",SERVICE_QUERY_STATUS|SERVICE_START|SERVICE_STOP); if(!svc.h) Fail("Open lfsvc");
    }
    SERVICE_STATUS_PROCESS State() {
        SERVICE_STATUS_PROCESS s{}; DWORD count{};
        if(!QueryServiceStatusEx(svc.h,SC_STATUS_PROCESS_INFO,reinterpret_cast<BYTE*>(&s),sizeof(s),&count)) Fail("Query lfsvc"); return s;
    }
    void Wait(DWORD target) {
        auto deadline=GetTickCount64()+25000;
        while(State().dwCurrentState!=target) { if(GetTickCount64()>deadline) Fail("Service state timeout",ERROR_TIMEOUT); Sleep(100); }
    }
    void Stop() {
        auto s=State(); Handle process;
        if(s.dwProcessId) {
            process.h=OpenProcess(SYNCHRONIZE,FALSE,s.dwProcessId);
            if(!process.h && GetLastError()==ERROR_ACCESS_DENIED) {
                DebugPrivilege privilege;
                process.h=OpenProcess(SYNCHRONIZE,FALSE,s.dwProcessId);
            }
            if(!process.h && GetLastError()!=ERROR_INVALID_PARAMETER) Fail("Open old service host");
        }
        if(s.dwCurrentState!=SERVICE_STOPPED && s.dwCurrentState!=SERVICE_STOP_PENDING) {
            SERVICE_STATUS status{}; if(!ControlService(svc.h,SERVICE_CONTROL_STOP,&status) && GetLastError()!=ERROR_SERVICE_NOT_ACTIVE) Fail("Stop lfsvc");
        }
        Wait(SERVICE_STOPPED);
        if(process.h && process.h!=INVALID_HANDLE_VALUE && WaitForSingleObject(process.h,10000)!=WAIT_OBJECT_0)
            Fail("Old service host is still alive; restart Windows and retry",ERROR_BUSY);
    }
    void Start() {
        if(!StartServiceW(svc.h,0,nullptr) && GetLastError()!=ERROR_SERVICE_ALREADY_RUNNING) Fail("Start lfsvc"); Wait(SERVICE_RUNNING);
    }
};
}
std::wstring ModuleDirectory() { wchar_t p[32768]{}; GetModuleFileNameW(nullptr,p,32768); return std::filesystem::path(p).parent_path(); }
std::wstring InstallDirectory() {
    PWSTR base{}; if(FAILED(SHGetKnownFolderPath(FOLDERID_ProgramData,0,nullptr,&base))) Fail("Find ProgramData");
    std::wstring path=std::wstring(base)+L"\\LocationHook"; CoTaskMemFree(base); return path;
}
std::wstring ConfigPath() {
    return InstallDirectory()+L"\\location-hook.ini";
}
bool IsInstalled() {
    auto key=OpenParameters(KEY_READ);
    return SamePath(String(ReadValue(key.h,L"ServiceDll")),InstallDirectory()+L"\\"+DllName);
}
std::wstring DeploymentStatus() {
    return IsInstalled()?L"已安装 · 使用配置位置":L"未安装 · 使用系统定位";
}
void EnsureConfig() { if(GetFileAttributesW(ConfigPath().c_str())==INVALID_FILE_ATTRIBUTES) AtomicConfig(Configuration{}); }
void Install(const Configuration& config) {
    Validate(config);
    SecureDirectory();
    auto source=ModuleDirectory()+L"\\"+DllName, destination=InstallDirectory()+L"\\"+DllName;
    if(GetFileAttributesW(source.c_str())==INVALID_FILE_ATTRIBUTES) throw std::runtime_error("Place LocationServiceHook.dll next to lfhookcfg.exe.");
    auto key=OpenParameters(KEY_READ|KEY_WRITE); auto previous=ReadValue(key.h,L"ServiceDll"); Host host;
    if(!SamePath(String(previous),destination) && !SamePath(String(previous),SystemDll(L"lfsvc.dll")))
        throw std::runtime_error("ServiceDll is managed by another provider; restore it before installing.");
    bool running=host.State().dwCurrentState!=SERVICE_STOPPED;
    // SaveBackup(previous,running);
    auto oldConfig=ReadConfig(ConfigPath());
    auto rollback=destination+L".rollback";
    bool hasOld=GetFileAttributesW(destination.c_str())!=INVALID_FILE_ATTRIBUTES;
    if(hasOld && !CopyFileW(destination.c_str(),rollback.c_str(),FALSE)) Fail("Backup installed DLL");
    host.Stop();
    try {
        if(!SamePath(source,destination) && !CopyFileW(source.c_str(),destination.c_str(),FALSE)) Fail("Copy hook DLL");
        AtomicConfig(config); SetValue(key.h,L"ServiceDll",StringValue(destination)); host.Start();
        if(hasOld) DeleteFileW(rollback.c_str());
    } catch(...) {
        try { host.Stop(); SetValue(key.h,L"ServiceDll",previous); AtomicConfig(oldConfig);
              if(hasOld) CopyFileW(rollback.c_str(),destination.c_str(),FALSE); if(running) host.Start(); } catch(...) {}
        throw;
    }
}
void Apply(const Configuration& config) {
    Validate(config); bool installed=IsInstalled(); auto old=ReadConfig(ConfigPath());
    if(!installed) { AtomicConfig(config); return; }
    Host host; host.Stop();
    try { AtomicConfig(config); host.Start(); }
    catch(...) { try { host.Stop(); AtomicConfig(old); host.Start(); } catch(...) {} throw; }
}
void Uninstall() {
    if(!IsInstalled()) return;
    auto key=OpenParameters(KEY_READ|KEY_WRITE);
    auto previous=ReadValue(key.h, L"ServiceDll");
    Host host;
    host.Stop();
    SetValue(key.h, L"ServiceDll", StringValue(L"%SystemRoot%\\System32\\lfsvc.dll"));
    host.Start();
    DeleteFileW((InstallDirectory()+L"\\"+DllName).c_str());
}
}
