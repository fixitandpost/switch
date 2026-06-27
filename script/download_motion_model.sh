#!/usr/bin/env bash
set -euo pipefail

usage() {
  printf 'Usage: SWITCH_MOTION_MODEL_URL=<url> %s <plugin-data-dir> [manifest]\n' "$0" >&2
}

if [[ $# -lt 1 || $# -gt 2 ]]; then
  usage
  exit 2
fi

if [[ -z "${SWITCH_MOTION_MODEL_URL:-}" ]]; then
  printf 'SWITCH_MOTION_MODEL_URL is required.\n' >&2
  exit 2
fi

plugin_data_dir=$1
manifest=${2:-"${plugin_data_dir}/models/manifest.json"}

if [[ ! -f "${manifest}" ]]; then
  printf 'Motion model manifest not found: %s\n' "${manifest}" >&2
  exit 2
fi

read_manifest_value() {
  local key=$1
  if command -v jq >/dev/null 2>&1; then
    jq -r ".models[0].${key}" "${manifest}"
    return
  fi
  if command -v plutil >/dev/null 2>&1; then
    plutil -extract "models.0.${key}" raw "${manifest}"
    return
  fi
  sed -n "s/.*\"${key}\"[[:space:]]*:[[:space:]]*\"\\([^\"]*\\)\".*/\\1/p" "${manifest}" | head -n 1
}

model_file=$(read_manifest_value file)
expected_sha=$(read_manifest_value sha256)

if [[ -z "${model_file}" || "${model_file}" == "null" || -z "${expected_sha}" || "${expected_sha}" == "null" ]]; then
  printf 'Motion model manifest is missing file or sha256.\n' >&2
  exit 2
fi

model_dir="${plugin_data_dir}/models"
destination="${model_dir}/${model_file}"
temporary="${destination}.download"

mkdir -p "${model_dir}"
curl --fail --location --show-error --output "${temporary}" "${SWITCH_MOTION_MODEL_URL}"

actual_sha=$(shasum -a 256 "${temporary}" | awk '{print $1}')
if [[ "${actual_sha}" != "${expected_sha}" ]]; then
  rm -f "${temporary}"
  printf 'Motion model checksum mismatch. expected=%s actual=%s\n' "${expected_sha}" "${actual_sha}" >&2
  exit 1
fi

mv "${temporary}" "${destination}"
chmod 0644 "${destination}"
printf 'Installed Switch Motion model: %s\n' "${destination}"
