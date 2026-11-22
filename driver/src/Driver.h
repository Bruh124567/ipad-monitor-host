#pragma once

#ifndef UMDF_USING_NTSTATUS
#define UMDF_USING_NTSTATUS
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <windows.h>
#include <bugcodes.h>
#include <wudfwdm.h>
#include <wdf.h>
#include <iddcx.h>

#include <avrt.h>
#include <d3d11_2.h>
#include <dxgi1_5.h>
#include <objbase.h>
#include <wrl/client.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <memory>
#include <mutex>
#include <vector>

#include "../../common/ipad_idd_ioctl.h"
#include "Edid.h"

struct ModeRequest {
  UINT width = 1920;
  UINT height = 1080;
  UINT hz = 60;
};

// A D3D11 device on the GPU that renders the virtual monitor's frames.
struct Direct3DDevice {
  explicit Direct3DDevice(LUID adapterLuid) : adapterLuid(adapterLuid) {}
  HRESULT Init();

  LUID adapterLuid;
  Microsoft::WRL::ComPtr<IDXGIFactory5> dxgiFactory;
  Microsoft::WRL::ComPtr<IDXGIAdapter1> adapter;
  Microsoft::WRL::ComPtr<ID3D11Device> device;
  Microsoft::WRL::ComPtr<ID3D11DeviceContext> deviceContext;
};

// Pulls frames out of the OS swap chain on its own thread. Step 2a just releases them again;
// frames are captured from user mode with the Desktop Duplication API in step 2b.
class SwapChainProcessor {
 public:
  SwapChainProcessor(IDDCX_SWAPCHAIN swapChain, std::shared_ptr<Direct3DDevice> device, HANDLE newFrameEvent);
  ~SwapChainProcessor();

 private:
  static DWORD CALLBACK RunThread(LPVOID argument);
  void Run();
  void RunCore();

  IDDCX_SWAPCHAIN swapChain_;
  std::shared_ptr<Direct3DDevice> device_;
  HANDLE newFrameEvent_;
  HANDLE terminateEvent_ = nullptr;
  HANDLE thread_ = nullptr;
};

class MonitorContext {
 public:
  explicit MonitorContext(IDDCX_MONITOR monitor) : monitor_(monitor) {}
  void AssignSwapChain(IDDCX_SWAPCHAIN swapChain, LUID renderAdapter, HANDLE newFrameEvent);
  void UnassignSwapChain();

 private:
  IDDCX_MONITOR monitor_;
  std::unique_ptr<SwapChainProcessor> processor_;
};

class DeviceContext {
 public:
  explicit DeviceContext(WDFDEVICE device) : device_(device) {}
  void InitAdapter();
  void OnAdapterInitFinished(NTSTATUS status);
  NTSTATUS AddMonitor(const IpadMonAddMonitor& request);
  NTSTATUS RemoveMonitor();
  void GetStatus(IpadMonStatus* out);

 private:
  WDFDEVICE device_;
  IDDCX_ADAPTER adapter_ = nullptr;
  IDDCX_MONITOR monitor_ = nullptr;
  std::mutex mutex_;
  bool adapterReady_ = false;
  UINT32 serial_ = 0;
  ModeRequest current_;
  std::array<uint8_t, 128> edid_{};
};

// WDF context wrappers (these must live at global scope).
struct DeviceContextWrapper {
  DeviceContext* ctx;
  void Cleanup() {
    delete ctx;
    ctx = nullptr;
  }
};
WDF_DECLARE_CONTEXT_TYPE(DeviceContextWrapper)

struct MonitorContextWrapper {
  MonitorContext* ctx;
  void Cleanup() {
    delete ctx;
    ctx = nullptr;
  }
};
WDF_DECLARE_CONTEXT_TYPE(MonitorContextWrapper)
