#!/usr/bin/env python3
"""Expand the existing private OSS signer to the Session attachment prefix.

Run on the ECS with the same temporary administrator environment used for
provisioning. --apply is required; no key or policy document is printed.
"""

import argparse
import hashlib
import importlib.util
import json
import time
from pathlib import Path


def provisioning():
    source = Path(__file__).with_name("provision-cloud-drive-ram.py")
    spec = importlib.util.spec_from_file_location("cloud_drive_ram_provision", source)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def upgrade(call=None, apply=False, pause=time.sleep, plan=None):
    if plan is None:
        plan = provisioning()
    if call is None:
        call = plan.call_cli
    identity = call("sts", "GetCallerIdentity")
    fingerprint = "|".join(str(identity.get(field, "")) for field in
                           ("AccountId", "Arn", "IdentityType"))
    if hashlib.sha256(fingerprint.encode()).hexdigest() != plan.ADMIN_IDENTITY_SHA256:
        raise plan.ProvisionError("Unexpected administrator identity")
    user = plan._user(call)
    policy = plan._policy(call)
    if (not user or user.get("UserName") != plan.USER or not policy or
            policy.get("PolicyName") != plan.POLICY):
        raise plan.ProvisionError("Existing signer identity or policy is missing")
    if (plan.POLICY, "Custom") not in plan._attached_policies(call):
        raise plan.ProvisionError("Signer policy is not attached to its RAM user")
    current = plan._policy_document(call, policy)
    if current == plan.POLICY_DOCUMENT:
        return "already-updated"
    if current != plan.LEGACY_POLICY_DOCUMENT:
        raise plan.ProvisionError("Existing signer policy differs from the reviewed version")
    if not apply:
        return "ready-to-upgrade"
    previous = policy["DefaultVersion"]
    document = json.dumps(plan.POLICY_DOCUMENT, separators=(",", ":"))
    try:
        call("ram", "CreatePolicyVersion", "--PolicyName", plan.POLICY,
             "--PolicyDocument", document, "--SetAsDefault", "true")
    except Exception:
        # The API reply can be lost after committing; always read back first.
        if plan._policy_document(call, plan._policy(call)) != plan.POLICY_DOCUMENT:
            raise
    for _ in range(5):
        updated = plan._policy(call)
        if (updated and updated.get("DefaultVersion") != previous and
                plan._policy_document(call, updated) == plan.POLICY_DOCUMENT):
            return "updated"
        pause(1)
    raise plan.ProvisionError("New RAM policy version was not confirmed as default")


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--apply", action="store_true")
    arguments = parser.parse_args()
    print(upgrade(apply=arguments.apply))


if __name__ == "__main__":
    main()
