# Switch Release Verification Checklist

Switch releases must be verified from an installed package, not only from a
build tree or rundir.

## Windows Packaged Verification

1. Confirm OBS Studio is installed at `C:\Program Files\obs-studio`.
2. Confirm CMake, Go, Inno Setup `iscc`, and Visual Studio 2022 Build Tools are
   installed and available for the Windows preset.
3. Run:

   ```powershell
   pwsh -File .\script\verify_windows_release.ps1 -Configuration RelWithDebInfo
   ```

   Set `SWITCH_MOTION_MODEL_URL` only when testing a model URL override.
4. Confirm the script builds, packages, installs, downloads the Motion model,
   launches OBS, and finds
   `[Switch] loaded version` in the OBS log.
5. In OBS, open `Tools -> Switch`.
6. Verify the Workspace, Vertical, Motion, and Automation modes open without
   errors.
7. Open and close the Switch Vertical docks.
8. Add or inspect the `Switch Motion` source filter and confirm Windows auto
   mode reports DirectML.
9. Enable Remote, confirm its status is clear, copy the URL, and verify the
   packaged remote web UI loads.
10. Close OBS normally and confirm shutdown is clean.

## Package Contents

The package must include:

- `obs-plugins/64bit/switch.dll`
- `obs-plugins/64bit/onnxruntime.dll`
- `data/obs-plugins/switch/locale/en-US.ini`
- all locale files with matching key sets
- `data/obs-plugins/switch/models/manifest.json`
- `data/obs-plugins/switch/models/yolo26-nano.onnx` after installer model download
- `data/obs-plugins/switch/scripts/download-motion-model.ps1`
- `data/obs-plugins/switch/remote/switch-remote-helper.exe`
- `data/obs-plugins/switch/remote/web/index.html`

## Release Discipline

- Keep compatibility IDs and persisted state keys stable.
- Use `Switch.GetCapabilities` to validate vendor API shape.
- Treat un-namespaced vendor calls as deprecated compatibility aliases.
- Do not ship a release that only passed dev-rundir verification.
