#!/usr/bin/env bash
set -euo pipefail

MODE="${1:-run}"

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="$ROOT_DIR/build_macos"
BUILD_CONFIG="RelWithDebInfo"
BUILD_PRESET="macos"
PLUGIN_NAME="switch"
PLUGIN_BUNDLE="$BUILD_DIR/$BUILD_CONFIG/$PLUGIN_NAME.plugin"
OBS_PLUGIN_DIR="${OBS_PLUGIN_DIR:-$HOME/Library/Application Support/obs-studio/plugins}"
DEST_BUNDLE="$OBS_PLUGIN_DIR/$PLUGIN_NAME.plugin"
OBS_APP_BUNDLE_ID="com.obsproject.obs-studio"
OBS_PROCESS_NAME="OBS"
OBS_LOG_DIR="$HOME/Library/Application Support/obs-studio/logs"

usage() {
  echo "usage: $0 [run|--debug|--logs|--telemetry|--verify]" >&2
}

latest_obs_log() {
  find "$OBS_LOG_DIR" -type f -name '*.txt' -print0 2>/dev/null \
    | xargs -0 ls -t 2>/dev/null \
    | head -n 1
}

wait_for_obs_exit() {
  local attempts=0
  while pgrep -x "$OBS_PROCESS_NAME" >/dev/null 2>&1; do
    attempts=$((attempts + 1))
    if [[ "$attempts" -ge 20 ]]; then
      echo "OBS Studio did not exit after a normal quit request. Close it from the UI and run again." >&2
      exit 1
    fi
    sleep 0.5
  done
}

stop_obs() {
  if pgrep -x "$OBS_PROCESS_NAME" >/dev/null 2>&1; then
    osascript -e "tell application id \"$OBS_APP_BUNDLE_ID\" to quit" >/dev/null 2>&1 || true
    wait_for_obs_exit
  fi
}

ensure_configured() {
  local cache_file="$BUILD_DIR/CMakeCache.txt"

  if [[ -f "$cache_file" ]]; then
    local cache_root
    cache_root="$(sed -n 's/^CMAKE_HOME_DIRECTORY:INTERNAL=//p' "$cache_file")"
    if [[ "$cache_root" != "$ROOT_DIR" ]]; then
      rm -rf "$BUILD_DIR"
    fi
  fi

  cmake --preset "$BUILD_PRESET"
}

build_plugin() {
  cmake --build --preset "$BUILD_PRESET" --parallel
}

deploy_plugin() {
  if [[ ! -d "$PLUGIN_BUNDLE" ]]; then
    echo "Expected plugin bundle at $PLUGIN_BUNDLE" >&2
    exit 1
  fi

  mkdir -p "$OBS_PLUGIN_DIR"
  rm -rf "$DEST_BUNDLE"
  /usr/bin/ditto "$PLUGIN_BUNDLE" "$DEST_BUNDLE"
}

open_obs() {
  /usr/bin/open -b "$OBS_APP_BUNDLE_ID"
}

wait_for_obs_launch() {
  local attempts=0
  while ! pgrep -x "$OBS_PROCESS_NAME" >/dev/null 2>&1; do
    attempts=$((attempts + 1))
    if [[ "$attempts" -ge 20 ]]; then
      echo "OBS Studio did not launch in time" >&2
      exit 1
    fi
    sleep 0.5
  done
}

tail_obs_log() {
  local log_file=""
  local attempts=0

  while [[ -z "$log_file" ]]; do
    log_file="$(latest_obs_log || true)"
    attempts=$((attempts + 1))
    if [[ "$attempts" -ge 20 ]]; then
      echo "Could not locate an OBS log file under $OBS_LOG_DIR" >&2
      exit 1
    fi
    sleep 0.5
  done

  tail -n +1 -f "$log_file"
}

tail_switch_log() {
  if command -v rg >/dev/null 2>&1; then
    tail_obs_log | rg --line-buffered 'Switch|switch'
  else
    tail_obs_log | grep --line-buffered -E 'Switch|switch'
  fi
}

run_loop() {
  stop_obs
  ensure_configured
  build_plugin
  deploy_plugin
}

case "$MODE" in
  run)
    run_loop
    open_obs
    ;;
  --debug|debug)
    run_loop
    lldb -- /Applications/OBS.app/Contents/MacOS/OBS
    ;;
  --logs|logs)
    run_loop
    open_obs
    wait_for_obs_launch
    tail_obs_log
    ;;
  --telemetry|telemetry)
    run_loop
    open_obs
    wait_for_obs_launch
    tail_switch_log
    ;;
  --verify|verify)
    run_loop
    open_obs
    wait_for_obs_launch
    sleep 2
    test -d "$DEST_BUNDLE"
    plutil -extract CFBundleDisplayName raw -o - "$DEST_BUNDLE/Contents/Info.plist" | grep -qx "Switch"
    ;;
  *)
    usage
    exit 2
    ;;
esac
