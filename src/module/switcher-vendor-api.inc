// Switch obs-websocket vendor API handlers. Included by switcher-dock.cpp inside the module translation unit.

static void vendor_set_error(obs_data_t *response_data, const char *code, const char *message);
static void vendor_set_string_array(obs_data_t *data, const char *name, std::initializer_list<const char *> values);

static void vendor_get_remote_state(obs_data_t *, obs_data_t *response_data, void *)
{
	obs_data_t *state = SwitcherRemoteManager::Instance()->BuildRemoteStateData();
	if (state) {
		obs_data_apply(response_data, state);
		obs_data_release(state);
	}
	obs_data_set_bool(response_data, "success", true);
}

static void vendor_list_slots(obs_data_t *, obs_data_t *response_data, void *)
{
	vendor_get_remote_state(nullptr, response_data, nullptr);
}

static void vendor_select_preview_slot(obs_data_t *request_data, obs_data_t *response_data, void *)
{
	const bool success =
		SwitcherRemoteManager::Instance()->SelectPreviewSlot(static_cast<int>(obs_data_get_int(request_data, "slotIndex")));
	obs_data_set_bool(response_data, "success", success);
}

static void vendor_cut(obs_data_t *, obs_data_t *response_data, void *)
{
	obs_data_set_bool(response_data, "success", SwitcherRemoteManager::Instance()->Cut());
}

static void vendor_auto(obs_data_t *, obs_data_t *response_data, void *)
{
	obs_data_set_bool(response_data, "success", SwitcherRemoteManager::Instance()->AutoTransition());
}

static void vendor_get_selected_slot_state(obs_data_t *, obs_data_t *response_data, void *)
{
	vendor_get_remote_state(nullptr, response_data, nullptr);
}

static void vendor_set_slot_visibility(obs_data_t *, obs_data_t *response_data, void *)
{
	vendor_set_error(response_data, "unsupported_operation", "Slot visibility is not supported for the workspace grid");
}

static void vendor_set_slot_mute(obs_data_t *, obs_data_t *response_data, void *)
{
	vendor_set_error(response_data, "unsupported_operation", "Slot mute is not supported for the workspace grid");
}

static void vendor_restart_remote(obs_data_t *, obs_data_t *response_data, void *)
{
	SwitcherRemoteManager::Instance()->Restart();
	obs_data_set_bool(response_data, "success", true);
}

static obs_data_array_t *vendor_string_array(std::initializer_list<const char *> values)
{
	obs_data_array_t *array = obs_data_array_create();
	for (const char *value : values) {
		obs_data_t *entry = obs_data_create();
		obs_data_set_string(entry, "id", value);
		obs_data_array_push_back(array, entry);
		obs_data_release(entry);
	}
	return array;
}

static void vendor_set_string_array(obs_data_t *data, const char *name, std::initializer_list<const char *> values)
{
	obs_data_array_t *array = vendor_string_array(values);
	obs_data_set_array(data, name, array);
	obs_data_array_release(array);
}

static void vendor_set_error(obs_data_t *response_data, const char *code, const char *message)
{
	obs_data_set_bool(response_data, "success", false);
	obs_data_set_string(response_data, "code", code);
	obs_data_set_string(response_data, "message", message);
}

static void vendor_get_switch_capabilities(obs_data_t *, obs_data_t *response_data, void *)
{
	obs_data_set_bool(response_data, "success", true);
	obs_data_set_string(response_data, "name", "Switch");
	obs_data_set_string(response_data, "version", PROJECT_VERSION);
	obs_data_set_int(response_data, "apiVersion", 1);
	obs_data_set_string(response_data, "obsVersion", obs_get_version_string());
	obs_data_set_bool(response_data, "workspaceAvailable", switcher_dock != nullptr);

	vendor_set_string_array(response_data, "modes", {"workspace", "vertical", "motion", "automation"});
	vendor_set_string_array(response_data, "vendorNamespaces",
				{"Switch", "Workspace", "Canvas", "Output", "Motion", "Automation"});
	vendor_set_string_array(response_data, "deprecatedVendorRequests",
				{"GetCapabilities", "GetRemoteState", "ListSlots", "SelectPreviewSlot", "Cut", "Auto",
				 "GetSelectedSlotState", "SetSlotVisibility", "SetSlotMute", "RestartRemote", "ListCanvases",
				 "SwitchCanvasScene", "CreateCanvasScene", "DeleteCanvasScene", "SetCanvasLink",
				 "OpenCanvasWindow", "OpenCanvasProjector", "PerformOutputAction", "ListMacros", "CreateMacro",
				 "DeleteMacro", "UpdateMacro", "SetMacroEnabled", "SetMacroPaused", "TriggerMacro",
				 "SetVariable", "DeleteVariable"});

	obs_data_t *motion = obs_data_create();
	obs_data_set_string(motion, "filterId", "switch_ai_tracker");
	obs_data_set_string(motion, "filterName", "Switch Motion");
	obs_data_set_string(motion, "modelOutputParser", "yolo26-end-to-end-nx6");
	obs_data_set_string(motion, "modelDistribution", "installer-download");
	obs_data_set_string(motion, "modelManifest", "models/manifest.json");
	obs_data_set_bool(motion, "renderPathNonBlocking", true);
	vendor_set_string_array(motion, "shotModes", {"ai_auto_frame", "keyframe_loop", "hybrid"});
	vendor_set_string_array(motion, "shotPlaybackModes",
				{"free_run", "restart_on_program", "pause_when_hidden", "cue_in_preview"});
#if defined(__APPLE__)
	vendor_set_string_array(motion, "backends", {"auto", "coreml", "cpu"});
	obs_data_set_string(motion, "autoBackend", "coreml");
#elif defined(_WIN32)
	vendor_set_string_array(motion, "backends", {"auto", "directml", "cpu"});
	obs_data_set_string(motion, "autoBackend", "directml");
#else
	vendor_set_string_array(motion, "backends", {"auto", "cpu"});
	obs_data_set_string(motion, "autoBackend", "cpu");
#endif
	obs_data_set_obj(response_data, "motion", motion);
	obs_data_release(motion);

	obs_data_t *vertical = obs_data_create();
	obs_data_set_bool(vertical, "isolatedOutputBackend", false);
	obs_data_set_string(vertical, "outputControlScope", "obs-global-frontend");
	obs_data_set_bool(vertical, "verticalCanvasState", switcher_dock != nullptr);
	obs_data_set_obj(response_data, "vertical", vertical);
	obs_data_release(vertical);

	obs_data_t *automation = obs_data_create();
	obs_data_set_bool(automation, "macros", true);
	obs_data_set_bool(automation, "variables", true);
	obs_data_set_bool(automation, "queues", true);
	obs_data_set_bool(automation, "connections", true);
	obs_data_set_obj(response_data, "automation", automation);
	obs_data_release(automation);
}

static bool vendor_require_workspace(obs_data_t *response_data)
{
	if (switcher_dock)
		return true;

	vendor_set_error(response_data, "workspace_unavailable", "Switch workspace is not available");
	return false;
}

static QString vendor_canvas_id_from_request(obs_data_t *request_data)
{
	const QString canvasId = QString::fromUtf8(obs_data_get_string(request_data, "canvasId"));
	return canvasId.isEmpty() && switcher_dock ? switcher_dock->VerticalCanvasId() : canvasId;
}

static void vendor_set_canvas_descriptor(obs_data_t *canvasData, const SwitchCanvasDescriptor &descriptor)
{
	obs_data_set_string(canvasData, "id", descriptor.id.toUtf8().constData());
	obs_data_set_string(canvasData, "name", descriptor.name.toUtf8().constData());
	obs_data_set_string(canvasData, "active_scene_uuid", descriptor.activeSceneUuid.toUtf8().constData());
	obs_data_set_string(canvasData, "active_scene_name", descriptor.activeSceneName.toUtf8().constData());
	obs_data_set_int(canvasData, "width", descriptor.size.width());
	obs_data_set_int(canvasData, "height", descriptor.size.height());
	obs_data_set_string(canvasData, "aspect_preset", descriptor.aspectPreset.toUtf8().constData());
	obs_data_set_bool(canvasData, "linked_scene_sync", descriptor.linkedSceneSync);
	obs_data_set_bool(canvasData, "follow_main_streaming", descriptor.followMainStreaming);
	obs_data_set_bool(canvasData, "follow_main_recording", descriptor.followMainRecording);
	obs_data_set_bool(canvasData, "follow_main_replay", descriptor.followMainReplay);
	obs_data_set_bool(canvasData, "follow_main_virtual_camera", descriptor.followMainVirtualCamera);
}

