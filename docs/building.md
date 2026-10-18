# Building WSL Tools

[Back to the README](../README.md)

Requirements: a Linux C++17 compiler with static C/C++ libraries, Windows Visual Studio 2022's **Desktop development with C++**, the Windows SDK, CMake (the VS component is supported), and PowerShell. The standard process/network/service views need no Node.js or Python runtime; the optional Node Inspector backend uses system Python 3 in the distro. The Windows plugin needs no Python, Node.js, or .NET application runtime.

From the Linux checkout:

```sh
./scripts/build-helper.sh
```

From Windows PowerShell, using the Windows-visible checkout path:

```powershell
.\scripts\build-windows.ps1
.\scripts\package.ps1
```

The first Windows build downloads the pinned upstream SDK source and checks its archive SHA-256. It derives public headers and the import library without building System Informer. Later builds can use `-SkipSdk`. Every Windows build still cleans native objects: this prevents stale header layouts across the WSL/Windows staging boundary. Matching PDB symbols are retained for crash diagnosis. The DLL is compiled and linked with Control Flow Guard support for hardened System Informer launches. Windows compilation is staged under `%LOCALAPPDATA%\WslTools\build`; `-BuildRoot` can select another staging directory.

Packaging requires **Inno Setup 6**: `package.ps1` finds `ISCC.exe` on `PATH` or in the standard Program Files installation folders. Use `-InnoCompiler 'C:\path\to\ISCC.exe'` to select it explicitly. The default produces both `WslTools-*-windows-x64.zip` and `WslTools-*-setup-x64.exe` (using the current version number instead of `*` of course); a missing compiler fails with installation guidance. Use `package.ps1 -SkipInstaller` to deliberately build only the ZIP. `scripts/build-installer.ps1 -PackageDirectory <prepared-payload> -OutputDirectory <output-folder>` can compile an installer from an already prepared package directory on local NTFS. Packaging and installer compilation stage under `%LOCALAPPDATA%\WslTools` before copying their final files to `dist/`, including when the checkout is on SSHFS.

`dist/` contains the DLL, observer, ZIP package and setup EXE; generated files and `.deps/` are ignored by Git. `scripts/build-helper.sh` refuses to silently publish a dynamically linked observer. The optional `CXX` environment variable selects another existing Linux compiler. The current Windows build target is x64, even though the helper source can also compile on ARM64.
