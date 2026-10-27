# Move transition for OBS Studio

Plugin for OBS Studio to move source to a new position during scene transition

# OBS 33 Windows fork

This is a community fork of [Exeldro's Move Transition](https://github.com/exeldro/obs-move-transition),
based on upstream commit `c1f1e3d`. The fork version is **3.2.2**. Exeldro remains the
original author; this is not an official Exeldro release.

The Windows packages use the plugin layout introduced in OBS Studio 33. The module
name and all source/filter identifiers stay the same, so existing scenes keep using
Move. OBS 33 is the minimum version for these Windows packages. The packaging also
follows the layout announced for OBS 34; OBS 34 runtime compatibility has not been tested.

See [CHANGES-OBS33.md](CHANGES-OBS33.md) for fixes and verification details.
The preventive fixes cover filter cleanup, UDP socket failures, and UDP listener
lifetime/shutdown. Automated Windows integration tests use an isolated OBS 33 runtime.

# Installation

Close OBS before replacing a plugin. Keep a backup of your scene collection and of
the previous plugin if you need to return to an older OBS version.

## Windows, standard OBS installation

Use the fork's `move-transition-3.2.2-windows-x64-Installer.exe`, or extract the
`-programdata.zip` archive into `%ProgramData%\obs-studio\plugins`:

```text
C:\ProgramData\obs-studio\plugins\move-transition\
  move-transition.dll
  data\
    manifest.json
    locale\
```

The installer checks for OBS 33 or newer, installs to ProgramData, and removes only
Move's old DLL/PDB from the detected OBS installation and the old ProgramData
`bin\64bit` directory. A custom standard OBS installation can be selected with
`/OBSDIR="D:\Apps\obs-studio"`. Manual ZIP installation does not remove old files:
remove only `move-transition.dll` and `move-transition.pdb` from the old
`obs-plugins\64bit` or `move-transition\bin\64bit` location after backing them up.

## Windows, portable OBS

Extract `-portable.zip` into the portable OBS root (the directory containing `bin`).
It creates `plugins\move-transition\move-transition.dll` and
`plugins\move-transition\data`. Do not put it into `obs-plugins`, `core`, or
`bin\64bit`. Portable OBS ignores the standard ProgramData plugin directory.

Restart OBS and check that Move appears without the **Legacy** label. The manifest
identifies this build as **Move (OBS 33 fork)**. If a manually installed legacy copy
remains, OBS 33 prefers a successfully loaded copy in the new location.

These instructions follow the [official OBS migration guide](https://obsproject.com/kb/legacy-plugin-locations).

## Other platforms

The installation migration in this fork targets Windows. For official upstream
downloads on other platforms, see https://obsproject.com/forum/resources/move-transition.913/.

Or enter `flatpak install com.obsproject.Studio.Plugin.MoveTransition` on your terminal

# Build

## Windows (Visual Studio 2022, x64)

Install the C++ desktop workload, a Windows SDK 10.0.20348.0 or newer, CMake 3.26 or
newer, and PowerShell 7.2 or newer. The existing build specification pins the OBS
31.1 development libraries; the Windows package layout itself requires OBS 33.

```powershell
cmake --preset windows-x64
cmake --build --preset windows-x64 --config RelWithDebInfo
cmake --install build_x64 --config RelWithDebInfo --prefix release/RelWithDebInfo
```

To create the ZIP archives without Inno Setup:

```powershell
. .github/scripts/utils.pwsh/Windows-Package.ps1
New-WindowsPluginPackage -ProjectRoot $PWD -Configuration RelWithDebInfo
```

The CI packaging entry point `.github/scripts/Package-Windows.ps1` additionally
compiles the installer with Inno Setup 6. Run it in CI with `iscc` available on PATH.

## Other build methods

1. In-tree build
    - Build OBS Studio: https://obsproject.com/wiki/Install-Instructions
    - Check out this repository to plugins/move-transition
    - Add `add_subdirectory(move-transition)` to plugins/CMakeLists.txt
    - Rebuild OBS Studio

1. Stand-alone build (Linux only)
    - Verify that you have package with development files for OBS
    - Check out this repository and run `cmake -S . -B build -DBUILD_OUT_OF_TREE=On && cmake --build build`

# Donations
- [GitHub Sponsor](https://github.com/sponsors/exeldro)
- [Ko-fi](https://ko-fi.com/exeldro)
- [Patreon](https://www.patreon.com/Exeldro)
- [PayPal](https://www.paypal.me/exeldro)
