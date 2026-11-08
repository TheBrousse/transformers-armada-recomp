// vrr_probe.cpp - the screen's variable refresh range from the graphics driver (vrr_probe.h; TODO 35).
//
// AMD: ADL (atiadlxx.dll, the driver's own library - nothing to install), read only: the adapters, their connected
// displays, each display's FreeSync state and range (ADL2_Display_FreeSyncState_Get). The structures are the public ADL
// SDK's (adl_structures.h: AdapterInfo, ADLDisplayInfo). The display is the one whose range tops at the window's screen's
// refresh (this PC, 2026-11-04: the 240 Hz PG34WCDM 210..240 Hz, the 100 Hz G34WQC 48..100). Read once; any failure
// leaves the range unknown.
#include "vrr_probe.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

namespace tf::vrr
{
#if defined(_WIN32)
    namespace
    {
        struct AdapterInfo
        {
            int iSize;
            int iAdapterIndex;
            char strUDID[256];
            int iBusNumber;
            int iDeviceNumber;
            int iFunctionNumber;
            int iVendorID;
            char strAdapterName[256];
            char strDisplayName[256];
            int iPresent;
            int iExist;
            char strDriverPath[256];
            char strDriverPathExt[256];
            char strPNPString[256];
            int iOSDisplayIndex;
        };
        struct DisplayId
        {
            int iDisplayLogicalIndex;
            int iDisplayPhysicalIndex;
            int iDisplayLogicalAdapterIndex;
            int iDisplayPhysicalAdapterIndex;
        };
        struct DisplayInfo
        {
            DisplayId displayID;
            int iDisplayControllerIndex;
            char strDisplayName[256];
            char strDisplayManufacturerName[256];
            int iDisplayType;
            int iDisplayOutputType;
            int iDisplayConnector;
            int iDisplayInfoMask;
            int iDisplayInfoValue;
        };
        using Malloc = void *(__stdcall *)(int);
        using Create = int (*)(Malloc, int, void **);
        using Destroy = int (*)(void *);
        using Count = int (*)(void *, int *);
        using Adapters = int (*)(void *, AdapterInfo *, int);
        using Displays = int (*)(void *, int, int *, DisplayInfo **, int);
        using FreeSync = int (*)(void *, int, int, int *, int *, int *, int *);

        void *__stdcall adlAlloc(int size) { return std::malloc(static_cast<size_t>(size > 0 ? size : 1)); }

        struct Seen
        {
            std::string name;
            int state;
            double minHz, maxHz;
        };

        std::vector<Seen> readAll()
        {
            std::vector<Seen> out;
            HMODULE dll = LoadLibraryW(L"atiadlxx.dll");
            if (!dll)
                dll = LoadLibraryW(L"atiadlxy.dll");
            if (!dll)
                return out;
            const auto create = reinterpret_cast<Create>(GetProcAddress(dll, "ADL2_Main_Control_Create"));
            const auto destroy = reinterpret_cast<Destroy>(GetProcAddress(dll, "ADL2_Main_Control_Destroy"));
            const auto count = reinterpret_cast<Count>(GetProcAddress(dll, "ADL2_Adapter_NumberOfAdapters_Get"));
            const auto adapters = reinterpret_cast<Adapters>(GetProcAddress(dll, "ADL2_Adapter_AdapterInfo_Get"));
            const auto displays = reinterpret_cast<Displays>(GetProcAddress(dll, "ADL2_Display_DisplayInfo_Get"));
            const auto freeSync = reinterpret_cast<FreeSync>(GetProcAddress(dll, "ADL2_Display_FreeSyncState_Get"));
            void *context = nullptr;
            if (!create || !destroy || !count || !adapters || !displays || !freeSync || create(adlAlloc, 1, &context) != 0)
            {
                FreeLibrary(dll);
                return out;
            }
            int n = 0;
            if (count(context, &n) == 0 && n > 0 && n < 256)
            {
                std::vector<AdapterInfo> info(static_cast<size_t>(n));
                for (AdapterInfo &a : info)
                {
                    std::memset(&a, 0, sizeof(a));
                    a.iSize = sizeof(AdapterInfo);
                }
                if (adapters(context, info.data(), static_cast<int>(sizeof(AdapterInfo) * info.size())) == 0)
                    for (const AdapterInfo &a : info)
                    {
                        if (!a.iPresent)
                            continue;
                        int shown = 0;
                        DisplayInfo *list = nullptr;
                        if (displays(context, a.iAdapterIndex, &shown, &list, 0) != 0 || !list)
                            continue;
                        for (int k = 0; k < shown && k < 64; ++k)
                        {
                            const DisplayInfo &d = list[k];
                            if ((d.iDisplayInfoValue & 0x3) != 0x3) // (connected and mapped)
                                continue;
                            int current = 0, fallback = 0, minMicro = 0, maxMicro = 0;
                            if (freeSync(context, a.iAdapterIndex, d.displayID.iDisplayLogicalIndex, &current, &fallback, &minMicro, &maxMicro) != 0)
                                continue;
                            Seen s;
                            s.name = std::string(d.strDisplayManufacturerName, strnlen(d.strDisplayManufacturerName, 256)) + " " +
                                     std::string(d.strDisplayName, strnlen(d.strDisplayName, 256));
                            s.state = current;
                            s.minHz = minMicro / 1.0e6;
                            s.maxHz = maxMicro / 1.0e6;
                            out.push_back(s);
                        }
                        std::free(list);
                    }
            }
            destroy(context);
            FreeLibrary(dll);
            return out;
        }
    }

    Range screenRange(double refreshHz)
    {
        static std::once_flag once;
        static std::vector<Seen> seen;
        std::call_once(once, [] { seen = readAll(); });
        Range r;
        const Seen *pick = nullptr;
        int matches = 0;
        for (const Seen &s : seen)
            if (refreshHz > 0.0 && s.maxHz > 0.0 && std::fabs(s.maxHz - refreshHz) <= 2.0)
            {
                if (!pick || pick->name != s.name)
                    ++matches;
                pick = pick ? pick : &s;
            }
        if (matches == 1 && pick && pick->minHz > 0.0 && pick->minHz < pick->maxHz)
        {
            r.known = true;
            r.state = pick->state;
            r.minHz = pick->minHz;
            r.maxHz = pick->maxHz;
        }
        static bool told = false;
        if (!told)
        {
            told = true;
            if (seen.empty())
                std::fprintf(stderr, "[tf] the screen's variable refresh range: none from the graphics driver (AMD's ADL only)\n");
            std::vector<std::string> said; // (each display once: ADL lists them under every adapter)
            for (const Seen &s : seen)
            {
                bool again = false;
                for (const std::string &n : said)
                    again = again || n == s.name;
                if (again)
                    continue;
                said.push_back(s.name);
                std::fprintf(stderr, "[tf] the graphics driver's FreeSync: %s state %d, range %.0f..%.0f Hz%s\n", s.name.c_str(), s.state, s.minHz, s.maxHz,
                             r.known && pick && s.name == pick->name ? " (this screen)" : "");
            }
        }
        return r;
    }
#else
    Range screenRange(double) { return Range{}; }
#endif
}
