# Switch

Switch is an OBS Studio plugin from Fix It & Post. It is built as one native OBS
tool with four top-level modes:

- **Workspace**: a multiview-style scene switcher with detachable source/view
  surfaces and quick projector/output controls.
- **Vertical**: a vertical-canvas workflow with linked scenes, projectors,
  windows, compact/dedicated OBS docks, and persisted vertical output settings.
- **Motion**: a native source-filter auto-framing engine with person detection,
  stable tracking IDs, subject lock/cycle controls, and smooth virtual PTZ
  pan/tilt/zoom.
- **Automation**: a macro system with triggers, actions, variables, queues,
  reusable connections, event log, import/export, and obs-websocket vendor APIs.

The plugin does not have a runtime dependency on external example repos.

## Product Surface

- `Tools -> Switch` opens the main Switch window.
- The main window uses a `Workspace / Vertical / Motion / Automation` shell.
- OBS docks expose focused Vertical surfaces:
  - `Switch Vertical`
  - `Switch Vertical Scenes`
  - `Switch Vertical Sources`
  - `Switch Vertical Transitions`
  - `Switch Vertical Settings`
- OBS Controls can expose quick Switch projector buttons for Multiview and
  Program outputs.
- Motion is applied through the `Switch Motion` OBS source filter, but normal
  configuration lives in the Motion tab.
- Vendor requests expose Workspace, Canvas, Output, Motion, and Automation
  control surfaces for obs-websocket/local remote workflows.
- `Switch.GetCapabilities` reports supported modes, namespaces, Motion backend
  options, and the current Vertical output-control scope.

## Current Production Notes

- Motion keeps its historical OBS filter id for compatibility with existing
  scenes, even though the user-facing name is `Switch Motion`.
- macOS Motion `Auto` selects the CoreML execution provider first. Apple decides
  the final CPU/GPU/ANE placement for CoreML execution; Switch does not promise
  ANE-only or zero-CPU inference.
- `SWITCH_MOTION_COREML_CPU_ONLY=1` forces CoreML CPU diagnostics mode.
- Windows Motion `Auto` selects the ONNX Runtime DirectML execution provider
  first, which targets the Windows GPU device through DirectML.
- Vertical output settings are persisted by Switch. Some output buttons still
  call OBS global frontend output APIs until a fully isolated vertical output
  backend is enabled.
- Motion model binaries are installer-managed. The repository keeps
  `data/models/manifest.json` for filename, parser, install location, and
  checksum metadata, but does not commit `.onnx`, `.pt`, `.mlmodel`, or
  `.mlpackage` artifacts.
- Installers can call the packaged
  `data/scripts/download-motion-model.ps1` on Windows or
  `script/download_motion_model.sh` on Unix-like systems to download and
  checksum the Motion model into the installed plugin data directory. The
  manifest carries the default pinned URL; `SWITCH_MOTION_MODEL_URL` can
  override it for signed private releases.

## Local Build

This repository follows the standalone OBS plugin workflow recommended by the
OBS Project, so it does not need to live inside the OBS Studio source tree.

For everyday macOS development, use the repo-local run loop:

```sh
./script/build_and_run.sh
```

That script will:

- gracefully stop OBS Studio if it is running
- recreate `build_macos` if the checkout path changed
- configure and build the `macos` preset
- copy the finished `switch.plugin` bundle into your local OBS plugin folder
- relaunch OBS Studio

Additional modes:

```sh
./script/build_and_run.sh --verify
./script/build_and_run.sh --logs
./script/build_and_run.sh --telemetry
./script/build_and_run.sh --debug
```

Raw configure and build steps:

```sh
cmake --preset macos
cmake --build --preset macos --parallel
ctest --test-dir build_macos --output-on-failure -C RelWithDebInfo
```

The built plugin bundle is available at:

```text
build_macos/RelWithDebInfo/switch.plugin
```

For release staging and packaging, install into a separate prefix instead of
your live OBS plugin directory:

```sh
cmake --install build_macos --config RelWithDebInfo --prefix "$PWD/release/RelWithDebInfo"
```

## Compatibility

| Target | Version |
| --- | --- |
| Minimum supported OBS Studio | `32.1.2` |
| Build baseline | `32.1.2` development headers |
| OBS dependency bundles | `2025-08-23` official `obs-deps` / Qt6 packages |

Switch is built for the OBS `32.x` UI and runtime model. OBS versions older
than `32.1.2` are unsupported.

## Architecture Notes

- [docs/example-integration-plan.md](docs/example-integration-plan.md)
  records the reference-plugin research and the current guardrails for
  Advanced Scene Switcher/Aitum-inspired features.
- [docs/vendor-api.md](docs/vendor-api.md) documents the obs-websocket vendor
  API contract, namespaced requests, and deprecated compatibility aliases.
- [docs/release-checklist.md](docs/release-checklist.md) records the packaged
  release verification path. Release acceptance must use an installed package,
  not only a development rundir.

## Maintained By

Fix It & Post

Repository: [github.com/fixitandpost/switch](https://github.com/fixitandpost/switch)

## References

- [OBS plugin docs](https://docs.obsproject.com/plugins)
- [OBS source API](https://docs.obsproject.com/reference-sources)
- [OBS frontend API](https://docs.obsproject.com/reference-frontend-api)
- [OBS plugin template](https://github.com/obsproject/obs-plugintemplate)
