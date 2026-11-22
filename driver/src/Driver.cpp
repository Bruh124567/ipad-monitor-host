// iPad Monitor virtual display driver (IddCx, user mode).
// Adds one virtual monitor on request. The request arrives as an IOCTL from vdctl.exe (and later
// from the host app) on the device interface declared in common/ipad_idd_ioctl.h.

#include "Driver.h"

#include <cstdarg>
#include <cstdio>

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "avrt.lib")
#pragma comment(lib, "ole32.lib")

using Microsoft::WRL::ComPtr;

namespace {

constexpr UINT kMinWidth = 320, kMaxWidth = 7680;
constexpr UINT kMinHeight = 240, kMaxHeight = 4320;
constexpr UINT kMinHz = 24, kMaxHz = 144;

void Log(const char* fmt, ...) {
  char buf[256];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(buf, sizeof(buf), fmt, ap);
  va_end(ap);
  OutputDebugStringA("[IpadMonitorIdd] ");
  OutputDebugStringA(buf);
  OutputDebugStringA("\n");
}

// The monitor-description callbacks don't receive a monitor handle, so the requested mode is shared here.
std::mutex g_modeMutex;
ModeRequest g_mode;

ModeRequest GetMode() {
  std::lock_guard<std::mutex> lk(g_modeMutex);
  return g_mode;
}
void SetMode(const ModeRequest& m) {
  std::lock_guard<std::mutex> lk(g_modeMutex);
  g_mode = m;
}

// Requested mode first (the preferred one), then common smaller modes the user may switch to.
std::vector<std::pair<UINT, UINT>> BuildResolutionList(UINT w, UINT h) {
  std::vector<std::pair<UINT, UINT>> list{{w, h}};
  static const UINT common[][2] = {{3840, 2160}, {2560, 1440}, {1920, 1080}, {1600, 900}, {1280, 720}, {1024, 768}};
  for (const auto& c : common) {
    if (c[0] <= w && c[1] <= h && !(c[0] == w && c[1] == h)) list.emplace_back(c[0], c[1]);
  }
  return list;
}

void FillSignalInfo(DISPLAYCONFIG_VIDEO_SIGNAL_INFO& mode, DWORD width, DWORD height, DWORD vsync, bool monitorMode) {
  mode.totalSize.cx = mode.activeSize.cx = width;
  mode.totalSize.cy = mode.activeSize.cy = height;
  mode.AdditionalSignalInfo.vSyncFreqDivider = monitorMode ? 0 : 1;
  mode.AdditionalSignalInfo.videoStandard = 255;
  mode.vSyncFreq.Numerator = vsync;
  mode.vSyncFreq.Denominator = mode.hSyncFreq.Denominator = 1;
  mode.hSyncFreq.Numerator = vsync * height;
  mode.scanLineOrdering = DISPLAYCONFIG_SCANLINE_ORDERING_PROGRESSIVE;
  mode.pixelRate = static_cast<UINT64>(vsync) * width * height;
}

void CreateTargetMode(IDDCX_TARGET_MODE& mode, UINT width, UINT height, UINT vsync) {
  mode = {};
  mode.Size = sizeof(mode);
  FillSignalInfo(mode.TargetVideoSignalInfo.targetVideoSignalInfo, width, height, vsync, false);
}

}  // namespace

// ---------------------------------------------------------------------------------------------
// Direct3D
// ---------------------------------------------------------------------------------------------

HRESULT Direct3DDevice::Init() {
  HRESULT hr = CreateDXGIFactory2(0, IID_PPV_ARGS(&dxgiFactory));
  if (FAILED(hr)) return hr;
  hr = dxgiFactory->EnumAdapterByLuid(adapterLuid, IID_PPV_ARGS(&adapter));
  if (FAILED(hr)) return hr;
  return D3D11CreateDevice(adapter.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT, nullptr, 0,
                           D3D11_SDK_VERSION, &device, nullptr, &deviceContext);
}

// ---------------------------------------------------------------------------------------------
// Swap chain processing
// ---------------------------------------------------------------------------------------------

