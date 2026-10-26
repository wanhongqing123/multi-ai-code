#!/usr/bin/env python3
"""Opt-in iPhone Air canary against live auth/private OSS using disposable A/B accounts.

The credential ledger stays outside the repository. This launcher stages only one
account at a time as a 0600 file in the dedicated simulator's MaiChat Documents
directory. The debug App consumes and unlinks that file before logging in. No
password or bearer is passed in command-line arguments or printed by this tool.
"""

import argparse
import json
import os
from pathlib import Path
import plistlib
import subprocess
import sys


ROOT = Path(__file__).resolve().parents[1]
REPOSITORY = Path(__file__).resolve().parents[4]
BUNDLE_ID = "com.kongshang.maichat"
TESTS = (
    ("A", "testOwnerNormalAppUploadPlayDelete"),
    ("A", "testOwnerAgentTools"),
    ("B", "testPeerEntryHidden"),
    ("B", "testPeerApiReturns404"),
)


def run(command, *, env=None, output=False):
    result = subprocess.run(command, check=True, env=env, text=True,
                            stdout=subprocess.PIPE if output else None)
    return result.stdout.strip() if output else None


def run_ui_test(command, case_name):
    completed = subprocess.run(command, text=True, stdout=subprocess.PIPE,
                               stderr=subprocess.STDOUT)
    lines = completed.stdout.splitlines()
    summary = [line for line in lines if "Test Case" in line and
               ("passed" in line or "failed" in line or "skipped" in line)]
    for line in summary:
        print(line, flush=True)
    if completed.returncode != 0 or len(summary) != 1 or "passed" not in summary[0]:
        errors = [line for line in lines if "error: -[" in line]
        for line in errors[-5:]:
            print(line, flush=True)
        raise ValueError(f"Canary UI test did not pass: {case_name}")


def assert_air_simulator(udid):
    data = json.loads(run(["xcrun", "simctl", "list", "devices", "available", "-j"],
                          output=True))
    matches = [device for group in data["devices"].values() for device in group
               if device["udid"] == udid]
    if len(matches) != 1 or matches[0]["name"] != "MaiChat Cloud Canary Air":
        raise ValueError("Use a dedicated available iPhone Air simulator UDID")
    return matches[0]["state"]


def read_ledger(path):
    file = Path(path).resolve(strict=True)
    if file.stat().st_mode & 0o777 != 0o600:
        raise ValueError("Credential ledger must have mode 0600")
    if REPOSITORY in file.parents:
        raise ValueError("Credential ledger must be outside the repository")
    accounts = json.loads(file.read_text(encoding="utf-8"))["accounts"]
    for role in ("A", "B"):
        account = accounts[role]
        if not account["id"] or not account["password"]:
            raise ValueError("Both disposable accounts need ID and password")
    if accounts["A"]["id"] == accounts["B"]["id"]:
        raise ValueError("A and B must be distinct accounts")
    return accounts