static bool vendor_output_type_supported(const QString &type)
{
	return type == QStringLiteral("recording") || type == QStringLiteral("streaming") ||
	       type == QStringLiteral("replay") || type == QStringLiteral("replay_buffer") ||
	       type == QStringLiteral("virtual_camera") || type == QStringLiteral("virtualcam");
}

static bool vendor_output_type_active(obs_data_t *state, const QString &type)
{
	if (!state)
		return false;
	if (type == QStringLiteral("recording"))
		return obs_data_get_bool(state, "recording_active");
	if (type == QStringLiteral("streaming"))
		return obs_data_get_bool(state, "streaming_active");
	if (type == QStringLiteral("replay") || type == QStringLiteral("replay_buffer"))
		return obs_data_get_bool(state, "replay_active");
	if (type == QStringLiteral("virtual_camera") || type == QStringLiteral("virtualcam"))
		return obs_data_get_bool(state, "virtual_camera_active");
	return false;
}

static QString vendor_toggle_action_for_output_type(const QString &type)
{
	if (type == QStringLiteral("recording"))
		return QStringLiteral("toggle_recording");
	if (type == QStringLiteral("streaming"))
		return QStringLiteral("toggle_streaming");
	if (type == QStringLiteral("replay") || type == QStringLiteral("replay_buffer"))
		return QStringLiteral("toggle_replay");
	if (type == QStringLiteral("virtual_camera") || type == QStringLiteral("virtualcam"))
		return QStringLiteral("toggle_virtual_camera");
	return {};
}

static void vendor_add_macro_to_response(obs_data_t *response_data, const SwitchAutomationMacro &macro)
{
	obs_data_t *macroData = SwitchAutomationMacroToObsData(macro);
	obs_data_set_obj(response_data, "macro", macroData);
	obs_data_release(macroData);
	obs_data_set_string(response_data, "macroId", macro.id.toUtf8().constData());
}

static void vendor_list_canvases(obs_data_t *, obs_data_t *response_data, void *)
{
	if (!vendor_require_workspace(response_data))
		return;

	obs_data_t *state = switcher_dock->BuildCanvasState();
	obs_data_apply(response_data, state);
	obs_data_release(state);
	obs_data_set_bool(response_data, "success", true);
}

static void vendor_get_canvas(obs_data_t *request_data, obs_data_t *response_data, void *)
{
	if (!vendor_require_workspace(response_data))
		return;

	const QString canvasId = vendor_canvas_id_from_request(request_data);
	const auto descriptor = switcher_dock->CanvasDescriptorForId(canvasId);
	if (descriptor.id.isEmpty()) {
		vendor_set_error(response_data, "canvas_not_found", "Canvas not found");
		return;
	}

	obs_data_t *canvasData = obs_data_create();
	vendor_set_canvas_descriptor(canvasData, descriptor);
	obs_data_set_obj(response_data, "canvas", canvasData);
	obs_data_release(canvasData);

	obs_data_array_t *sceneArray = obs_data_array_create();
	for (const auto &scene : switcher_dock->CanvasScenes(canvasId)) {
		obs_data_t *entry = obs_data_create();
		obs_data_set_string(entry, "uuid", scene.uuid.toUtf8().constData());
		obs_data_set_string(entry, "name", scene.name.toUtf8().constData());
		obs_data_array_push_back(sceneArray, entry);
		obs_data_release(entry);
	}
	obs_data_set_array(response_data, "scenes", sceneArray);
	obs_data_array_release(sceneArray);

	obs_data_array_t *sourceArray = obs_data_array_create();
	for (const auto &source : switcher_dock->CanvasSources(canvasId)) {
		obs_data_t *entry = obs_data_create();
		obs_data_set_int(entry, "id", source.id);
		obs_data_set_string(entry, "name", source.name.toUtf8().constData());
		obs_data_set_bool(entry, "visible", source.visible);
		obs_data_set_bool(entry, "locked", source.locked);
		obs_data_set_int(entry, "depth", source.depth);
		obs_data_set_bool(entry, "group", source.isGroup);
		obs_data_array_push_back(sourceArray, entry);
		obs_data_release(entry);
	}
	obs_data_set_array(response_data, "sources", sourceArray);
	obs_data_array_release(sourceArray);

	obs_data_array_t *linkArray = obs_data_array_create();
	if (auto *manager = switcher_dock->CanvasManager()) {
		for (const auto &link : manager->Links()) {
			obs_data_t *entry = obs_data_create();
			obs_data_set_string(entry, "main_scene_uuid", link.mainSceneUuid.toUtf8().constData());
			obs_data_set_string(entry, "main_scene_name", link.mainSceneName.toUtf8().constData());
			obs_data_set_string(entry, "target_scene_uuid", link.targetSceneUuid.toUtf8().constData());
			obs_data_set_string(entry, "target_scene_name", link.targetSceneName.toUtf8().constData());
			obs_data_array_push_back(linkArray, entry);
			obs_data_release(entry);
		}
	}
	obs_data_set_array(response_data, "links", linkArray);
	obs_data_array_release(linkArray);
	obs_data_set_bool(response_data, "success", true);
}

static void vendor_switch_canvas_scene(obs_data_t *request_data, obs_data_t *response_data, void *)
{
	if (!vendor_require_workspace(response_data))
		return;

	const QString canvasId = QString::fromUtf8(obs_data_get_string(request_data, "canvasId"));
	const QString sceneId = QString::fromUtf8(obs_data_get_string(request_data, "sceneId"));
	const QString sceneName = QString::fromUtf8(obs_data_get_string(request_data, "sceneName"));
	const bool success = switcher_dock->SwitchCanvasScene(canvasId, !sceneId.isEmpty() ? sceneId : sceneName);
	obs_data_set_bool(response_data, "success", success);
}

static void vendor_create_canvas_scene(obs_data_t *request_data, obs_data_t *response_data, void *)
{
	if (!vendor_require_workspace(response_data))
		return;

	const QString canvasId = QString::fromUtf8(obs_data_get_string(request_data, "canvasId"));
	const QString baseName = QString::fromUtf8(obs_data_get_string(request_data, "baseName"));
	QString createdName;
	const bool success = switcher_dock->CreateCanvasScene(canvasId, baseName, &createdName);
	obs_data_set_bool(response_data, "success", success);
	if (success && !createdName.isEmpty())
		obs_data_set_string(response_data, "sceneName", createdName.toUtf8().constData());
}

static void vendor_delete_canvas_scene(obs_data_t *request_data, obs_data_t *response_data, void *)
{
	if (!vendor_require_workspace(response_data))
		return;

	const QString canvasId = QString::fromUtf8(obs_data_get_string(request_data, "canvasId"));
	const QString sceneId = QString::fromUtf8(obs_data_get_string(request_data, "sceneId"));
	const QString sceneName = QString::fromUtf8(obs_data_get_string(request_data, "sceneName"));
	const bool success = switcher_dock->DeleteCanvasScene(canvasId, !sceneId.isEmpty() ? sceneId : sceneName);
	obs_data_set_bool(response_data, "success", success);
}

static void vendor_set_canvas_link(obs_data_t *request_data, obs_data_t *response_data, void *)
{
	if (!vendor_require_workspace(response_data))
		return;

	const QString mainSceneUuid = QString::fromUtf8(obs_data_get_string(request_data, "mainSceneUuid"));
	const QString targetSceneId = QString::fromUtf8(obs_data_get_string(request_data, "targetSceneId"));
	const QString targetSceneName = QString::fromUtf8(obs_data_get_string(request_data, "targetSceneName"));
	const bool success =
		switcher_dock->SetCanvasLink(mainSceneUuid, !targetSceneId.isEmpty() ? targetSceneId : targetSceneName);
	obs_data_set_bool(response_data, "success", success);
}

static void vendor_set_linked_scene(obs_data_t *request_data, obs_data_t *response_data, void *)
{
	vendor_set_canvas_link(request_data, response_data, nullptr);
}

static void vendor_open_canvas_window(obs_data_t *request_data, obs_data_t *response_data, void *)
{
	if (!vendor_require_workspace(response_data))
		return;

	const QString canvasId = QString::fromUtf8(obs_data_get_string(request_data, "canvasId"));
	obs_data_set_bool(response_data, "success", switcher_dock->OpenCanvasWindow(canvasId));
}

static void vendor_open_canvas_projector(obs_data_t *request_data, obs_data_t *response_data, void *)
{
	if (!vendor_require_workspace(response_data))
		return;

	const QString canvasId = QString::fromUtf8(obs_data_get_string(request_data, "canvasId"));
	obs_data_set_bool(response_data, "success", switcher_dock->OpenCanvasProjector(canvasId));
}