SwapChainProcessor::SwapChainProcessor(IDDCX_SWAPCHAIN swapChain, std::shared_ptr<Direct3DDevice> device,
                                       HANDLE newFrameEvent)
    : swapChain_(swapChain), device_(std::move(device)), newFrameEvent_(newFrameEvent) {
  terminateEvent_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
  thread_ = CreateThread(nullptr, 0, RunThread, this, 0, nullptr);
}

SwapChainProcessor::~SwapChainProcessor() {
  if (terminateEvent_) SetEvent(terminateEvent_);
  if (thread_) {
    WaitForSingleObject(thread_, INFINITE);
    CloseHandle(thread_);
  }
  if (terminateEvent_) CloseHandle(terminateEvent_);
}

DWORD CALLBACK SwapChainProcessor::RunThread(LPVOID argument) {
  static_cast<SwapChainProcessor*>(argument)->Run();
  return 0;
}

void SwapChainProcessor::Run() {
  DWORD task = 0;
  HANDLE avTask = AvSetMmThreadCharacteristicsW(L"Distribution", &task);
  RunCore();
  // Deleting the swap chain when processing ends tells the OS to hand us a new one if it needs to.
  WdfObjectDelete(reinterpret_cast<WDFOBJECT>(swapChain_));
  swapChain_ = nullptr;
  if (avTask) AvRevertMmThreadCharacteristics(avTask);
}

void SwapChainProcessor::RunCore() {
  ComPtr<IDXGIDevice> dxgiDevice;
  if (FAILED(device_->device.As(&dxgiDevice))) return;

  IDARG_IN_SWAPCHAINSETDEVICE setDevice = {};
  setDevice.pDevice = dxgiDevice.Get();
  if (FAILED(IddCxSwapChainSetDevice(swapChain_, &setDevice))) return;

  for (;;) {
    IDARG_OUT_RELEASEANDACQUIREBUFFER buffer = {};
    HRESULT hr = IddCxSwapChainReleaseAndAcquireBuffer(swapChain_, &buffer);
    if (hr == E_PENDING) {
      HANDLE waitHandles[] = {newFrameEvent_, terminateEvent_};
      const DWORD result = WaitForMultipleObjects(ARRAYSIZE(waitHandles), waitHandles, FALSE, 16);
      if (result == WAIT_OBJECT_0 || result == WAIT_TIMEOUT) continue;  // new buffer (or just poll again)
      break;                                                           // terminate, or the wait failed
    } else if (SUCCEEDED(hr)) {
      // A frame is ready. We don't process it here; just drop our reference and tell the OS we're done.
      ComPtr<IDXGIResource> acquired;
      acquired.Attach(buffer.MetaData.pSurface);
      acquired.Reset();
      if (FAILED(IddCxSwapChainFinishedProcessingFrame(swapChain_))) break;
    } else {
      break;  // swap chain abandoned (e.g. display mode change or device lost)
    }
  }
}

// ---------------------------------------------------------------------------------------------
// Monitor and device contexts
// ---------------------------------------------------------------------------------------------

void MonitorContext::AssignSwapChain(IDDCX_SWAPCHAIN swapChain, LUID renderAdapter, HANDLE newFrameEvent) {
  processor_.reset();
  auto device = std::make_shared<Direct3DDevice>(renderAdapter);
  if (FAILED(device->Init())) {
    Log("Direct3D init failed; abandoning swap chain");
    WdfObjectDelete(reinterpret_cast<WDFOBJECT>(swapChain));  // makes the OS try again
    return;
  }
  processor_ = std::make_unique<SwapChainProcessor>(swapChain, device, newFrameEvent);
}

void MonitorContext::UnassignSwapChain() { processor_.reset(); }

