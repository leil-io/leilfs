/*


 Copyright 2023 Leil Storage OÜ

 This file is part of SaunaFS.

 SaunaFS is free software: you can redistribute it and/or modify
 it under the terms of the GNU General Public License as published by
 the Free Software Foundation, version 3.

 SaunaFS is distributed in the hope that it will be useful,
 but WITHOUT ANY WARRANTY; without even the implied warranty of
 MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 GNU General Public License for more details.

 You should have received a copy of the GNU General Public License
 along with SaunaFS. If not, see <http://www.gnu.org/licenses/>.
 */

#pragma once

#include "common/platform.h"

#ifdef _WIN32
#include <windows.h>
#elif __linux__
#include <sys/sysinfo.h>
#endif

class IMemoryInfo {
public:
    virtual ~IMemoryInfo() = default;
    virtual unsigned long long getAvailableMemory() = 0;
    virtual unsigned long long getTotalMemory() = 0;
};

#ifdef _WIN32
class WindowsMemoryInfo : public IMemoryInfo {
public:
    unsigned long long getAvailableMemory() override {
        MEMORYSTATUSEX memInfo;
        memInfo.dwLength = sizeof(MEMORYSTATUSEX);
        GlobalMemoryStatusEx(&memInfo);
        return memInfo.ullAvailPhys;
    }

    unsigned long long getTotalMemory() override {
        MEMORYSTATUSEX memInfo;
        memInfo.dwLength = sizeof(MEMORYSTATUSEX);
        GlobalMemoryStatusEx(&memInfo);
        return memInfo.ullTotalPhys;
    }
};
#elif __linux__
class LinuxMemoryInfo : public IMemoryInfo {
public:
    unsigned long long getAvailableMemory() override {
        struct sysinfo memInfo;
        sysinfo(&memInfo);
        return (memInfo.freeram * memInfo.mem_unit);
    }

    unsigned long long getTotalMemory() override {
        struct sysinfo memInfo;
        sysinfo(&memInfo);
        return (memInfo.totalram * memInfo.mem_unit);
    }
};
#endif
