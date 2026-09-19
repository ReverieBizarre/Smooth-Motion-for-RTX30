#!/usr/bin/env python3
"""display_modes.py - report the *currently active* display mode per monitor.

Why this matters for the sm86_smooth flicker hunt: the whole cadence argument
depends on the refresh rate the compositor is actually running at, not on what
the panel supports. 24fps video + Smooth Motion doubling = 48 fps, which lands
on 144 Hz as a clean 3:1 hold, but on 60 Hz as an awkward 4:5 pattern.

Prints, for every monitor: device name, desktop rectangle, whether it is the
primary, the active resolution/refresh, and whether 144 Hz is available at all.
"""
import ctypes
from ctypes import wintypes

user32 = ctypes.windll.user32

ENUM_CURRENT_SETTINGS = -1
ENUM_REGISTRY_SETTINGS = -2
EDS_RAWMODE = 0x2
CCHDEVICENAME = 32
CCHFORMNAME = 32
MONITORINFOF_PRIMARY = 0x1
QDC_ONLY_ACTIVE_PATHS = 0x2


class DEVMODE(ctypes.Structure):
    _fields_ = [
        ("dmDeviceName", wintypes.WCHAR * CCHDEVICENAME),
        ("dmSpecVersion", wintypes.WORD),
        ("dmDriverVersion", wintypes.WORD),
        ("dmSize", wintypes.WORD),
        ("dmDriverExtra", wintypes.WORD),
        ("dmFields", wintypes.DWORD),
        ("dmOrientation", ctypes.c_short),
        ("dmPaperSize", ctypes.c_short),
        ("dmPaperLength", ctypes.c_short),
        ("dmPaperWidth", ctypes.c_short),
        ("dmScale", ctypes.c_short),
        ("dmCopies", ctypes.c_short),
        ("dmDefaultSource", ctypes.c_short),
        ("dmPrintQuality", ctypes.c_short),
        ("dmColor", ctypes.c_short),
        ("dmDuplex", ctypes.c_short),
        ("dmYResolution", ctypes.c_short),
        ("dmTTOption", ctypes.c_short),
        ("dmCollate", ctypes.c_short),
        ("dmFormName", wintypes.WCHAR * CCHFORMNAME),
        ("dmLogPixels", wintypes.WORD),
        ("dmBitsPerPel", wintypes.DWORD),
        ("dmPelsWidth", wintypes.DWORD),
        ("dmPelsHeight", wintypes.DWORD),
        ("dmDisplayFlags", wintypes.DWORD),
        ("dmDisplayFrequency", wintypes.DWORD),
        ("dmICMMethod", wintypes.DWORD),
        ("dmICMIntent", wintypes.DWORD),
        ("dmMediaType", wintypes.DWORD),
        ("dmDitherType", wintypes.DWORD),
        ("dmReserved1", wintypes.DWORD),
        ("dmReserved2", wintypes.DWORD),
        ("dmPanningWidth", wintypes.DWORD),
        ("dmPanningHeight", wintypes.DWORD),
    ]


class MONITORINFOEXW(ctypes.Structure):
    _fields_ = [
        ("cbSize", wintypes.DWORD),
        ("rcMonitor", wintypes.RECT),
        ("rcWork", wintypes.RECT),
        ("dwFlags", wintypes.DWORD),
        ("szDevice", wintypes.WCHAR * CCHDEVICENAME),
    ]


def modes(device):
    """All distinct (w, h, hz) modes the monitor advertises."""
    out = []
    i = 0
    while True:
        dm = DEVMODE()
        dm.dmSize = ctypes.sizeof(DEVMODE)
        if not user32.EnumDisplaySettingsExW(device, i, ctypes.byref(dm), EDS_RAWMODE):
            break
        out.append((dm.dmPelsWidth, dm.dmPelsHeight, dm.dmDisplayFrequency))
        i += 1
    return sorted(set(out), key=lambda t: (t[2], t[0], t[1]))


def main():
    monitors = []

    MONITORENUMPROC = ctypes.WINFUNCTYPE(
        ctypes.c_int, ctypes.c_void_p, ctypes.c_void_p,
        ctypes.POINTER(wintypes.RECT), ctypes.c_void_p)

    def cb(hmon, hdc, lprc, data):
        mi = MONITORINFOEXW()
        mi.cbSize = ctypes.sizeof(MONITORINFOEXW)
        user32.GetMonitorInfoW(hmon, ctypes.byref(mi))
        monitors.append(mi)
        return 1

    user32.EnumDisplayMonitors(None, None, MONITORENUMPROC(cb), None)

    print(f"monitors: {len(monitors)}\n")
    for mi in monitors:
        dev = mi.szDevice
        dm = DEVMODE()
        dm.dmSize = ctypes.sizeof(DEVMODE)
        ok = user32.EnumDisplaySettingsW(dev, ENUM_CURRENT_SETTINGS, ctypes.byref(dm))
        primary = "PRIMARY" if (mi.dwFlags & MONITORINFOF_PRIMARY) else "secondary"
        rect = mi.rcMonitor
        print(f"{dev:20s} {primary:10s} desktop=({rect.left},{rect.top})-({rect.right},{rect.bottom})")
        if ok:
            print(f"  ACTIVE MODE      : {dm.dmPelsWidth}x{dm.dmPelsHeight} @ "
                  f"{dm.dmDisplayFrequency} Hz, {dm.dmBitsPerPel}bpp")
        else:
            print("  ACTIVE MODE      : <query failed>")
        allm = modes(dev)
        hz = sorted({m[2] for m in allm})
        print(f"  available refresh: {hz}")
        if 144 in hz:
            at144 = [m for m in allm if m[2] == 144]
            print(f"  144Hz modes      : {at144}")
        hi = max(allm, key=lambda m: m[2])
        print(f"  highest mode     : {hi[0]}x{hi[1]} @ {hi[2]} Hz")
        print()


if __name__ == "__main__":
    main()