void DeviceContext::InitAdapter() {
  IDDCX_ADAPTER_CAPS caps = {};
  caps.Size = sizeof(caps);
  caps.MaxMonitorsSupported = 1;
  caps.EndPointDiagnostics.Size = sizeof(caps.EndPointDiagnostics);
  caps.EndPointDiagnostics.GammaSupport = IDDCX_FEATURE_IMPLEMENTATION_NONE;
  caps.EndPointDiagnostics.TransmissionType = IDDCX_TRANSMISSION_TYPE_WIRED_OTHER;
  caps.EndPointDiagnostics.pEndPointFriendlyName = L"iPad Monitor Virtual Display";
  caps.EndPointDiagnostics.pEndPointManufacturerName = L"iPad Monitor";
  caps.EndPointDiagnostics.pEndPointModelName = L"iPad Monitor Virtual Display";

  IDDCX_ENDPOINT_VERSION version = {};
  version.Size = sizeof(version);
  version.MajorVer = 1;
  caps.EndPointDiagnostics.pFirmwareVersion = &version;
  caps.EndPointDiagnostics.pHardwareVersion = &version;

  WDF_OBJECT_ATTRIBUTES attr;
  WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&attr, DeviceContextWrapper);

  IDARG_IN_ADAPTER_INIT init = {};
  init.WdfDevice = device_;
  init.pCaps = &caps;
  init.ObjectAttributes = &attr;

  IDARG_OUT_ADAPTER_INIT out = {};
  const NTSTATUS status = IddCxAdapterInitAsync(&init, &out);
  if (NT_SUCCESS(status)) {
    adapter_ = out.AdapterObject;
    WdfObjectGet_DeviceContextWrapper(out.AdapterObject)->ctx = this;
  } else {
    Log("IddCxAdapterInitAsync failed: 0x%08X", static_cast<unsigned>(status));
  }
}

void DeviceContext::OnAdapterInitFinished(NTSTATUS status) {
  std::lock_guard<std::mutex> lk(mutex_);
  adapterReady_ = NT_SUCCESS(status);
  Log("Adapter init finished: 0x%08X", static_cast<unsigned>(status));
}

NTSTATUS DeviceContext::AddMonitor(const IpadMonAddMonitor& req) {
  if (req.width < kMinWidth || req.width > kMaxWidth || req.height < kMinHeight || req.height > kMaxHeight ||
      req.refreshHz < kMinHz || req.refreshHz > kMaxHz) {
    return STATUS_INVALID_PARAMETER;
  }
  std::lock_guard<std::mutex> lk(mutex_);
  if (!adapterReady_) return STATUS_DEVICE_NOT_READY;
  if (monitor_ != nullptr) return STATUS_INVALID_DEVICE_STATE;

  current_.width = req.width;
  current_.height = req.height;
  current_.hz = req.refreshHz;
  SetMode(current_);
  // A fresh serial number each time stops Windows reusing cached mode data from an earlier monitor.
  edid_ = BuildEdid(req.width, req.height, req.refreshHz, ++serial_);

  WDF_OBJECT_ATTRIBUTES attr;
  WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&attr, MonitorContextWrapper);
  attr.EvtCleanupCallback = [](WDFOBJECT object) {
    auto* wrapper = WdfObjectGet_MonitorContextWrapper(object);
    if (wrapper) wrapper->Cleanup();
  };

  IDDCX_MONITOR_INFO info = {};
  info.Size = sizeof(info);
  info.MonitorType = DISPLAYCONFIG_OUTPUT_TECHNOLOGY_HDMI;
  info.ConnectorIndex = 0;
  info.MonitorDescription.Size = sizeof(info.MonitorDescription);
  info.MonitorDescription.Type = IDDCX_MONITOR_DESCRIPTION_TYPE_EDID;
  info.MonitorDescription.DataSize = static_cast<UINT>(edid_.size());
  info.MonitorDescription.pData = edid_.data();
  CoCreateGuid(&info.MonitorContainerId);

  IDARG_IN_MONITORCREATE create = {};
  create.ObjectAttributes = &attr;
  create.pMonitorInfo = &info;

  IDARG_OUT_MONITORCREATE created = {};
  NTSTATUS status = IddCxMonitorCreate(adapter_, &create, &created);
  if (!NT_SUCCESS(status)) {
    Log("IddCxMonitorCreate failed: 0x%08X", static_cast<unsigned>(status));
    return status;
  }
  monitor_ = created.MonitorObject;
  WdfObjectGet_MonitorContextWrapper(monitor_)->ctx = new MonitorContext(monitor_);

  IDARG_OUT_MONITORARRIVAL arrival = {};
  status = IddCxMonitorArrival(monitor_, &arrival);
  if (!NT_SUCCESS(status)) {
    Log("IddCxMonitorArrival failed: 0x%08X", static_cast<unsigned>(status));
    WdfObjectDelete(reinterpret_cast<WDFOBJECT>(monitor_));
    monitor_ = nullptr;
    return status;
  }
  Log("Monitor added: %ux%u @ %u Hz", req.width, req.height, req.refreshHz);
  return STATUS_SUCCESS;
}

