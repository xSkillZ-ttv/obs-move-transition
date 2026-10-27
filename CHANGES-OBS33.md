# OBS 33 Windows fork (3.2.2)

Based on Exeldro's `c1f1e3dc7d517b73a2dcbed613f357e9d77349c1` (version 3.2.1).
This is a community fork, not an official upstream release.

## Changes

- Install the Windows module beside `data/` in
  `ProgramData/obs-studio/plugins/move-transition`, as required by the new OBS loader.
- Produce separate ProgramData and portable ZIPs. Portable packages contain
  `plugins/move-transition/`. Stage each archive from a clean directory and reject
  an old `bin/64bit` install tree rather than silently shipping mixed layouts.
- Install the optional OBS 33 manifest with the fork's version and repository.
- Keep the module name, source/filter identifiers, resources and original author.
- Check OBS 33+ separately from the plugin destination in the Windows installer.
  Do not reuse the old OBS installation path from an earlier uninstall record.
  Remove only Move's known legacy DLL/PDB after the new files are installed.
- Generate `version.h` in the build directory instead of modifying the source tree.

## Confirmed runtime fixes

1. **Wrong UDP array index:** four trigger-removal loops used the trigger index to
   index the server array. Removing/disabling a filter with multiple triggers or
   differently populated servers could read outside the array or leave a dangling
   filter registered. The bounds and empty checks now use the current server.
2. **Filter history memory leak:** destroying a filter did not release the
   `filters_done` array used by chained hotkeys. Destruction now releases it.
3. **Socket leak on bind failure:** if a UDP port was already in use, the worker
   returned without closing its socket. The failure path now closes it.
4. **UDP worker lifetime and shutdown:** listeners now own stable heap objects,
   so growing the registry cannot invalidate a worker's pointer. Socket setup and
   thread-creation failures roll back completely. Idle workers close their sockets,
   and shutdown joins workers before releasing shared state. Packet callbacks hold
   source references and run outside the registry mutex. A bounded socket wait
   replaces the internal shutdown datagrams, which could be lost during startup.

## Verification

- `python tests/test_runtime_regressions.py`: compiles the production function
  bodies in a focused C harness with OBS/network doubles. Nineteen scenarios passed
  locally using Zig 0.17.0 as a C compiler, including initialization failures,
  registry growth, reentrant callbacks, retry and shutdown. The original upstream
  code failed each of the initial three affected test categories before the fixes.
- `pwsh -File .github/tests/Test-WindowsPackage.ps1`: seven checks passed for ZIP
  contents, repeat packaging and invalid inputs. With `-IsccPath <ISCC.exe>`, an
  eighth check compiles the actual installer template with fixture files. It
  passed locally with Inno Setup 6.7.3. The installer was not executed.
- The new regression workflow runs both checks. The existing full build workflow
  compiles the plugin on Windows, macOS and Linux.
- The Windows build also runs `tests/smoke_obs33_windows.py --udp-stress` against
  the official OBS 33.0.0 runtime in an isolated directory. This checks plugin
  loading, metadata, locale resources, registered source types, simultaneous UDP
  listeners, port changes, bind failure/retry, and worker/socket cleanup. It does
  not open the OBS interface or use the user's settings.
  The stress scenario creates 194 filters across 32 ports, checks real matching
  and non-matching UDP packets, and requires zero UDP workers and released ports
  before Winsock cleanup. Headless graphics/frontend warnings are expected; this
  test covers the loader and UDP behavior, not rendered transitions.

## Limits of this review

This is a focused packaging and bug-fix review, not a guarantee that all bugs have
been found. OBS 34 runtime compatibility is untested; its announced Windows layout
is covered. macOS/Linux packaging has not been migrated by this change.

Scene transitions, hardware-specific NVIDIA tracking and DirectShow camera
controls still require end-to-end testing with the corresponding scenes/devices.
