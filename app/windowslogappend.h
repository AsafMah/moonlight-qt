#pragma once

#include <windows.h>

// Offset -1 asks Windows to select EOF atomically, including for synchronous
// handles owned by QFile. A separate seek-to-EOF races other processes.
inline bool appendWindowsLog(HANDLE file, const char* text, DWORD length)
{
    OVERLAPPED append = {};
    append.Offset = append.OffsetHigh = MAXDWORD;
    DWORD written = 0;
    return WriteFile(file, text, length, &written, &append) && written == length;
}