NTSTATUS DeviceContext::RemoveMonitor() {
  std::lock_guard<std::mutex> lk(mutex_);
  if (monitor_ == nullptr) return STATUS_INVALID_DEVICE_STATE;
  const NTSTATUS status = IddCxMonitorDeparture(monitor_);
  monitor_ = nullptr;  // IddCx releases the monitor object
  Log("Monitor removed: 0x%08X", static_cast<unsigned>(status));
  return status;
}

void DeviceContext::GetStatus(IpadMonStatus* out) {
  std::lock_guard<std::mutex> lk(mutex_);
  out->adapterReady = adapterReady_ ? 1 : 0;
  out->monitorPresent = monitor_ ? 1 : 0;
  out->width = monitor_ ? current_.width : 0;
  out->height = monitor_ ? current_.height : 0;
  out->refreshHz = monitor_ ? current_.hz : 0;
}

// ---------------------------------------------------------------------------------------------
// IddCx and WDF callbacks
// ---------------------------------------------------------------------------------------------

extern "C" DRIVER_INITIALIZE DriverEntry;
EVT_WDF_DRIVER_DEVICE_ADD IpadMonDeviceAdd;
EVT_WDF_DEVICE_D0_ENTRY IpadMonDeviceD0Entry;
EVT_IDD_CX_ADAPTER_INIT_FINISHED IpadMonAdapterInitFinished;
EVT_IDD_CX_ADAPTER_COMMIT_MODES IpadMonAdapterCommitModes;
EVT_IDD_CX_PARSE_MONITOR_DESCRIPTION IpadMonParseMonitorDescription;
EVT_IDD_CX_MONITOR_GET_DEFAULT_DESCRIPTION_MODES IpadMonMonitorGetDefaultModes;
EVT_IDD_CX_MONITOR_QUERY_TARGET_MODES IpadMonMonitorQueryModes;
EVT_IDD_CX_MONITOR_ASSIGN_SWAPCHAIN IpadMonMonitorAssignSwapChain;
EVT_IDD_CX_MONITOR_UNASSIGN_SWAPCHAIN IpadMonMonitorUnassignSwapChain;

extern "C" NTSTATUS DriverEntry(PDRIVER_OBJECT driverObject, PUNICODE_STRING registryPath) {
  WDF_DRIVER_CONFIG config;
  WDF_OBJECT_ATTRIBUTES attributes;
  WDF_OBJECT_ATTRIBUTES_INIT(&attributes);
  WDF_DRIVER_CONFIG_INIT(&config, IpadMonDeviceAdd);
  return WdfDriverCreate(driverObject, registryPath, &attributes, &config, WDF_NO_HANDLE);
}

// IddCx forwards device IOCTLs here. Same signature as EVT_WDF_IO_QUEUE_IO_DEVICE_CONTROL, but with the device first.
static VOID IpadMonDeviceIoControl(WDFDEVICE device, WDFREQUEST request, size_t outputLength, size_t inputLength,
                                   ULONG ioControlCode) {
  UNREFERENCED_PARAMETER(outputLength);
  UNREFERENCED_PARAMETER(inputLength);
  auto* wrapper = WdfObjectGet_DeviceContextWrapper(device);
  DeviceContext* ctx = wrapper ? wrapper->ctx : nullptr;
  NTSTATUS status = STATUS_INVALID_DEVICE_REQUEST;
  size_t info = 0;

  if (ctx) {
    switch (ioControlCode) {
      case IOCTL_IPADMON_ADD_MONITOR: {
        IpadMonAddMonitor* in = nullptr;
        status = WdfRequestRetrieveInputBuffer(request, sizeof(*in), reinterpret_cast<PVOID*>(&in), nullptr);
        if (NT_SUCCESS(status)) status = ctx->AddMonitor(*in);
        break;
      }
      case IOCTL_IPADMON_REMOVE_MONITOR:
        status = ctx->RemoveMonitor();
        break;
      case IOCTL_IPADMON_GET_STATUS: {
        IpadMonStatus* out = nullptr;
        status = WdfRequestRetrieveOutputBuffer(request, sizeof(*out), reinterpret_cast<PVOID*>(&out), nullptr);
        if (NT_SUCCESS(status)) {
          ctx->GetStatus(out);
          info = sizeof(*out);
        }
        break;
      }
      default:
        break;
    }
  }
  WdfRequestCompleteWithInformation(request, status, info);
}

