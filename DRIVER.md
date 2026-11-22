# iPad Monitor: virtual display driver

Adds a real second monitor to Windows. Nothing is sent to the iPad yet; this step only proves the driver loads
and Windows treats the virtual monitor as a display. Capture, encoding and decoding come later

```
driver/        IddCx virtual display driver (C++, user-mode)
common/        header shared by the driver and the tool
tools/vdctl/   command-line tool: install the driver, add/remove the virtual monitor
```

## 0. One-time setup (administrator PowerShell)

1. Visual Studio 2026 with "Desktop development with C++", plus the individual component **Windows Driver Kit**.
   Install the Windows SDK and WDK matching versions (WDK 10.0.28000.x) if the installer asks.
2. Turn on test-signing (Secure Boot must be off), then reboot:
   ```
   bcdedit /set testsigning on
   ```
   Windows then shows a "Test Mode" watermark in the corner of the desktop.

## 1. Build

Driver (Developer PowerShell for VS 2026, or open the .vcxproj in Visual Studio, Release, x64):
```
msbuild driver\IpadMonitorIdd.vcxproj /p:Configuration=Release /p:Platform=x64
```
Tool:
```
cd tools\vdctl
cmake -B build -G "Visual Studio 18 2026" -A x64
cmake --build build --config Release
```

## 2. Sign and install (administrator PowerShell)

```
.\driver\package-and-sign.ps1
.\tools\vdctl\build\Release\vdctl.exe install .\driver\package\IpadMonitorIdd.inf
.\tools\vdctl\build\Release\vdctl.exe status
```
Device Manager should list "iPad Monitor Virtual Display" under Display adapters with no warning icon,
and `status` should say `adapter ready: yes`.

## 3. Add the virtual monitor

```
vdctl add 1920 1080
```
A new display appears in Settings > System > Display (extend it, set scaling there). To try another size:
```
vdctl remove
vdctl add 2732 2048 60
```
`vdctl uninstall` removes the device again.
