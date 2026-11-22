// Shared by the virtual display driver and the vdctl tool.
// Include <windows.h> before this header.
#pragma once

#include <guiddef.h>
#include <winioctl.h>

// Device interface the driver publishes so user-mode programs can find and talk to it.
inline const GUID kIpadMonInterface = {0x6c7b3f0e, 0x5f8a, 0x4a5e, {0x9a, 0x6d, 0x3c, 0x1f, 0x2b, 0x8e, 0x41, 0x7d}};

#define IPADMON_CTL(n) CTL_CODE(FILE_DEVICE_UNKNOWN, 0x800 + (n), METHOD_BUFFERED, FILE_ANY_ACCESS)
#define IOCTL_IPADMON_ADD_MONITOR IPADMON_CTL(1)     // in: IpadMonAddMonitor
#define IOCTL_IPADMON_REMOVE_MONITOR IPADMON_CTL(2)  // no data
#define IOCTL_IPADMON_GET_STATUS IPADMON_CTL(3)      // out: IpadMonStatus

struct IpadMonAddMonitor {
  UINT32 width;
  UINT32 height;
  UINT32 refreshHz;
};

struct IpadMonStatus {
  UINT32 adapterReady;
  UINT32 monitorPresent;
  UINT32 width;
  UINT32 height;
  UINT32 refreshHz;
};