_Use_decl_annotations_ NTSTATUS IpadMonDeviceAdd(WDFDRIVER driver, PWDFDEVICE_INIT deviceInit) {
  UNREFERENCED_PARAMETER(driver);

  WDF_PNPPOWER_EVENT_CALLBACKS pnp;
  WDF_PNPPOWER_EVENT_CALLBACKS_INIT(&pnp);
  pnp.EvtDeviceD0Entry = IpadMonDeviceD0Entry;
  WdfDeviceInitSetPnpPowerEventCallbacks(deviceInit, &pnp);

  IDD_CX_CLIENT_CONFIG config;
  IDD_CX_CLIENT_CONFIG_INIT(&config);
  config.EvtIddCxDeviceIoControl = IpadMonDeviceIoControl;
  config.EvtIddCxAdapterInitFinished = IpadMonAdapterInitFinished;
  config.EvtIddCxParseMonitorDescription = IpadMonParseMonitorDescription;
  config.EvtIddCxMonitorGetDefaultDescriptionModes = IpadMonMonitorGetDefaultModes;
  config.EvtIddCxMonitorQueryTargetModes = IpadMonMonitorQueryModes;
  config.EvtIddCxAdapterCommitModes = IpadMonAdapterCommitModes;
  config.EvtIddCxMonitorAssignSwapChain = IpadMonMonitorAssignSwapChain;
  config.EvtIddCxMonitorUnassignSwapChain = IpadMonMonitorUnassignSwapChain;

  NTSTATUS status = IddCxDeviceInitConfig(deviceInit, &config);
  if (!NT_SUCCESS(status)) return status;

  WDF_OBJECT_ATTRIBUTES attr;
  WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&attr, DeviceContextWrapper);
  attr.EvtCleanupCallback = [](WDFOBJECT object) {
    auto* wrapper = WdfObjectGet_DeviceContextWrapper(object);
    if (wrapper) wrapper->Cleanup();
  };

  WDFDEVICE device = nullptr;
  status = WdfDeviceCreate(&deviceInit, &attr, &device);
  if (!NT_SUCCESS(status)) return status;

  status = IddCxDeviceInitialize(device);
  if (!NT_SUCCESS(status)) return status;

  // Lets vdctl.exe and the host app find this device.
  status = WdfDeviceCreateDeviceInterface(device, &kIpadMonInterface, nullptr);
  if (!NT_SUCCESS(status)) return status;

  WdfObjectGet_DeviceContextWrapper(device)->ctx = new DeviceContext(device);
  return STATUS_SUCCESS;
}

_Use_decl_annotations_ NTSTATUS IpadMonDeviceD0Entry(WDFDEVICE device, WDF_POWER_DEVICE_STATE previousState) {
  UNREFERENCED_PARAMETER(previousState);
  auto* wrapper = WdfObjectGet_DeviceContextWrapper(device);
  if (wrapper && wrapper->ctx) wrapper->ctx->InitAdapter();
  return STATUS_SUCCESS;
}

_Use_decl_annotations_ NTSTATUS IpadMonAdapterInitFinished(IDDCX_ADAPTER adapter,
                                                           const IDARG_IN_ADAPTER_INIT_FINISHED* inArgs) {
  auto* wrapper = WdfObjectGet_DeviceContextWrapper(adapter);
  if (wrapper && wrapper->ctx) wrapper->ctx->OnAdapterInitFinished(inArgs->AdapterInitStatus);
  return STATUS_SUCCESS;
}

