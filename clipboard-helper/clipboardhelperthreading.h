#pragma once

// Where the clipboard helper runs its ClipboardSync engine.
//
// The engine moved to a dedicated worker thread so a pasteboard write blocked
// by contention can't wedge the helper's event loop (qiin2333#282). That is
// only valid where the native clipboard may be used from any thread: macOS
// (NSPasteboard is thread-safe). Qt's Windows clipboard is OLE-based and the
// X11/Wayland ones are event driven, so there every QClipboard call must stay
// on the GUI thread or writes silently never reach the system clipboard.
namespace ClipboardHelperThreading {

inline constexpr bool engineUsesWorkerThread()
{
#ifdef Q_OS_MACOS
    return true;
#else
    return false;
#endif
}

} // namespace ClipboardHelperThreading
