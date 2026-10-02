#include <Windows.h>
#include "hde/hde64.h"
#include <span>
#include <string_view>
#include <map>
#include <vector>
#include <algorithm>

using namespace std::literals;

static std::span<RUNTIME_FUNCTION>  EnumRuntimeFunctions(HMODULE mod) {
    auto base = reinterpret_cast<uintptr_t>(mod);
    auto dosHeader = reinterpret_cast<PIMAGE_DOS_HEADER>(base);
    auto ntHeaders = reinterpret_cast<PIMAGE_NT_HEADERS64>(base + dosHeader->e_lfanew);

    // 获取 Exception Directory
    const auto& exceptionDir = ntHeaders->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXCEPTION];
    if (exceptionDir.VirtualAddress == 0 || exceptionDir.Size == 0) {
        return {};
    }
    auto pRuntimeFunctions = reinterpret_cast<PRUNTIME_FUNCTION>(base + exceptionDir.VirtualAddress);
    size_t count = exceptionDir.Size / sizeof(RUNTIME_FUNCTION);
    return std::span<RUNTIME_FUNCTION>(pRuntimeFunctions, count);
}

static std::span<IMAGE_SECTION_HEADER>  EnumSectionHeaders(HMODULE mod) {
    PIMAGE_DOS_HEADER dosHeader = reinterpret_cast<PIMAGE_DOS_HEADER>(mod);
    PIMAGE_NT_HEADERS64 ntHeaders = reinterpret_cast<PIMAGE_NT_HEADERS64>(reinterpret_cast<uintptr_t>(mod) + dosHeader->e_lfanew);
    return std::span<IMAGE_SECTION_HEADER>(IMAGE_FIRST_SECTION(ntHeaders), ntHeaders->FileHeader.NumberOfSections);
}

static std::string_view name(const IMAGE_SECTION_HEADER& section) {
    int len = 0;
    for (int i = IMAGE_SIZEOF_SHORT_NAME - 1; i >= 0; --i) {
        if (section.Name[i] != 0 && section.Name[i] != ' ') {
            len = i + 1;
            break;
        }
    }
    return std::string_view(reinterpret_cast<const char*>(section.Name), len);
}

struct func_ptr_pos {
    uintptr_t addrs[3];
    size_t cnt{};
};

void** find_vtbl(HMODULE modlf) {
    if (!modlf) return nullptr;
    auto funcs = EnumRuntimeFunctions(modlf);
    auto sections = EnumSectionHeaders(modlf);
    auto text = std::find_if(sections.begin(), sections.end(), [](const IMAGE_SECTION_HEADER& s) {
        return name(s) == ".text"sv;
    });
    auto rdata = std::find_if(sections.begin(), sections.end(), [](const IMAGE_SECTION_HEADER& s) {
        return name(s) == ".rdata"sv;
    });
    if (text == sections.end() || rdata == sections.end()) return nullptr;
    auto in_text = [text, modlf](uintptr_t addr) {
        addr -= (uintptr_t)modlf;
        return addr >= text->VirtualAddress && addr < text->VirtualAddress + text->Misc.VirtualSize;
    };
    auto in_rdata = [rdata, modlf](uintptr_t addr) {
        addr -= (uintptr_t)modlf;
        return addr >= rdata->VirtualAddress && addr < rdata->VirtualAddress + rdata->Misc.VirtualSize;
    };

    std::map<uintptr_t, func_ptr_pos> funcMap;
    for (auto ap = rdata->VirtualAddress+24; ap < rdata->VirtualAddress + rdata->Misc.VirtualSize; ap += sizeof(uintptr_t)) {
        auto ptr = *(uintptr_t*)((char*)modlf + ap);
        if (in_text(ptr)) {
            auto& pos = funcMap[ptr];
            if (pos.cnt < std::size(pos.addrs)) pos.addrs[pos.cnt] = ap;
            pos.cnt++;
        }
    }
    std::vector<func_ptr_pos> candies;
    for (auto [ptr, pos] : funcMap) {
        if (pos.cnt > 2) continue;
        auto rva = ptr - (uintptr_t)modlf;
        auto pf = std::lower_bound(funcs.begin(), funcs.end(), RUNTIME_FUNCTION{ (DWORD)rva, 0, 0 }, [](const RUNTIME_FUNCTION& a, const RUNTIME_FUNCTION& b) {
            return a.BeginAddress < b.BeginAddress;
        });
        if (pf != funcs.end() && pf->BeginAddress == rva) {
            auto size = pf->EndAddress - pf->BeginAddress;
            for (auto ip = (char*)modlf + pf->BeginAddress; ip < (char*)modlf + pf->EndAddress - 5; ) {
                hde64s hs{};
                auto ilen = hde64_disasm(ip, &hs);
                if (ilen == 0 || (hs.flags & F_ERROR)) break;
                bool is_rip_relative = (hs.flags & F_MODRM) && (hs.modrm_mod == 0) && (hs.modrm_rm == 5);
                uintptr_t target_addr = (uintptr_t)ip + hs.len + (int32_t)hs.disp.disp32;
                if (is_rip_relative && in_rdata(target_addr)) {
                    const char* opstr = (const char*)target_addr;
                    if (strcmp(opstr, "CLocationSession::StartSubscriberRequest") == 0) {
                        candies.push_back(pos);
                        break;
                    }
                }
                ip += ilen;
            }
        }
    }
    if (candies.size() == 1) {
        // return any vtbl in the found set.
        auto rp = candies[0].addrs[0] - 24 + (char*)modlf;
        return (void**)rp;
    }
    return nullptr;
}