_Use_decl_annotations_ NTSTATUS IpadMonAdapterCommitModes(IDDCX_ADAPTER adapter, const IDARG_IN_COMMITMODES* inArgs) {
  UNREFERENCED_PARAMETER(adapter);
  UNREFERENCED_PARAMETER(inArgs);
  return STATUS_SUCCESS;
}

_Use_decl_annotations_ NTSTATUS IpadMonParseMonitorDescription(const IDARG_IN_PARSEMONITORDESCRIPTION* inArgs,
                                                               IDARG_OUT_PARSEMONITORDESCRIPTION* outArgs) {
  // We generate the EDID ourselves, so the modes are known without parsing it.
  const ModeRequest req = GetMode();
  const auto list = BuildResolutionList(req.width, req.height);
  outArgs->MonitorModeBufferOutputCount = static_cast<UINT>(list.size());
  if (inArgs->MonitorModeBufferInputCount < list.size()) {
    return inArgs->MonitorModeBufferInputCount > 0 ? STATUS_BUFFER_TOO_SMALL : STATUS_SUCCESS;
  }
  for (size_t i = 0; i < list.size(); ++i) {
    IDDCX_MONITOR_MODE& mode = inArgs->pMonitorModes[i];
    mode = {};
    mode.Size = sizeof(IDDCX_MONITOR_MODE);
    mode.Origin = IDDCX_MONITOR_MODE_ORIGIN_MONITORDESCRIPTOR;
    FillSignalInfo(mode.MonitorVideoSignalInfo, list[i].first, list[i].second, req.hz, true);
  }
  outArgs->PreferredMonitorModeIdx = 0;
  return STATUS_SUCCESS;
}

_Use_decl_annotations_ NTSTATUS IpadMonMonitorGetDefaultModes(IDDCX_MONITOR monitor,
                                                              const IDARG_IN_GETDEFAULTDESCRIPTIONMODES* inArgs,
                                                              IDARG_OUT_GETDEFAULTDESCRIPTIONMODES* outArgs) {
  UNREFERENCED_PARAMETER(monitor);
  UNREFERENCED_PARAMETER(inArgs);
  UNREFERENCED_PARAMETER(outArgs);
  return STATUS_NOT_IMPLEMENTED;  // monitors here always come with an EDID
}

_Use_decl_annotations_ NTSTATUS IpadMonMonitorQueryModes(IDDCX_MONITOR monitor, const IDARG_IN_QUERYTARGETMODES* inArgs,
                                                         IDARG_OUT_QUERYTARGETMODES* outArgs) {
  UNREFERENCED_PARAMETER(monitor);
  const ModeRequest req = GetMode();
  const auto list = BuildResolutionList(req.width, req.height);
  std::vector<IDDCX_TARGET_MODE> modes(list.size());
  for (size_t i = 0; i < list.size(); ++i) CreateTargetMode(modes[i], list[i].first, list[i].second, req.hz);
  outArgs->TargetModeBufferOutputCount = static_cast<UINT>(modes.size());
  if (inArgs->TargetModeBufferInputCount >= modes.size()) {
    std::copy(modes.begin(), modes.end(), inArgs->pTargetModes);
  }
  return STATUS_SUCCESS;
}

_Use_decl_annotations_ NTSTATUS IpadMonMonitorAssignSwapChain(IDDCX_MONITOR monitor, const IDARG_IN_SETSWAPCHAIN* inArgs) {
  auto* wrapper = WdfObjectGet_MonitorContextWrapper(monitor);
  if (wrapper && wrapper->ctx) {
    wrapper->ctx->AssignSwapChain(inArgs->hSwapChain, inArgs->RenderAdapterLuid, inArgs->hNextSurfaceAvailable);
  }
  return STATUS_SUCCESS;
}

_Use_decl_annotations_ NTSTATUS IpadMonMonitorUnassignSwapChain(IDDCX_MONITOR monitor) {
  auto* wrapper = WdfObjectGet_MonitorContextWrapper(monitor);
  if (wrapper && wrapper->ctx) wrapper->ctx->UnassignSwapChain();
  return STATUS_SUCCESS;
}