static void vendor_perform_output_action(obs_data_t *request_data, obs_data_t *response_data, void *)
{
	if (!vendor_require_workspace(response_data))
		return;

	const QString action = QString::fromUtf8(obs_data_get_string(request_data, "action"));
	const QString value = QString::fromUtf8(obs_data_get_string(request_data, "value"));
	obs_data_set_bool(response_data, "success", switcher_dock->PerformOutputAction(action, value));
}

static void vendor_get_canvas_output_state(obs_data_t *request_data, obs_data_t *response_data, void *)
{
	UNUSED_PARAMETER(request_data);
	if (!vendor_require_workspace(response_data))
		return;

	obs_data_t *state = switcher_dock->BuildCanvasState();
	obs_data_set_bool(response_data, "streaming_active", obs_data_get_bool(state, "streaming_active"));
	obs_data_set_bool(response_data, "recording_active", obs_data_get_bool(state, "recording_active"));
	obs_data_set_bool(response_data, "recording_paused", obs_data_get_bool(state, "recording_paused"));
	obs_data_set_bool(response_data, "replay_active", obs_data_get_bool(state, "replay_active"));
	obs_data_set_bool(response_data, "virtual_camera_active", obs_data_get_bool(state, "virtual_camera_active"));
	if (obs_data_t *output = obs_data_get_obj(state, "vertical_output")) {
		obs_data_set_obj(response_data, "vertical_output", output);
		obs_data_release(output);
	}
	obs_data_release(state);
	obs_data_set_bool(response_data, "success", true);
}

static void vendor_output_start(obs_data_t *request_data, obs_data_t *response_data, void *)
{
	if (!vendor_require_workspace(response_data))
		return;

	const QString type = QString::fromUtf8(obs_data_get_string(request_data, "type")).trimmed().toLower();
	if (!vendor_output_type_supported(type)) {
		vendor_set_error(response_data, "unsupported_output_type", "Unsupported output type");
		return;
	}

	obs_data_t *state = switcher_dock->BuildCanvasState();
	const bool active = vendor_output_type_active(state, type);
	obs_data_release(state);
	const bool success = active || switcher_dock->PerformOutputAction(vendor_toggle_action_for_output_type(type));
	obs_data_set_bool(response_data, "success", success);
}

static void vendor_output_stop(obs_data_t *request_data, obs_data_t *response_data, void *)
{
	if (!vendor_require_workspace(response_data))
		return;

	const QString type = QString::fromUtf8(obs_data_get_string(request_data, "type")).trimmed().toLower();
	if (!vendor_output_type_supported(type)) {
		vendor_set_error(response_data, "unsupported_output_type", "Unsupported output type");
		return;
	}

	obs_data_t *state = switcher_dock->BuildCanvasState();
	const bool active = vendor_output_type_active(state, type);
	obs_data_release(state);
	const bool success = !active || switcher_dock->PerformOutputAction(vendor_toggle_action_for_output_type(type));
	obs_data_set_bool(response_data, "success", success);
}

static void vendor_output_pause(obs_data_t *request_data, obs_data_t *response_data, void *)
{
	if (!vendor_require_workspace(response_data))
		return;

	const QString type = QString::fromUtf8(obs_data_get_string(request_data, "type")).trimmed().toLower();
	if (!type.isEmpty() && type != QStringLiteral("recording")) {
		vendor_set_error(response_data, "unsupported_output_type", "Pause is only supported for recording");
		return;
	}

	const bool targetPaused = !obs_data_has_user_value(request_data, "paused") || obs_data_get_bool(request_data, "paused");
	obs_data_t *state = switcher_dock->BuildCanvasState();
	const bool recordingActive = obs_data_get_bool(state, "recording_active");
	const bool recordingPaused = obs_data_get_bool(state, "recording_paused");
	obs_data_release(state);
	if (!recordingActive) {
		vendor_set_error(response_data, "recording_inactive", "Recording is not active");
		return;
	}
	const bool success = (recordingPaused == targetPaused) ||
			     switcher_dock->PerformOutputAction(QStringLiteral("toggle_recording_pause"));
	obs_data_set_bool(response_data, "success", success);
}

static void vendor_output_split(obs_data_t *, obs_data_t *response_data, void *)
{
	if (!vendor_require_workspace(response_data))
		return;
	obs_data_set_bool(response_data, "success", switcher_dock->PerformOutputAction(QStringLiteral("split_recording")));
}

static void vendor_output_chapter(obs_data_t *request_data, obs_data_t *response_data, void *)
{
	if (!vendor_require_workspace(response_data))
		return;
	const QString value = QString::fromUtf8(obs_data_get_string(request_data, "value"));
	obs_data_set_bool(response_data, "success",
			 switcher_dock->PerformOutputAction(QStringLiteral("add_recording_chapter"), value));
}

static void vendor_output_save_replay(obs_data_t *, obs_data_t *response_data, void *)
{
	if (!vendor_require_workspace(response_data))
		return;
	obs_data_set_bool(response_data, "success", switcher_dock->PerformOutputAction(QStringLiteral("save_replay")));
}

static void vendor_list_macros(obs_data_t *, obs_data_t *response_data, void *)
{
	if (!vendor_require_workspace(response_data))
		return;

	obs_data_t *state = switcher_dock->BuildAutomationState();
	obs_data_apply(response_data, state);
	obs_data_release(state);
	obs_data_set_bool(response_data, "success", true);
}

static void vendor_create_macro(obs_data_t *request_data, obs_data_t *response_data, void *)
{
	if (!vendor_require_workspace(response_data))
		return;

	const QString name = QString::fromUtf8(obs_data_get_string(request_data, "name"));
	const QString macroId = switcher_dock->CreateAutomationMacro(name);
	obs_data_set_bool(response_data, "success", !macroId.isEmpty());
	if (!macroId.isEmpty())
		obs_data_set_string(response_data, "macroId", macroId.toUtf8().constData());
}

static void vendor_delete_macro(obs_data_t *request_data, obs_data_t *response_data, void *)
{
	if (!vendor_require_workspace(response_data))
		return;

	const QString macroId = QString::fromUtf8(obs_data_get_string(request_data, "macroId"));
	obs_data_set_bool(response_data, "success", switcher_dock->DeleteAutomationMacro(macroId));
}

static void vendor_update_macro(obs_data_t *request_data, obs_data_t *response_data, void *)
{
	if (!vendor_require_workspace(response_data))
		return;

	SwitchMacroDescriptor macro;
	macro.id = QString::fromUtf8(obs_data_get_string(request_data, "macroId"));
	macro.name = QString::fromUtf8(obs_data_get_string(request_data, "name"));
	macro.group = QString::fromUtf8(obs_data_get_string(request_data, "group"));
	macro.enabled = obs_data_get_bool(request_data, "enabled");
	macro.paused = obs_data_get_bool(request_data, "paused");
	macro.runMode = QString::fromUtf8(obs_data_get_string(request_data, "runMode"));
	macro.triggerType = QString::fromUtf8(obs_data_get_string(request_data, "triggerType"));
	macro.intervalMs = int(obs_data_get_int(request_data, "intervalMs"));
	macro.desiredState = obs_data_get_bool(request_data, "desiredState");
	macro.triggerConnectionId = QString::fromUtf8(obs_data_get_string(request_data, "triggerConnectionId"));
	macro.triggerConnectionName = QString::fromUtf8(obs_data_get_string(request_data, "triggerConnectionName"));
	macro.triggerSceneUuid = QString::fromUtf8(obs_data_get_string(request_data, "triggerSceneUuid"));
	macro.triggerSceneName = QString::fromUtf8(obs_data_get_string(request_data, "triggerSceneName"));
	macro.triggerValueKey = QString::fromUtf8(obs_data_get_string(request_data, "triggerValueKey"));
	macro.triggerValue = QString::fromUtf8(obs_data_get_string(request_data, "triggerValue"));
	macro.actionType = QString::fromUtf8(obs_data_get_string(request_data, "actionType"));
	macro.actionConnectionId = QString::fromUtf8(obs_data_get_string(request_data, "actionConnectionId"));
	macro.actionConnectionName = QString::fromUtf8(obs_data_get_string(request_data, "actionConnectionName"));
	macro.actionSceneUuid = QString::fromUtf8(obs_data_get_string(request_data, "actionSceneUuid"));
	macro.actionSceneName = QString::fromUtf8(obs_data_get_string(request_data, "actionSceneName"));
	macro.actionValueKey = QString::fromUtf8(obs_data_get_string(request_data, "actionValueKey"));
	macro.actionValue = QString::fromUtf8(obs_data_get_string(request_data, "actionValue"));
	macro.actionDelayMs = int(obs_data_get_int(request_data, "actionDelayMs"));

	obs_data_set_bool(response_data, "success", switcher_dock->UpdateAutomationMacro(macro));
}

