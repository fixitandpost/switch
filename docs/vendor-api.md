# Switch Vendor API

Switch registers an obs-websocket vendor named `Switch`. New integrations should
use namespaced requests and treat un-namespaced requests as compatibility aliases.

## Capabilities

Call `Switch.GetCapabilities` first.

Response fields:

- `success`: `true` when capabilities were produced.
- `apiVersion`: vendor API contract version. Current value: `1`.
- `version`: Switch plugin version.
- `obsVersion`: running OBS Studio version.
- `workspaceAvailable`: whether the Switch workspace UI/service is available.
- `modes`: `workspace`, `vertical`, `motion`, and `automation`.
- `vendorNamespaces`: supported request namespaces.
- `deprecatedVendorRequests`: legacy request names kept for compatibility.
- `motion`, `vertical`, `automation`: mode-specific capability objects.

## Response Contract

All new failure responses use:

```json
{
  "success": false,
  "code": "machine_readable_error",
  "message": "Human readable error"
}
```

Success responses keep their existing payloads and include `success: true`.

## Namespaces

Preferred request namespaces:

- `Switch.*`: product-level capabilities.
- `Workspace.*`: workspace grid and preview/program actions.
- `Canvas.*`: vertical canvas state, scenes, projectors, and links.
- `Output.*`: OBS output shortcut actions currently scoped to OBS frontend outputs.
- `Motion.*`: profiles, source bindings, shots, tracks, and runtime stats.
- `Automation.*`: macros, variables, queues, connections, and status.

## Compatibility Aliases

Legacy un-namespaced requests remain registered for existing clients. They are
listed in `deprecatedVendorRequests` and should not be used for new clients.

Examples include `GetCapabilities`, `GetRemoteState`, `ListSlots`, `Cut`,
`Auto`, `ListCanvases`, `CreateCanvasScene`, `ListMacros`, `CreateMacro`,
`TriggerMacro`, `SetVariable`, and `DeleteVariable`.

## Common Error Codes

- `workspace_unavailable`: Switch workspace services are not available.
- `unsupported_operation`: request is known but unsupported for the target surface.
- `unsupported_output_type`: output type is not supported.
- `recording_inactive`: recording-only action was requested while recording is inactive.
- `canvas_not_found`: requested vertical canvas was not found.
- `automation_macro_not_found`: requested automation macro was not found.
- `automation_macro_upsert_failed`: macro create/update failed validation or persistence.
- `motion_profile_not_found`: requested Motion profile was not found.
- `motion_shot_not_found`: requested Motion shot was not found.
- `motion_tracks_unavailable`: no active Motion tracks are available.
