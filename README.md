# botox

![menu](https://i.imgur.com/Mqn41VO.png)

## Compile

### Requirements

- **Visual Studio 2022** with the **Desktop development with C++** workload (MSVC v143 toolset + Windows 10/11 SDK).
- **DirectX SDK (June 2010)**: [download](https://www.microsoft.com/en-us/download/details.aspx?id=6812). The installer sets `DXSDK_DIR`, which the project uses for `d3dx9.lib`.
  If install fails with error `S1023`, uninstall the *Microsoft Visual C++ 2010 x86/x64 Redistributable* first, then retry.
- **vcpkg** with FreeType (32-bit, static, `/MD` runtime):

```bat
git clone https://github.com/microsoft/vcpkg C:\vcpkg
C:\vcpkg\bootstrap-vcpkg.bat
C:\vcpkg\vcpkg integrate install
C:\vcpkg\vcpkg install freetype:x86-windows-static-md
```

Everything else (ImGui, MinHook, json, WebView2, fonts) is already in `csgo-sdk/dependencies/`.

### Build

1. Open `csgo-sdk/csgo-sdk.sln`.
2. Pick **Release | x86** (Win32). CS:GO is 32-bit and the hooks use inline `__asm`, so x64 configs will not work.
3. Build → Build Solution (`Ctrl+Shift+B`).

Output: `csgo-sdk/release/botox.dll` (Debug | x86 gives `csgo-sdk/debug/deb.dll`).

Command line alternative, from a *Developer Command Prompt for VS 2022*:

```bat
msbuild csgo-sdk\csgo-sdk.sln /p:Configuration=Release /p:Platform=x86 /m
```

### Common errors

| Error | Fix |
|---|---|
| `cannot open include file 'd3dx9.h'` / `cannot open file 'd3dx9.lib'` | DirectX SDK June 2010 missing, or restart VS so it sees `DXSDK_DIR`. |
| `cannot open file 'freetype.lib'` / `ft2build.h` not found | Run `vcpkg integrate install` and install `freetype:x86-windows-static-md` (exact triplet). |
| `MSB8020: build tools for v143 cannot be found` | Install the MSVC v143 toolset from the VS installer. |