static void vendor_set_macro_enabled(obs_data_t *request_data, obs_data_t *response_data, void *)
{
	if (!vendor_require_workspace(response_data))
		return;

	const QString macroId = QString::fromUtf8(obs_data_get_string(request_data, "macroId"));
	const bool enabled = obs_data_get_bool(request_data, "enabled");
	obs_data_set_bool(response_data, "success", switcher_dock->SetAutomationMacroEnabled(macroId, enabled));
}

static void vendor_set_macro_paused(obs_data_t *request_data, obs_data_t *response_data, void *)
{
	if (!vendor_require_workspace(response_data))
		return;

	const QString macroId = QString::fromUtf8(obs_data_get_string(request_data, "macroId"));
	const bool paused = obs_data_get_bool(request_data, "paused");
	obs_data_set_bool(response_data, "success", switcher_dock->SetAutomationMacroPaused(macroId, paused));
}

static void vendor_trigger_macro(obs_data_t *request_data, obs_data_t *response_data, void *)
{
	if (!vendor_require_workspace(response_data))
		return;

	const QString macroId = QString::fromUtf8(obs_data_get_string(request_data, "macroId"));
	QString message;
	const bool success = switcher_dock->TriggerAutomationMacro(macroId, &message);
	obs_data_set_bool(response_data, "success", success);
	if (!message.isEmpty())
		obs_data_set_string(response_data, "message", message.toUtf8().constData());
}

static void vendor_set_variable(obs_data_t *request_data, obs_data_t *response_data, void *)
{
	if (!vendor_require_workspace(response_data))
		return;

	const QString key = QString::fromUtf8(obs_data_get_string(request_data, "key"));
	const QString value = QString::fromUtf8(obs_data_get_string(request_data, "value"));
	obs_data_set_bool(response_data, "success", switcher_dock->SetAutomationVariable(key, value));
}

static void vendor_delete_variable(obs_data_t *request_data, obs_data_t *response_data, void *)
{
	if (!vendor_require_workspace(response_data))
		return;

	const QString key = QString::fromUtf8(obs_data_get_string(request_data, "key"));
	obs_data_set_bool(response_data, "success", switcher_dock->DeleteAutomationVariable(key));
}

static void vendor_get_macro(obs_data_t *request_data, obs_data_t *response_data, void *)
{
	if (!vendor_require_workspace(response_data))
		return;

	const QString macroId = QString::fromUtf8(obs_data_get_string(request_data, "macroId"));
	const auto macro = switcher_dock->AutomationMacroDefinition(macroId);
	const bool success = !macro.id.isEmpty();
	obs_data_set_bool(response_data, "success", success);
	if (!success) {
		obs_data_set_string(response_data, "code", "automation_macro_not_found");
		obs_data_set_string(response_data, "message", "Automation macro not found");
		return;
	}

	vendor_add_macro_to_response(response_data, macro);
}

static void vendor_upsert_macro(obs_data_t *request_data, obs_data_t *response_data, void *)
{
	if (!vendor_require_workspace(response_data))
		return;

	QString effectiveId;
	bool success = false;

	if (obs_data_t *macroData = obs_data_get_obj(request_data, "macro")) {
		auto macro = SwitchAutomationMacroFromObsData(macroData);
		obs_data_release(macroData);
		success = switcher_dock->UpsertAutomationMacro(macro, &effectiveId);
	} else {
		SwitchMacroDescriptor macro;
		macro.id = QString::fromUtf8(obs_data_get_string(request_data, "macroId"));
		if (macro.id.isEmpty()) {
			const QString requestedName = QString::fromUtf8(obs_data_get_string(request_data, "name"));
			macro.id = switcher_dock->CreateAutomationMacro(requestedName);
		}
		macro.name = QString::fromUtf8(obs_data_get_string(request_data, "name"));
		macro.group = QString::fromUtf8(obs_data_get_string(request_data, "group"));
		macro.enabled = !obs_data_has_user_value(request_data, "enabled") || obs_data_get_bool(request_data, "enabled");
		macro.paused = obs_data_get_bool(request_data, "paused");
		macro.runMode = QString::fromUtf8(obs_data_get_string(request_data, "runMode"));
		macro.triggerType = QString::fromUtf8(obs_data_get_string(request_data, "triggerType"));
		macro.intervalMs = int(obs_data_get_int(request_data, "intervalMs"));
		macro.desiredState = !obs_data_has_user_value(request_data, "desiredState") ||
				     obs_data_get_bool(request_data, "desiredState");
		macro.triggerConnectionId = QString::fromUtf8(obs_data_get_string(request_data, "triggerConnectionId"));
		macro.triggerConnectionName = QString::fromUtf8(obs_data_get_string(request_data, "triggerConnectionName"));
		macro.triggerSceneUuid = QString::fromUtf8(obs_data_get_string(request_data, "triggerSceneUuid"));
		macro.triggerSceneName = QString::fromUtf8(obs_data_get_string(request_data, "triggerSceneName"));
		macro.triggerValueKey = QString::fromUtf8(obs_data_get_string(request_data, "triggerValueKey"));
		macro.triggerValue = QString::fromUtf8(obs_data_get_string(request_data, "triggerValue"));
		macro.actionType = QString::fromUtf8(obs_data_get_string(request_data, "actionType"));
		macro.actionConnectionId = QString::fromUtf8(obs_data_get_string(request_data, "actionConnectionId"));
		macro.actionConnectionName = QString::fromUtf8(obs_data_get_string(request_data, "actionConnectionName"));
		macro.actionSceneUuid = QString::fromUtf8(obs_data_get_string(request_data, "actionSceneUuid"));
		macro.actionSceneName = QString::fromUtf8(obs_data_get_string(request_data, "actionSceneName"));
		macro.actionValueKey = QString::fromUtf8(obs_data_get_string(request_data, "actionValueKey"));
		macro.actionValue = QString::fromUtf8(obs_data_get_string(request_data, "actionValue"));
		macro.actionDelayMs = int(obs_data_get_int(request_data, "actionDelayMs"));
		effectiveId = macro.id;
		success = !macro.id.isEmpty() && switcher_dock->UpdateAutomationMacro(macro);
	}

	obs_data_set_bool(response_data, "success", success);
	if (!success) {
		obs_data_set_string(response_data, "code", "automation_macro_upsert_failed");
		obs_data_set_string(response_data, "message", "Unable to upsert automation macro");
		return;
	}

	const auto macro = switcher_dock->AutomationMacroDefinition(effectiveId);
	if (!macro.id.isEmpty())
		vendor_add_macro_to_response(response_data, macro);
}

static void vendor_list_variables(obs_data_t *, obs_data_t *response_data, void *)
{
	if (!vendor_require_workspace(response_data))
		return;

	obs_data_t *state = switcher_dock->BuildAutomationState();
	if (obs_data_t *variables = obs_data_get_obj(state, "variables")) {
		obs_data_set_obj(response_data, "variables", variables);
		obs_data_release(variables);
	}
	obs_data_release(state);
	obs_data_set_bool(response_data, "success", true);
}

static void vendor_list_queues(obs_data_t *, obs_data_t *response_data, void *)
{
	if (!vendor_require_workspace(response_data))
		return;

	obs_data_t *state = switcher_dock->BuildAutomationState();
	if (obs_data_array_t *queues = obs_data_get_array(state, "queues")) {
		obs_data_set_array(response_data, "queues", queues);
		obs_data_array_release(queues);
	}
	obs_data_release(state);
	obs_data_set_bool(response_data, "success", true);
}

static void vendor_clear_queue(obs_data_t *request_data, obs_data_t *response_data, void *)
{
	if (!vendor_require_workspace(response_data))
		return;

	const QString queueId = QString::fromUtf8(obs_data_get_string(request_data, "queueId"));
	obs_data_set_bool(response_data, "success", switcher_dock->ClearAutomationQueue(queueId));
}

static void vendor_get_automation_status(obs_data_t *, obs_data_t *response_data, void *)
{
	if (!vendor_require_workspace(response_data))
		return;

	obs_data_t *state = switcher_dock->BuildAutomationState();
	obs_data_apply(response_data, state);
	obs_data_release(state);
	obs_data_set_bool(response_data, "success", true);
}

static void vendor_list_connections(obs_data_t *, obs_data_t *response_data, void *)
{
	if (!vendor_require_workspace(response_data))
		return;

	obs_data_t *state = switcher_dock->BuildAutomationState();
	if (obs_data_array_t *connections = obs_data_get_array(state, "connections")) {
		obs_data_set_array(response_data, "connections", connections);
		obs_data_array_release(connections);
	}
	obs_data_release(state);
	obs_data_set_bool(response_data, "success", true);
}

