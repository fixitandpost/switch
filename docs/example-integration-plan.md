# Reference Integration Notes

This document records the reference-plugin research behind Switch. It is not a
runtime dependency list and it is not a plan to clone another plugin.

References studied:

- [Advanced Scene Switcher](https://github.com/WarmUpTill/SceneSwitcher) for
  macro orchestration, condition/action registries, import/export, remote
  control, and onboarding patterns.
- [Aitum Vertical](https://github.com/Aitum/obs-vertical-canvas) for separate
  canvas workflows, linked scenes, projector flows, output settings, and
  vertical-first operational UI.
- [norihiro/obs-face-tracker](https://github.com/norihiro/obs-face-tracker) and
  [royshil/obs-detect](https://github.com/royshil/obs-detect) for native OBS
  source-filter tracking/detection patterns.

The references are intentionally not runtime or build dependencies of Switch.

## Current Switch Product Shape

Switch is now organized around four top-level modes:

- `Workspace`: multiview-style scene switching, detachable views, projectors,
  per-view controls, and local remote support.
- `Vertical`: native vertical canvas management, linked scenes, projector/window
  flows, transition overrides, compact/dedicated OBS docks, and persisted
  vertical output settings.
- `Motion`: native source-filter auto-framing with YOLO26-style detection
  parsing, persistent track IDs, subject lock/cycle/clear controls, smooth
  virtual PTZ transforms, and runtime telemetry.
- `Automation`: macro runtime, variables, queues, reusable connections,
  OSC-backed triggers/actions, event log, import/export, and vendor APIs.

## Guardrails From The References

- OBS source filters are the right integration point for Motion because they
  attach behavior to the source that needs to be framed and can render without
  blocking the main UI.
- Workspace and Vertical should stay Switch-native. Aitum-style behavior depends
  on real canvas/output ownership, not just a detached preview.
- Automation should grow through registered condition/action families instead of
  one-off UI branches.
- Remote/vendor APIs should remain namespaced and additive. Keep compatibility
  aliases only long enough to migrate clients.
- Normal users should configure Motion from `Tools -> Switch -> Motion`; source
  filter properties are an advanced troubleshooting surface.

## What Switch Covers Now

The current implementation spans these main files:

- [`src/module/switcher-dock.cpp`](../src/module/switcher-dock.cpp):
  OBS module entrypoint, Tools menu, dock registration, frontend lifecycle, and
  obs-websocket vendor requests.
- [`src/workspace/switcher-workspace.cpp`](../src/workspace/switcher-workspace.cpp):
  main Switch UI, workspace grid, vertical UI, Motion UI, automation UI, and
  dock widgets.
- [`src/vertical/switch-canvas-manager.cpp`](../src/vertical/switch-canvas-manager.cpp):
  vertical canvas state, scene linking, transition overrides, and output state
  persistence.
- [`src/motion/switch-motion-manager.cpp`](../src/motion/switch-motion-manager.cpp):
  Motion profiles, source/filter binding, track state, serialization, and
  runtime telemetry.
- [`src/motion/switch-ai-tracker.cpp`](../src/motion/switch-ai-tracker.cpp):
  compatibility-named native OBS filter for Switch Motion inference and
  non-blocking render transforms.
- [`src/automation/switch-automation-engine.cpp`](../src/automation/switch-automation-engine.cpp):
  macro runtime and frontend/OSC-backed trigger/action execution.
- [`src/automation/switch-automation-model.cpp`](../src/automation/switch-automation-model.cpp):
  automation document model and serialization.
- [`src/automation/switch-osc.cpp`](../src/automation/switch-osc.cpp):
  OSC transport and parsing.

## Known Production Gaps

- `src/workspace/switcher-workspace.cpp` and `src/module/switcher-dock.cpp` are
  still large coordination files. They should continue to be split into domain
  widgets/controllers and per-domain vendor API handlers after the current
  behavior is stable.
- The Motion filter keeps the historical `switch_ai_tracker` OBS id to preserve
  scene compatibility. A future migration can add a new id only if existing
  scenes are upgraded safely.
- macOS Motion uses CoreML in Auto mode, but CoreML/ONNX Runtime provider flags
  do not guarantee ANE-only execution.
- Windows Motion uses ONNX Runtime DirectML in Auto mode and packages the
  runtime DLL beside the installed plugin binary.
- Vertical output controls persist Switch settings, but some start/stop buttons
  still operate OBS global outputs until the isolated vertical backend is built.
- Automation is usable but not full Advanced Scene Switcher parity. New
  condition/action families should be added incrementally with tests.

## Recommended Hardening Order

1. Keep lifecycle stability first: open/close Tools window, open/close docks,
   add/remove Motion filters, save scene data, and quit OBS cleanly.
2. Keep Motion tuning focused on smooth target continuity, no black edges, and
   safe filter-property behavior.
3. Split large source files by ownership once the live workflows are stable.
4. Keep model binaries installer-managed: source keeps only the manifest and
   checksum contract.
5. Keep Windows DirectML validation in the packaged release script so GPU
   runtime regressions are caught from the installed plugin.
