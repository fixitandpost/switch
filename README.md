# Switcher

Switcher is an OBS Studio plugin from Fix It & Post that combines a
multiview-style scene switcher with detachable source control docks.

## What It Does

- Opens a Switcher workspace from the OBS `Tools` menu
- Shows 4, 9, 16, or 25 scene views in a multiview-style layout
- Lets you switch scenes directly from the grid
- Supports detached docks for per-source preview, media controls, audio
  controls, filters, properties, and scene item actions

## Local Build

This repository follows the standalone OBS plugin workflow recommended by the
OBS Project, so it does not need to live inside the OBS Studio source tree.

1. Configure the macOS build:

   `cmake --preset macos`

2. Build the plugin:

   `cmake --build build_macos --config RelWithDebInfo --parallel`

3. Install it into the default OBS plugin directory:

   `cmake --install build_macos --config RelWithDebInfo`

By default, the macOS install location is:

`~/Library/Application Support/obs-studio/plugins`

To stage an install somewhere else for inspection, override the prefix:

`cmake --install build_macos --config RelWithDebInfo --prefix "$PWD/release/RelWithDebInfo"`

## Compatibility

| Target | Version |
| --- | --- |
| Minimum supported OBS Studio | `32.1.2` |
| Build baseline | `32.1.2` development headers |
| OBS dependency bundles | `2025-08-23` official `obs-deps` / Qt6 packages |

Switcher is built for the OBS `32.x` UI and runtime model. OBS versions older
than `32.1.2` are unsupported.

## Notes

- Local builds use the official OBS Studio `32.1.2` development headers and the
  matching dependency versions defined in `buildspec.json`.
- If you want to use a non-default plugin location while testing, OBS also
  supports `OBS_PLUGINS_PATH` and `OBS_PLUGINS_DATA_PATH`.

## Maintained By

Fix It & Post

Repository: [github.com/fixitandpost/switcher](https://github.com/fixitandpost/switcher)

## References

- [OBS plugin docs](https://docs.obsproject.com/plugins)
- [OBS frontend API](https://docs.obsproject.com/reference-frontend-api)
- [OBS plugin template](https://github.com/obsproject/obs-plugintemplate)