static void vendor_test_connection(obs_data_t *request_data, obs_data_t *response_data, void *)
{
	if (!vendor_require_workspace(response_data))
		return;

	const QString connectionId = QString::fromUtf8(obs_data_get_string(request_data, "connectionId"));
	QString message;
	const bool success = switcher_dock->TestAutomationConnection(connectionId, &message);
	obs_data_set_bool(response_data, "success", success);
	if (!message.isEmpty())
		obs_data_set_string(response_data, "message", message.toUtf8().constData());

	const auto connection = switcher_dock->AutomationConnection(connectionId);
	if (!connection.id.isEmpty()) {
		obs_data_t *state = switcher_dock->BuildAutomationState();
		if (obs_data_array_t *connections = obs_data_get_array(state, "connections")) {
			obs_data_set_array(response_data, "connections", connections);
			obs_data_array_release(connections);
		}
		obs_data_release(state);
	}
}

static void vendor_get_motion_state(obs_data_t *, obs_data_t *response_data, void *)
{
	if (!vendor_require_workspace(response_data))
		return;
	obs_data_t *state = switcher_dock->BuildMotionState();
	obs_data_set_obj(response_data, "state", state);
	obs_data_release(state);
	obs_data_set_bool(response_data, "success", true);
}

static void vendor_list_motion_profiles(obs_data_t *, obs_data_t *response_data, void *)
{
	if (!vendor_require_workspace(response_data))
		return;
	obs_data_t *state = switcher_dock->BuildMotionState();
	if (obs_data_array_t *profiles = obs_data_get_array(state, "profiles")) {
		obs_data_set_array(response_data, "profiles", profiles);
		obs_data_array_release(profiles);
	}
	obs_data_release(state);
	obs_data_set_bool(response_data, "success", true);
}

static void vendor_get_motion_profile(obs_data_t *request_data, obs_data_t *response_data, void *)
{
	if (!vendor_require_workspace(response_data))
		return;
	const QString profileId = QString::fromUtf8(obs_data_get_string(request_data, "profileId"));
	const auto profile = switcher_dock->MotionProfileDefinition(profileId);
	if (profile.id.isEmpty()) {
		vendor_set_error(response_data, "motion_profile_not_found", "Motion profile not found");
		return;
	}
	obs_data_t *profileData = SwitchMotionProfileToObsData(profile);
	obs_data_set_obj(response_data, "profile", profileData);
	obs_data_release(profileData);
	obs_data_set_bool(response_data, "success", true);
}

static void vendor_upsert_motion_profile(obs_data_t *request_data, obs_data_t *response_data, void *)
{
	if (!vendor_require_workspace(response_data))
		return;

	SwitchMotionProfile profile;
	if (obs_data_t *profileData = obs_data_get_obj(request_data, "profile")) {
		profile = SwitchMotionProfileFromObsData(profileData);
		obs_data_release(profileData);
	} else {
		const QString requestedId = QString::fromUtf8(obs_data_get_string(request_data, "profileId"));
		profile = requestedId.isEmpty() ? SwitchDefaultMotionProfile() : switcher_dock->MotionProfileDefinition(requestedId);
		if (profile.id.isEmpty())
			profile = SwitchDefaultMotionProfile();
		profile.id = requestedId.isEmpty() ? profile.id : requestedId;
		profile.name = QString::fromUtf8(obs_data_get_string(request_data, "name"));
		if (profile.name.isEmpty())
			profile.name = QStringLiteral("Motion Profile");
		if (obs_data_has_user_value(request_data, "enabled"))
			profile.enabled = obs_data_get_bool(request_data, "enabled");
		if (obs_data_has_user_value(request_data, "confidenceThreshold"))
			profile.confidenceThreshold = static_cast<float>(obs_data_get_double(request_data, "confidenceThreshold"));
		if (obs_data_has_user_value(request_data, "targetClassId"))
			profile.targetClassId = static_cast<int>(obs_data_get_int(request_data, "targetClassId"));
		if (obs_data_has_user_value(request_data, "maxZoom"))
			profile.maxZoom = static_cast<float>(obs_data_get_double(request_data, "maxZoom"));
		if (obs_data_has_user_value(request_data, "framingMargin"))
			profile.framingMargin = static_cast<float>(obs_data_get_double(request_data, "framingMargin"));
		if (obs_data_has_user_value(request_data, "deadZone"))
			profile.deadZone = static_cast<float>(obs_data_get_double(request_data, "deadZone"));
		if (obs_data_has_user_value(request_data, "smoothing"))
			profile.smoothing = static_cast<float>(obs_data_get_double(request_data, "smoothing"));
		if (obs_data_has_user_value(request_data, "holdMs"))
			profile.holdMs = static_cast<int>(obs_data_get_int(request_data, "holdMs"));
		if (obs_data_has_user_value(request_data, "backend"))
			profile.backend = QString::fromUtf8(obs_data_get_string(request_data, "backend"));
		if (obs_data_has_user_value(request_data, "modelPath"))
			profile.modelPath = QString::fromUtf8(obs_data_get_string(request_data, "modelPath"));
		if (obs_data_has_user_value(request_data, "subjectMode"))
			profile.subjectMode = QString::fromUtf8(obs_data_get_string(request_data, "subjectMode"));
		if (obs_data_has_user_value(request_data, "framingMode"))
			profile.framingMode = QString::fromUtf8(obs_data_get_string(request_data, "framingMode"));
		if (obs_data_has_user_value(request_data, "trackerHighThreshold"))
			profile.trackerHighThreshold = static_cast<float>(obs_data_get_double(request_data, "trackerHighThreshold"));
		if (obs_data_has_user_value(request_data, "trackerLowThreshold"))
			profile.trackerLowThreshold = static_cast<float>(obs_data_get_double(request_data, "trackerLowThreshold"));
		if (obs_data_has_user_value(request_data, "newTrackThreshold"))
			profile.newTrackThreshold = static_cast<float>(obs_data_get_double(request_data, "newTrackThreshold"));
		if (obs_data_has_user_value(request_data, "trackBufferFrames"))
			profile.trackBufferFrames = static_cast<int>(obs_data_get_int(request_data, "trackBufferFrames"));
		if (obs_data_has_user_value(request_data, "autoSwitchMs"))
			profile.autoSwitchMs = static_cast<int>(obs_data_get_int(request_data, "autoSwitchMs"));
		if (obs_data_has_user_value(request_data, "panResponsiveness"))
			profile.panResponsiveness = static_cast<float>(obs_data_get_double(request_data, "panResponsiveness"));
		if (obs_data_has_user_value(request_data, "tiltResponsiveness"))
			profile.tiltResponsiveness = static_cast<float>(obs_data_get_double(request_data, "tiltResponsiveness"));
		if (obs_data_has_user_value(request_data, "zoomResponsiveness"))
			profile.zoomResponsiveness = static_cast<float>(obs_data_get_double(request_data, "zoomResponsiveness"));
		if (obs_data_has_user_value(request_data, "maxPanSpeed"))
			profile.maxPanSpeed = static_cast<float>(obs_data_get_double(request_data, "maxPanSpeed"));
		if (obs_data_has_user_value(request_data, "maxTiltSpeed"))
			profile.maxTiltSpeed = static_cast<float>(obs_data_get_double(request_data, "maxTiltSpeed"));
		if (obs_data_has_user_value(request_data, "maxZoomSpeed"))
			profile.maxZoomSpeed = static_cast<float>(obs_data_get_double(request_data, "maxZoomSpeed"));
		if (obs_data_has_user_value(request_data, "debugOverlay"))
			profile.debugOverlay = obs_data_get_bool(request_data, "debugOverlay");
		if (obs_data_has_user_value(request_data, "lockedTrackId"))
			profile.lockedTrackId = static_cast<int>(obs_data_get_int(request_data, "lockedTrackId"));
	}

	QString effectiveId;
	const bool success = switcher_dock->UpsertMotionProfile(profile, &effectiveId);
	obs_data_set_bool(response_data, "success", success);
	obs_data_set_string(response_data, "profileId", effectiveId.toUtf8().constData());
	if (success) {
		obs_data_t *profileData = SwitchMotionProfileToObsData(switcher_dock->MotionProfileDefinition(effectiveId));
		obs_data_set_obj(response_data, "profile", profileData);
		obs_data_release(profileData);
	}
}

static void vendor_delete_motion_profile(obs_data_t *request_data, obs_data_t *response_data, void *)
{
	if (!vendor_require_workspace(response_data))
		return;
	const QString profileId = QString::fromUtf8(obs_data_get_string(request_data, "profileId"));
	obs_data_set_bool(response_data, "success", switcher_dock->DeleteMotionProfile(profileId));
}

