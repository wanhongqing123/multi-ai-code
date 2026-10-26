#!/usr/bin/env bash
# Explicit Android emulator canary against the dedicated production cloud bucket.
set -euo pipefail

if [[ "${1:-}" != "--execute-canary" || "$#" != 1 ]]; then
  printf 'Usage: MAICHAT_CANARY_CREDENTIAL_FILE=/absolute/private/accounts.json bash %s --execute-canary\n' "$0" >&2
  exit 2
fi

credential_file=${MAICHAT_CANARY_CREDENTIAL_FILE:?Set the private canary ledger path}
adb_tool=${ANDROID_ADB:-${HOME}/Library/Android/sdk/platform-tools/adb}
app_id=com.kongshang.maichat.isolated
device=emulator-5554
device_file=files/cloud-drive-canary-credentials.json

python3 - "$credential_file" <<'PY'
import json
import os
import re
import stat
import sys

name = sys.argv[1]
if not os.path.isabs(name):
    raise SystemExit('Canary ledger path must be absolute')
info = os.lstat(name)
if (not stat.S_ISREG(info.st_mode) or info.st_uid != os.getuid()
        or stat.S_IMODE(info.st_mode) != 0o600 or info.st_size >= 4096):
    raise SystemExit('Canary ledger must be an owned 0600 regular file under 4 KiB')
with open(name, encoding='utf-8') as source:
    ledger = json.load(source)
for letter in 'AB':
    item = ledger['accounts'][letter]
    if (not re.fullmatch(r'cloudcanary_' + letter.lower() + r'_[0-9a-f]{12}', item['id'])
            or item['state'] != 'verified' or not 32 <= len(item['password']) <= 128):
        raise SystemExit('Canary ledger accounts are not verified disposable users')
PY

if [[ "$("$adb_tool" -s "$device" get-serialno)" != "$device" ]]; then
  printf 'Required emulator is unavailable; no other device may be used\n' >&2
  exit 1
fi
package_info=$("$adb_tool" -s "$device" shell dumpsys package "$app_id")
if [[ "$package_info" != *'versionCode=232'* ]]; then
  printf 'Install isolated Android canary build 232 on emulator-5554 first\n' >&2
  exit 1
fi

cleanup() {
  "$adb_tool" -s "$device" shell run-as "$app_id" rm -f "$device_file" >/dev/null 2>&1 || true
}
trap cleanup EXIT
"$adb_tool" -s "$device" shell \
  "run-as $app_id sh -c 'umask 077; cat > $device_file'" < "$credential_file"
if ! "$adb_tool" -s "$device" shell run-as "$app_id" ls -l "$device_file" |
    grep -q '^\-rw------- '; then
  printf 'Device canary ledger was not written with mode 0600\n' >&2
  exit 1
fi

if ! output=$("$adb_tool" -s "$device" shell am instrument -w -r \
  -e runCloudCanary 1 \
  -e class com.kongshang.maichat.MaiCloudDriveProductionCanaryTest \
  "$app_id.test/androidx.test.runner.AndroidJUnitRunner"); then
  printf 'Android instrumentation could not start\n' >&2
  exit 1
fi
if [[ "$output" == *'OK (1 test)'* && "$output" == *'INSTRUMENTATION_CODE: -1'* ]]; then
  printf 'Android production cloud canary: PASS (1 test)\n'
else
  printf 'Android production cloud canary: FAIL; inspect instrumentation status locally\n' >&2
  printf '%s\n' "$output" | grep -E '^(INSTRUMENTATION_STATUS: (test|stack)|INSTRUMENTATION_RESULT:|INSTRUMENTATION_CODE:|FAILURES!!!|Tests run:)' >&2 || true
  exit 1
fi
