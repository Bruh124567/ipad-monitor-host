// vdctl: installs the iPad Monitor virtual display driver and controls its virtual monitor.
//
//   vdctl install <path\to\IpadMonitorIdd.inf>   (administrator)
//   vdctl uninstall                              (administrator)
//   vdctl status
//   vdctl add <width> <height> [hz]              (default 60 Hz)
//   vdctl remove

#include <windows.h>
#include <newdev.h>
#include <setupapi.h>

#include <cstdio>
#include <cstdlib>
#include <cwchar>
#include <string>
#include <vector>

#include "../../common/ipad_idd_ioctl.h"

namespace {

const wchar_t kHardwareId[] = L"Root\\IpadMonitorIdd";
const GUID kDisplayClass = {0x4d36e968, 0xe325, 0x11ce, {0xbf, 0xc1, 0x08, 0x00, 0x2b, 0xe1, 0x03, 0x18}};

void PrintWin32Error(const char* what, DWORD code = GetLastError()) {
  std::fprintf(stderr, "%s failed (Win32 error %lu)\n", what, code);
  if (code == ERROR_ACCESS_DENIED) std::fprintf(stderr, "  Run this command from an administrator prompt.\n");
}

int Install(const wchar_t* infArg) {
  wchar_t inf[MAX_PATH];
  if (!GetFullPathNameW(infArg, MAX_PATH, inf, nullptr) || GetFileAttributesW(inf) == INVALID_FILE_ATTRIBUTES) {
    std::fprintf(stderr, "INF file not found: %ls\n", infArg);
    return 1;
  }
  GUID classGuid;
  wchar_t className[MAX_CLASS_NAME_LEN];
  if (!SetupDiGetINFClassW(inf, &classGuid, className, MAX_CLASS_NAME_LEN, nullptr)) {
    PrintWin32Error("SetupDiGetINFClass");
    return 1;
  }
  HDEVINFO set = SetupDiCreateDeviceInfoList(&classGuid, nullptr);
  if (set == INVALID_HANDLE_VALUE) {
    PrintWin32Error("SetupDiCreateDeviceInfoList");
    return 1;
  }
  SP_DEVINFO_DATA dev = {};
  dev.cbSize = sizeof(dev);
  int rc = 1;
  if (!SetupDiCreateDeviceInfoW(set, className, &classGuid, nullptr, nullptr, DICD_GENERATE_ID, &dev)) {
    PrintWin32Error("SetupDiCreateDeviceInfo");
  } else {
    wchar_t hwid[64] = {};  // the buffer stays zero-filled, so the multi-string ends with two NULs
    wcscpy_s(hwid, kHardwareId);
    const DWORD bytes = static_cast<DWORD>((wcslen(hwid) + 2) * sizeof(wchar_t));
    if (!SetupDiSetDeviceRegistryPropertyW(set, &dev, SPDRP_HARDWAREID, reinterpret_cast<const BYTE*>(hwid), bytes)) {
      PrintWin32Error("SetupDiSetDeviceRegistryProperty");
    } else if (!SetupDiCallClassInstaller(DIF_REGISTERDEVICE, set, &dev)) {
      PrintWin32Error("SetupDiCallClassInstaller(DIF_REGISTERDEVICE)");
    } else {
      BOOL reboot = FALSE;
      if (!UpdateDriverForPlugAndPlayDevicesW(nullptr, kHardwareId, inf, INSTALLFLAG_FORCE, &reboot)) {
        PrintWin32Error("UpdateDriverForPlugAndPlayDevices");
        std::fprintf(stderr, "  If the error is 0xE0000242 or about signatures: is test-signing on and is the driver signed?\n");
        SetupDiCallClassInstaller(DIF_REMOVE, set, &dev);  // don't leave an empty device node behind
      } else {
        std::printf("Driver installed.%s\n", reboot ? " A reboot is required." : "");
        rc = 0;
      }
    }
  }
  SetupDiDestroyDeviceInfoList(set);
  return rc;
}

int Uninstall() {
  HDEVINFO set = SetupDiGetClassDevsW(&kDisplayClass, nullptr, nullptr, DIGCF_PRESENT);
  if (set == INVALID_HANDLE_VALUE) {
    PrintWin32Error("SetupDiGetClassDevs");
    return 1;
  }
  int removed = 0;
  SP_DEVINFO_DATA dev = {};
  dev.cbSize = sizeof(dev);
  for (DWORD i = 0; SetupDiEnumDeviceInfo(set, i, &dev); ++i) {
    wchar_t ids[512] = {};
    if (!SetupDiGetDeviceRegistryPropertyW(set, &dev, SPDRP_HARDWAREID, nullptr, reinterpret_cast<PBYTE>(ids),
                                           sizeof(ids) - sizeof(wchar_t), nullptr)) {
      continue;
    }
    for (const wchar_t* p = ids; *p; p += wcslen(p) + 1) {
      if (_wcsicmp(p, kHardwareId) == 0) {
        if (SetupDiCallClassInstaller(DIF_REMOVE, set, &dev)) ++removed;
        else PrintWin32Error("DIF_REMOVE");
        break;
      }
    }
  }
  SetupDiDestroyDeviceInfoList(set);
  std::printf("Removed %d device(s). The driver package stays in the driver store; remove it with\n"
              "  pnputil /enum-drivers   then   pnputil /delete-driver oemNN.inf /uninstall\n", removed);
  return 0;
}

HANDLE OpenDriver() {
  HDEVINFO set = SetupDiGetClassDevsW(&kIpadMonInterface, nullptr, nullptr, DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
  if (set == INVALID_HANDLE_VALUE) return INVALID_HANDLE_VALUE;
  SP_DEVICE_INTERFACE_DATA ifd = {};
  ifd.cbSize = sizeof(ifd);
  HANDLE h = INVALID_HANDLE_VALUE;
  if (SetupDiEnumDeviceInterfaces(set, nullptr, &kIpadMonInterface, 0, &ifd)) {
    DWORD need = 0;
    SetupDiGetDeviceInterfaceDetailW(set, &ifd, nullptr, 0, &need, nullptr);
    std::vector<BYTE> buf(need);
    auto* detail = reinterpret_cast<SP_DEVICE_INTERFACE_DETAIL_DATA_W*>(buf.data());
    detail->cbSize = sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W);
    if (SetupDiGetDeviceInterfaceDetailW(set, &ifd, detail, need, nullptr, nullptr)) {
      h = CreateFileW(detail->DevicePath, GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                      OPEN_EXISTING, 0, nullptr);
    }
  }
  SetupDiDestroyDeviceInfoList(set);
  return h;
}

bool Control(DWORD code, void* in, DWORD inSize, void* out, DWORD outSize) {
  HANDLE h = OpenDriver();
  if (h == INVALID_HANDLE_VALUE) {
    const DWORD err = GetLastError();
    std::fprintf(stderr, "Can't open the driver (is it installed and running?). Win32 error %lu\n", err);
    if (err == ERROR_ACCESS_DENIED) std::fprintf(stderr, "  Try an administrator prompt.\n");
    return false;
  }
  DWORD returned = 0;
  const BOOL ok = DeviceIoControl(h, code, in, inSize, out, outSize, &returned, nullptr);
  const DWORD err = GetLastError();
  CloseHandle(h);
  if (!ok) {
    PrintWin32Error("DeviceIoControl", err);
    return false;
  }
  return true;
}

int Status() {
  IpadMonStatus st = {};
  if (!Control(IOCTL_IPADMON_GET_STATUS, nullptr, 0, &st, sizeof(st))) return 1;
  std::printf("adapter ready: %s\n", st.adapterReady ? "yes" : "no");
  if (st.monitorPresent) std::printf("virtual monitor: %ux%u @ %u Hz\n", st.width, st.height, st.refreshHz);
  else std::printf("virtual monitor: none\n");
  return 0;
}

int Add(UINT32 w, UINT32 h, UINT32 hz) {
  IpadMonAddMonitor req = {w, h, hz};
  if (!Control(IOCTL_IPADMON_ADD_MONITOR, &req, sizeof(req), nullptr, 0)) return 1;
  std::printf("Virtual monitor added: %ux%u @ %u Hz. Check Settings > System > Display.\n", w, h, hz);
  return 0;
}

int Remove() {
  if (!Control(IOCTL_IPADMON_REMOVE_MONITOR, nullptr, 0, nullptr, 0)) return 1;
  std::printf("Virtual monitor removed.\n");
  return 0;
}

void Usage() {
  std::printf("Usage:\n"
              "  vdctl install <IpadMonitorIdd.inf>   (administrator)\n"
              "  vdctl uninstall                      (administrator)\n"
              "  vdctl status\n"
              "  vdctl add <width> <height> [hz]\n"
              "  vdctl remove\n");
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
  if (argc < 2) { Usage(); return 1; }
  const std::wstring cmd = argv[1];
  if (cmd == L"install" && argc == 3) return Install(argv[2]);
  if (cmd == L"uninstall") return Uninstall();
  if (cmd == L"status") return Status();
  if (cmd == L"remove") return Remove();
  if (cmd == L"add" && (argc == 4 || argc == 5)) {
    const UINT32 w = static_cast<UINT32>(_wtoi(argv[2]));
    const UINT32 h = static_cast<UINT32>(_wtoi(argv[3]));
    const UINT32 hz = argc == 5 ? static_cast<UINT32>(_wtoi(argv[4])) : 60;
    return Add(w, h, hz);
  }
  Usage();
  return 1;
}