static void vendor_set_motion_enabled(obs_data_t *request_data, obs_data_t *response_data, void *)
{
	if (!vendor_require_workspace(response_data))
		return;
	const QString profileId = QString::fromUtf8(obs_data_get_string(request_data, "profileId"));
	const bool enabled = obs_data_get_bool(request_data, "enabled");
	obs_data_set_bool(response_data, "success", switcher_dock->SetMotionEnabled(profileId, enabled));
}

static void vendor_bind_motion_source(obs_data_t *request_data, obs_data_t *response_data, void *)
{
	if (!vendor_require_workspace(response_data))
		return;
	const QString sourceUuid = QString::fromUtf8(obs_data_get_string(request_data, "sourceUuid"));
	const QString sourceName = QString::fromUtf8(obs_data_get_string(request_data, "sourceName"));
	const QString profileId = QString::fromUtf8(obs_data_get_string(request_data, "profileId"));
	QString message;
	const bool success = switcher_dock->BindMotionSource(sourceUuid, sourceName, profileId, &message);
	obs_data_set_bool(response_data, "success", success);
	if (!message.isEmpty())
		obs_data_set_string(response_data, "message", message.toUtf8().constData());
}

static void vendor_unbind_motion_source(obs_data_t *request_data, obs_data_t *response_data, void *)
{
	if (!vendor_require_workspace(response_data))
		return;
	const QString sourceUuid = QString::fromUtf8(obs_data_get_string(request_data, "sourceUuid"));
	QString message;
	const bool success = switcher_dock->UnbindMotionSource(sourceUuid, &message);
	obs_data_set_bool(response_data, "success", success);
	if (!message.isEmpty())
		obs_data_set_string(response_data, "message", message.toUtf8().constData());
}

static void vendor_list_motion_shots(obs_data_t *, obs_data_t *response_data, void *)
{
	if (!vendor_require_workspace(response_data))
		return;
	obs_data_t *state = switcher_dock->BuildMotionState();
	if (obs_data_array_t *shots = obs_data_get_array(state, "shots")) {
		obs_data_set_array(response_data, "shots", shots);
		obs_data_array_release(shots);
	}
	obs_data_release(state);
	obs_data_set_bool(response_data, "success", true);
}

static void vendor_get_motion_shot(obs_data_t *request_data, obs_data_t *response_data, void *)
{
	if (!vendor_require_workspace(response_data))
		return;
	const QString shotId = QString::fromUtf8(obs_data_get_string(request_data, "shotId"));
	const auto shot = switcher_dock->MotionShotDefinition(shotId);
	if (shot.id.isEmpty()) {
		vendor_set_error(response_data, "motion_shot_not_found", "Motion shot not found");
		return;
	}
	obs_data_t *shotData = SwitchMotionShotToObsData(shot);
	obs_data_set_obj(response_data, "shot", shotData);
	obs_data_release(shotData);
	obs_data_set_bool(response_data, "success", true);
}

static void vendor_upsert_motion_shot(obs_data_t *request_data, obs_data_t *response_data, void *)
{
	if (!vendor_require_workspace(response_data))
		return;

	SwitchMotionShot shot;
	if (obs_data_t *shotData = obs_data_get_obj(request_data, "shot")) {
		shot = SwitchMotionShotFromObsData(shotData);
		obs_data_release(shotData);
	} else {
		const QString requestedId = QString::fromUtf8(obs_data_get_string(request_data, "shotId"));
		shot = requestedId.isEmpty() ? SwitchDefaultMotionShot() : switcher_dock->MotionShotDefinition(requestedId);
		if (shot.id.isEmpty())
			shot = SwitchDefaultMotionShot();
		if (!requestedId.isEmpty())
			shot.id = requestedId;
		if (obs_data_has_user_value(request_data, "name"))
			shot.name = QString::fromUtf8(obs_data_get_string(request_data, "name"));
		if (obs_data_has_user_value(request_data, "enabled"))
			shot.enabled = obs_data_get_bool(request_data, "enabled");
		if (obs_data_has_user_value(request_data, "sceneUuid"))
			shot.sceneUuid = QString::fromUtf8(obs_data_get_string(request_data, "sceneUuid"));
		if (obs_data_has_user_value(request_data, "sceneName"))
			shot.sceneName = QString::fromUtf8(obs_data_get_string(request_data, "sceneName"));
		if (obs_data_has_user_value(request_data, "sceneItemId"))
			shot.sceneItemId = static_cast<int64_t>(obs_data_get_int(request_data, "sceneItemId"));
		if (obs_data_has_user_value(request_data, "sourceUuid"))
			shot.sourceUuid = QString::fromUtf8(obs_data_get_string(request_data, "sourceUuid"));
		if (obs_data_has_user_value(request_data, "sourceName"))
			shot.sourceName = QString::fromUtf8(obs_data_get_string(request_data, "sourceName"));
		if (obs_data_has_user_value(request_data, "profileId"))
			shot.profileId = QString::fromUtf8(obs_data_get_string(request_data, "profileId"));
		if (obs_data_has_user_value(request_data, "shotMode"))
			shot.shotMode = QString::fromUtf8(obs_data_get_string(request_data, "shotMode"));
		if (obs_data_has_user_value(request_data, "playbackMode"))
			shot.playbackMode = QString::fromUtf8(obs_data_get_string(request_data, "playbackMode"));
		if (obs_data_has_user_value(request_data, "startPanX"))
			shot.startPanX = static_cast<float>(obs_data_get_double(request_data, "startPanX"));
		if (obs_data_has_user_value(request_data, "startPanY"))
			shot.startPanY = static_cast<float>(obs_data_get_double(request_data, "startPanY"));
		if (obs_data_has_user_value(request_data, "startZoom"))
			shot.startZoom = static_cast<float>(obs_data_get_double(request_data, "startZoom"));
		if (obs_data_has_user_value(request_data, "endPanX"))
			shot.endPanX = static_cast<float>(obs_data_get_double(request_data, "endPanX"));
		if (obs_data_has_user_value(request_data, "endPanY"))
			shot.endPanY = static_cast<float>(obs_data_get_double(request_data, "endPanY"));
		if (obs_data_has_user_value(request_data, "endZoom"))
			shot.endZoom = static_cast<float>(obs_data_get_double(request_data, "endZoom"));
		if (obs_data_has_user_value(request_data, "durationMs"))
			shot.durationMs = static_cast<int>(obs_data_get_int(request_data, "durationMs"));
		if (obs_data_has_user_value(request_data, "easing"))
			shot.easing = QString::fromUtf8(obs_data_get_string(request_data, "easing"));
		if (obs_data_has_user_value(request_data, "loopMode"))
			shot.loopMode = QString::fromUtf8(obs_data_get_string(request_data, "loopMode"));
		if (obs_data_has_user_value(request_data, "maxZoom"))
			shot.maxZoom = static_cast<float>(obs_data_get_double(request_data, "maxZoom"));
	}

	QString effectiveId;
	const bool success = switcher_dock->UpsertMotionShot(shot, &effectiveId);
	obs_data_set_bool(response_data, "success", success);
	obs_data_set_string(response_data, "shotId", effectiveId.toUtf8().constData());
	if (success) {
		obs_data_t *shotData = SwitchMotionShotToObsData(switcher_dock->MotionShotDefinition(effectiveId));
		obs_data_set_obj(response_data, "shot", shotData);
		obs_data_release(shotData);
	}
}

static void vendor_delete_motion_shot(obs_data_t *request_data, obs_data_t *response_data, void *)
{
	if (!vendor_require_workspace(response_data))
		return;
	const QString shotId = QString::fromUtf8(obs_data_get_string(request_data, "shotId"));
	obs_data_set_bool(response_data, "success", switcher_dock->DeleteMotionShot(shotId));
}