def stage_credential(container, account):
    file = Path(container) / "Documents/cloud-drive-production-canary.json"
    file.parent.mkdir(parents=True, exist_ok=True)
    payload = json.dumps({"account": account["id"],
                          "password": account["password"]}).encode("utf-8")
    fd = os.open(file, os.O_WRONLY | os.O_CREAT | os.O_TRUNC, 0o600)
    try:
        os.fchmod(fd, 0o600)
        os.write(fd, payload)
        os.fsync(fd)
    finally:
        os.close(fd)
    return file


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    mode = parser.add_mutually_exclusive_group(required=True)
    mode.add_argument("--prepare", action="store_true",
                      help="Build only; makes no production request")
    mode.add_argument("--execute", action="store_true",
                      help="Run live A/B upload and Agent tests after explicit go-ahead")
    parser.add_argument("--simulator", required=True, help="dedicated iPhone Air UDID")
    parser.add_argument("--credentials", help="repository-external 0600 A/B JSON ledger")
    parser.add_argument("--case", action="append", choices=[name for _, name in TESTS],
                        help="run one named case; repeat for multiple cases")
    parser.add_argument("--derived-data", type=Path,
                        default=Path("/tmp/maichat-ios-cloud-agent-cache-canary"))
    args = parser.parse_args()
    simulator_state = assert_air_simulator(args.simulator)
    base = ["xcodebuild", "-workspace", str(ROOT / "MaiChat.xcworkspace"),
            "-scheme", "MaiChat", "-configuration", "Debug", "-destination",
            f"platform=iOS Simulator,id={args.simulator}", "-derivedDataPath",
            str(args.derived_data), "-parallel-testing-enabled", "NO"]
    if args.prepare:
        run(base + ["CODE_SIGNING_ALLOWED=NO", "build-for-testing"])
        print("iOS cloud production canary built; no live request was made")
        return
    if os.environ.get("MAICHAT_RUN_CLOUD_PROD_CANARY") != "YES":
        raise ValueError("Set MAICHAT_RUN_CLOUD_PROD_CANARY=YES to execute")
    if not args.credentials:
        raise ValueError("--credentials is required for live execution")
    accounts = read_ledger(args.credentials)
    xctestruns = sorted((args.derived_data / "Build/Products").glob("*.xctestrun"))
    app = args.derived_data / "Build/Products/Debug-iphonesimulator/MaiChat.app"
    if len(xctestruns) != 1 or not app.is_dir():
        raise ValueError("Run --prepare first with the same derived-data directory")
    if simulator_state == "Shutdown":
        run(["xcrun", "simctl", "boot", args.simulator])
    run(["xcrun", "simctl", "bootstatus", args.simulator, "-b"])
    # `test-without-building` deliberately does not inherit shell environment
    # variables into XCTest. Put only non-secret opt-in and expected IDs in a
    # private copy of the test plan; passwords remain in the App's one-time file.
    runfile = xctestruns[0].with_name("MaiChatCloudCanary.xctestrun")
    with xctestruns[0].open("rb") as source:
        plan = plistlib.load(source)
    test_environment = plan["MaiChatUITests"]["TestingEnvironmentVariables"]
    test_environment["MAICHAT_RUN_CLOUD_PROD_CANARY"] = "YES"
    test_environment["MAICHAT_CLOUD_CANARY_A_USER_ID"] = accounts["A"]["id"]
    test_environment["MAICHAT_CLOUD_CANARY_B_USER_ID"] = accounts["B"]["id"]
    fd = os.open(runfile, os.O_WRONLY | os.O_CREAT | os.O_TRUNC, 0o600)
    with os.fdopen(fd, "wb") as output:
        os.fchmod(output.fileno(), 0o600)
        plistlib.dump(plan, output)
    try:
        selected = [(role, name) for role, name in TESTS
                    if args.case is None or name in args.case]
        for role, name in selected:
            run(["xcrun", "simctl", "install", args.simulator, str(app)])
            container = run(["xcrun", "simctl", "get_app_container", args.simulator,
                             BUNDLE_ID, "data"], output=True)
            staged = stage_credential(container, accounts[role])
            try:
                print(f"Running iOS cloud canary {role}: {name}", flush=True)
                run_ui_test(["xcodebuild", "-xctestrun", str(runfile),
                             "-destination", f"platform=iOS Simulator,id={args.simulator}",
                             "-parallel-testing-enabled", "NO",
                             "-only-testing:MaiChatUITests/"
                             f"CloudDriveProductionCanaryUITests/{name}",
                             "test-without-building"], name)
                if staged.exists():
                    raise ValueError("Canary credential was not consumed by the App")
            finally:
                print(f"Canary {role} credential consumed: {not staged.exists()}",
                      flush=True)
                staged.unlink(missing_ok=True)
    finally:
        runfile.unlink(missing_ok=True)
        # The disposable simulator should not keep an authenticated App or media.
        run(["xcrun", "simctl", "uninstall", args.simulator, BUNDLE_ID])
    print("iOS cloud production canary passed; verify account A has no leftovers")


if __name__ == "__main__":
    try:
        main()
    except (OSError, ValueError, subprocess.CalledProcessError, KeyError) as error:
        print(f"iOS cloud canary stopped: {type(error).__name__}", file=sys.stderr)
        sys.exit(1)