static void vendor_bind_motion_scene_item(obs_data_t *request_data, obs_data_t *response_data, void *)
{
	if (!vendor_require_workspace(response_data))
		return;
	const QString shotId = QString::fromUtf8(obs_data_get_string(request_data, "shotId"));
	SwitchMotionShot shot = shotId.isEmpty() ? SwitchDefaultMotionShot() : switcher_dock->MotionShotDefinition(shotId);
	if (!shotId.isEmpty() && shot.id.isEmpty()) {
		vendor_set_error(response_data, "motion_shot_not_found", "Motion shot not found");
		return;
	}
	if (!shotId.isEmpty())
		shot.id = shotId;
	shot.sceneUuid = QString::fromUtf8(obs_data_get_string(request_data, "sceneUuid"));
	shot.sceneName = QString::fromUtf8(obs_data_get_string(request_data, "sceneName"));
	shot.sceneItemId = static_cast<int64_t>(obs_data_get_int(request_data, "sceneItemId"));
	shot.sourceUuid = QString::fromUtf8(obs_data_get_string(request_data, "sourceUuid"));
	shot.sourceName = QString::fromUtf8(obs_data_get_string(request_data, "sourceName"));
	if (obs_data_has_user_value(request_data, "profileId"))
		shot.profileId = QString::fromUtf8(obs_data_get_string(request_data, "profileId"));
	if (obs_data_has_user_value(request_data, "shotMode"))
		shot.shotMode = QString::fromUtf8(obs_data_get_string(request_data, "shotMode"));
	if (obs_data_has_user_value(request_data, "playbackMode"))
		shot.playbackMode = QString::fromUtf8(obs_data_get_string(request_data, "playbackMode"));
	QString effectiveId;
	QString message;
	const bool success = switcher_dock->BindMotionSceneItem(shot, &effectiveId, &message);
	obs_data_set_bool(response_data, "success", success);
	obs_data_set_string(response_data, "shotId", effectiveId.toUtf8().constData());
	if (!message.isEmpty())
		obs_data_set_string(response_data, "message", message.toUtf8().constData());
}

static void vendor_set_motion_shot_enabled(obs_data_t *request_data, obs_data_t *response_data, void *)
{
	if (!vendor_require_workspace(response_data))
		return;
	const QString shotId = QString::fromUtf8(obs_data_get_string(request_data, "shotId"));
	const bool enabled = obs_data_get_bool(request_data, "enabled");
	obs_data_set_bool(response_data, "success", switcher_dock->SetMotionShotEnabled(shotId, enabled));
}

static void vendor_set_motion_shot_playback(obs_data_t *request_data, obs_data_t *response_data, void *)
{
	if (!vendor_require_workspace(response_data))
		return;
	auto shot = switcher_dock->MotionShotDefinition(QString::fromUtf8(obs_data_get_string(request_data, "shotId")));
	if (shot.id.isEmpty()) {
		vendor_set_error(response_data, "motion_shot_not_found", "Motion shot not found");
		return;
	}
	shot.playbackMode = QString::fromUtf8(obs_data_get_string(request_data, "playbackMode"));
	QString effectiveId;
	const bool success = switcher_dock->UpsertMotionShot(shot, &effectiveId);
	obs_data_set_bool(response_data, "success", success);
	obs_data_set_string(response_data, "shotId", effectiveId.toUtf8().constData());
}

static void vendor_get_active_motion_shot(obs_data_t *, obs_data_t *response_data, void *)
{
	if (!vendor_require_workspace(response_data))
		return;
	obs_data_t *state = switcher_dock->BuildMotionState();
	if (obs_data_t *runtime = obs_data_get_obj(state, "runtime")) {
		obs_data_set_string(response_data, "shotId", obs_data_get_string(runtime, "activeShotId"));
		obs_data_set_string(response_data, "shotName", obs_data_get_string(runtime, "activeShotName"));
		obs_data_set_string(response_data, "sceneName", obs_data_get_string(runtime, "activeSceneName"));
		obs_data_set_string(response_data, "shotMode", obs_data_get_string(runtime, "activeShotMode"));
		obs_data_set_string(response_data, "playbackMode", obs_data_get_string(runtime, "activeShotPlaybackMode"));
		obs_data_set_int(response_data, "phaseMs", obs_data_get_int(runtime, "activeShotPhaseMs"));
		obs_data_set_double(response_data, "camX", obs_data_get_double(runtime, "activeShotCamX"));
		obs_data_set_double(response_data, "camY", obs_data_get_double(runtime, "activeShotCamY"));
		obs_data_set_double(response_data, "zoom", obs_data_get_double(runtime, "activeShotZoom"));
		obs_data_release(runtime);
	}
	obs_data_release(state);
	obs_data_set_bool(response_data, "success", true);
}

static void vendor_list_motion_shot_presets(obs_data_t *, obs_data_t *response_data, void *)
{
	if (!vendor_require_workspace(response_data))
		return;
	obs_data_array_t *presets = SwitchMotionShotPresetsToObsArray();
	obs_data_set_array(response_data, "presets", presets);
	obs_data_array_release(presets);
	obs_data_set_bool(response_data, "success", true);
}

static QString vendor_motion_profile_id(obs_data_t *request_data)
{
	QString profileId = QString::fromUtf8(obs_data_get_string(request_data, "profileId"));
	if (!profileId.isEmpty())
		return profileId;
	obs_data_t *state = switcher_dock ? switcher_dock->BuildMotionState() : nullptr;
	if (!state)
		return {};
	if (obs_data_array_t *profiles = obs_data_get_array(state, "profiles")) {
		if (obs_data_array_count(profiles) > 0) {
			obs_data_t *profile = obs_data_array_item(profiles, 0);
			profileId = QString::fromUtf8(obs_data_get_string(profile, "id"));
			obs_data_release(profile);
		}
		obs_data_array_release(profiles);
	}
	obs_data_release(state);
	return profileId;
}

static void vendor_list_motion_tracks(obs_data_t *, obs_data_t *response_data, void *)
{
	if (!vendor_require_workspace(response_data))
		return;
	obs_data_t *state = switcher_dock->BuildMotionState();
	if (obs_data_array_t *tracks = obs_data_get_array(state, "tracks")) {
		obs_data_set_array(response_data, "tracks", tracks);
		obs_data_array_release(tracks);
	}
	obs_data_release(state);
	obs_data_set_bool(response_data, "success", true);
}

static void vendor_get_motion_target(obs_data_t *, obs_data_t *response_data, void *)
{
	if (!vendor_require_workspace(response_data))
		return;
	obs_data_t *state = switcher_dock->BuildMotionState();
	if (obs_data_t *target = obs_data_get_obj(state, "target")) {
		obs_data_set_obj(response_data, "target", target);
		obs_data_release(target);
	}
	obs_data_release(state);
	obs_data_set_bool(response_data, "success", true);
}

static void vendor_get_motion_runtime_stats(obs_data_t *, obs_data_t *response_data, void *)
{
	if (!vendor_require_workspace(response_data))
		return;
	obs_data_t *state = switcher_dock->BuildMotionState();
	if (obs_data_t *runtime = obs_data_get_obj(state, "runtime")) {
		obs_data_set_obj(response_data, "runtime", runtime);
		obs_data_release(runtime);
	}
	obs_data_release(state);
	obs_data_set_bool(response_data, "success", true);
}

static void vendor_set_motion_target_track(obs_data_t *request_data, obs_data_t *response_data, void *)
{
	if (!vendor_require_workspace(response_data))
		return;
	const QString profileId = vendor_motion_profile_id(request_data);
	auto profile = switcher_dock->MotionProfileDefinition(profileId);
	if (profile.id.isEmpty()) {
		vendor_set_error(response_data, "motion_profile_not_found", "Motion profile not found");
		return;
	}
	profile.subjectMode = QStringLiteral("locked");
	profile.lockedTrackId = static_cast<int>(obs_data_get_int(request_data, "trackId"));
	QString effectiveId;
	const bool success = switcher_dock->UpsertMotionProfile(profile, &effectiveId);
	obs_data_set_bool(response_data, "success", success);
	obs_data_set_string(response_data, "profileId", effectiveId.toUtf8().constData());
	obs_data_set_int(response_data, "trackId", profile.lockedTrackId);
}

static void vendor_clear_motion_target_track(obs_data_t *request_data, obs_data_t *response_data, void *)
{
	if (!vendor_require_workspace(response_data))
		return;
	const QString profileId = vendor_motion_profile_id(request_data);
	auto profile = switcher_dock->MotionProfileDefinition(profileId);
	if (profile.id.isEmpty()) {
		vendor_set_error(response_data, "motion_profile_not_found", "Motion profile not found");
		return;
	}
	profile.subjectMode = QStringLiteral("auto");
	profile.lockedTrackId = -1;
	QString effectiveId;
	const bool success = switcher_dock->UpsertMotionProfile(profile, &effectiveId);
	obs_data_set_bool(response_data, "success", success);
	obs_data_set_string(response_data, "profileId", effectiveId.toUtf8().constData());
}

static void vendor_cycle_motion_target(obs_data_t *request_data, obs_data_t *response_data, void *)
{
	if (!vendor_require_workspace(response_data))
		return;
	obs_data_t *state = switcher_dock->BuildMotionState();
	obs_data_t *runtime = obs_data_get_obj(state, "runtime");
	obs_data_array_t *tracks = runtime ? obs_data_get_array(runtime, "tracks") : nullptr;
	QVector<int> activeIds;
	if (tracks) {
		const size_t count = obs_data_array_count(tracks);
		for (size_t index = 0; index < count; index++) {
			obs_data_t *track = obs_data_array_item(tracks, index);
			const QString trackState = QString::fromUtf8(obs_data_get_string(track, "state"));
			if (trackState == QStringLiteral("active") || trackState == QStringLiteral("new") ||
			    trackState == QStringLiteral("recovered"))
				activeIds.push_back(static_cast<int>(obs_data_get_int(track, "trackId")));
			obs_data_release(track);
		}
		obs_data_array_release(tracks);
	}
	const int currentTrackId = runtime ? static_cast<int>(obs_data_get_int(runtime, "targetTrackId")) : -1;
	if (runtime)
	obs_data_release(runtime);
	obs_data_release(state);
	if (activeIds.isEmpty()) {
		vendor_set_error(response_data, "motion_tracks_unavailable", "No active Motion tracks");
		return;
	}
	std::sort(activeIds.begin(), activeIds.end());
	const QString direction = QString::fromUtf8(obs_data_get_string(request_data, "direction"));
	const int step = direction == QStringLiteral("previous") || direction == QStringLiteral("prev") ? -1 : 1;
	const int currentIndex = activeIds.indexOf(currentTrackId);
	const int targetId = currentIndex < 0 ? (step > 0 ? activeIds.front() : activeIds.back())
					      : activeIds[(currentIndex + step + activeIds.size()) % activeIds.size()];
	obs_data_set_int(request_data, "trackId", targetId);
	vendor_set_motion_target_track(request_data, response_data, nullptr);
}

static void vendor_set_motion_subject_mode(obs_data_t *request_data, obs_data_t *response_data, void *)
{
	if (!vendor_require_workspace(response_data))
		return;
	const QString profileId = vendor_motion_profile_id(request_data);
	auto profile = switcher_dock->MotionProfileDefinition(profileId);
	if (profile.id.isEmpty()) {
		vendor_set_error(response_data, "motion_profile_not_found", "Motion profile not found");
		return;
	}
	const QString mode = QString::fromUtf8(obs_data_get_string(request_data, "subjectMode"));
	profile.subjectMode = mode.isEmpty() ? QStringLiteral("auto") : mode;
	if (profile.subjectMode != QStringLiteral("locked"))
		profile.lockedTrackId = -1;
	QString effectiveId;
	const bool success = switcher_dock->UpsertMotionProfile(profile, &effectiveId);
	obs_data_set_bool(response_data, "success", success);
	obs_data_set_string(response_data, "profileId", effectiveId.toUtf8().constData());
	obs_data_set_string(response_data, "subjectMode", profile.subjectMode.toUtf8().constData());
}

constexpr VendorRequestRegistration kVendorRequests[] = {
	{"GetCapabilities", vendor_get_switch_capabilities},
	{"Switch.GetCapabilities", vendor_get_switch_capabilities},
	{"GetRemoteState", vendor_get_remote_state},
	{"ListSlots", vendor_list_slots},
	{"SelectPreviewSlot", vendor_select_preview_slot},
	{"Cut", vendor_cut},
	{"Auto", vendor_auto},
	{"GetSelectedSlotState", vendor_get_selected_slot_state},
	{"SetSlotVisibility", vendor_set_slot_visibility},
	{"SetSlotMute", vendor_set_slot_mute},
	{"RestartRemote", vendor_restart_remote},
	{"Workspace.GetState", vendor_get_remote_state},
	{"Workspace.ListSlots", vendor_list_slots},
	{"Workspace.SelectPreviewSlot", vendor_select_preview_slot},
	{"Workspace.Cut", vendor_cut},
	{"Workspace.Auto", vendor_auto},
	{"Workspace.GetSelectedSlotState", vendor_get_selected_slot_state},
	{"Workspace.SetSlotVisibility", vendor_set_slot_visibility},
	{"Workspace.SetSlotMute", vendor_set_slot_mute},
	{"Workspace.RestartRemote", vendor_restart_remote},
	{"ListCanvases", vendor_list_canvases},
	{"SwitchCanvasScene", vendor_switch_canvas_scene},
	{"CreateCanvasScene", vendor_create_canvas_scene},
	{"DeleteCanvasScene", vendor_delete_canvas_scene},
	{"SetCanvasLink", vendor_set_canvas_link},
	{"OpenCanvasWindow", vendor_open_canvas_window},
	{"OpenCanvasProjector", vendor_open_canvas_projector},
	{"PerformOutputAction", vendor_perform_output_action},
	{"Canvas.List", vendor_list_canvases},
	{"Canvas.Get", vendor_get_canvas},
	{"Canvas.SwitchScene", vendor_switch_canvas_scene},
	{"Canvas.CreateScene", vendor_create_canvas_scene},
	{"Canvas.DeleteScene", vendor_delete_canvas_scene},
	{"Canvas.SetLink", vendor_set_canvas_link},
	{"Canvas.SetLinkedScene", vendor_set_linked_scene},
	{"Canvas.OpenWindow", vendor_open_canvas_window},
	{"Canvas.OpenProjector", vendor_open_canvas_projector},
	{"Canvas.GetOutputState", vendor_get_canvas_output_state},
	{"Output.PerformAction", vendor_perform_output_action},
	{"Output.Start", vendor_output_start},
	{"Output.Stop", vendor_output_stop},
	{"Output.Pause", vendor_output_pause},
	{"Output.Split", vendor_output_split},
	{"Output.Chapter", vendor_output_chapter},
	{"Output.SaveReplay", vendor_output_save_replay},
	{"ListMacros", vendor_list_macros},
	{"CreateMacro", vendor_create_macro},
	{"DeleteMacro", vendor_delete_macro},
	{"UpdateMacro", vendor_update_macro},
	{"SetMacroEnabled", vendor_set_macro_enabled},
	{"SetMacroPaused", vendor_set_macro_paused},
	{"TriggerMacro", vendor_trigger_macro},
	{"SetVariable", vendor_set_variable},
	{"DeleteVariable", vendor_delete_variable},
	{"Automation.ListMacros", vendor_list_macros},
	{"Automation.GetMacro", vendor_get_macro},
	{"Automation.UpsertMacro", vendor_upsert_macro},
	{"Automation.DeleteMacro", vendor_delete_macro},
	{"Automation.RunMacro", vendor_trigger_macro},
	{"Automation.SetPaused", vendor_set_macro_paused},
	{"Automation.SetEnabled", vendor_set_macro_enabled},
	{"Automation.ListVariables", vendor_list_variables},
	{"Automation.SetVariable", vendor_set_variable},
	{"Automation.DeleteVariable", vendor_delete_variable},
	{"Automation.ListQueues", vendor_list_queues},
	{"Automation.ClearQueue", vendor_clear_queue},
	{"Automation.GetStatus", vendor_get_automation_status},
	{"Automation.ListConnections", vendor_list_connections},
	{"Automation.TestConnection", vendor_test_connection},
	{"Motion.GetState", vendor_get_motion_state},
	{"Motion.ListProfiles", vendor_list_motion_profiles},
	{"Motion.GetProfile", vendor_get_motion_profile},
	{"Motion.UpsertProfile", vendor_upsert_motion_profile},
	{"Motion.DeleteProfile", vendor_delete_motion_profile},
	{"Motion.SetEnabled", vendor_set_motion_enabled},
	{"Motion.BindSource", vendor_bind_motion_source},
	{"Motion.UnbindSource", vendor_unbind_motion_source},
	{"Motion.ListShots", vendor_list_motion_shots},
	{"Motion.GetShot", vendor_get_motion_shot},
	{"Motion.UpsertShot", vendor_upsert_motion_shot},
	{"Motion.DeleteShot", vendor_delete_motion_shot},
	{"Motion.BindSceneItem", vendor_bind_motion_scene_item},
	{"Motion.SetShotEnabled", vendor_set_motion_shot_enabled},
	{"Motion.SetShotPlayback", vendor_set_motion_shot_playback},
	{"Motion.GetActiveShot", vendor_get_active_motion_shot},
	{"Motion.ListShotPresets", vendor_list_motion_shot_presets},
	{"Motion.ListTracks", vendor_list_motion_tracks},
	{"Motion.GetTarget", vendor_get_motion_target},
	{"Motion.SetTargetTrack", vendor_set_motion_target_track},
	{"Motion.ClearTargetTrack", vendor_clear_motion_target_track},
	{"Motion.CycleTarget", vendor_cycle_motion_target},
	{"Motion.SetSubjectMode", vendor_set_motion_subject_mode},
	{"Motion.GetRuntimeStats", vendor_get_motion_runtime_stats},
};
