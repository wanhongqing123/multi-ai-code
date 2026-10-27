#!/usr/bin/env python3
"""One-time provisioning of the private cloud-drive OSS RAM identity.

Run only on the MaiChat ECS after loading /etc/maichat/media.env into the
environment. The new AccessKey response is captured in memory and written to
a root-only file; it is never printed or passed as a command-line argument.
"""

import argparse
import hashlib
import json
import os
import re
import subprocess
import sys
import tempfile
import urllib.parse
import uuid
from pathlib import Path


BUCKET = "maichat-private-cloud-drive-gz-20261025-f346b709"
USER = "maichat-cloud-drive-signer"
POLICY = "MaiChatCloudDriveSigner"
DESTINATION = Path("/etc/maichat/cloud-drive.env")
CLI = "/usr/local/bin/aliyun"
ADMIN_IDENTITY_SHA256 = (
    "28058191a543d61c01a97456289e3651125b2b3f6438507d5fcdcf8c600a3f05"
)

POLICY_DOCUMENT = {
    "Version": "1",
    "Statement": [
        {
            "Effect": "Deny",
            "Action": "oss:*",
            "Resource": [
                "acs:oss:*:*:maichat-ai-video-resource",
                "acs:oss:*:*:maichat-ai-video-resource/*",
            ],
        },
        {
            "Effect": "Allow",
            "Action": [
                "oss:GetBucketInfo",
                "oss:GetBucketLocation",
                "oss:GetBucketAcl",
                "oss:GetBucketVersioning",
                "oss:GetBucketLifecycle",
                "oss:ListMultipartUploads",
            ],
            "Resource": f"acs:oss:*:*:{BUCKET}",
        },
        {
            "Effect": "Allow",
            "Action": "oss:ListObjects",
            "Resource": f"acs:oss:*:*:{BUCKET}",
            "Condition": {
                "StringLike": {
                    "oss:Prefix": ["cloud-drive/", "cloud-drive/*"]
                }
            },
        },
        {
            "Effect": "Allow",
            "Action": [
                "oss:PutObject",
                "oss:GetObject",
                "oss:DeleteObject",
                "oss:ListParts",
                "oss:AbortMultipartUpload",
                "oss:GetObjectAcl",
            ],
            "Resource": f"acs:oss:*:*:{BUCKET}/cloud-drive/*",
        },
    ],
}

LEGACY_POLICY_DOCUMENT = json.loads(json.dumps(POLICY_DOCUMENT))
POLICY_DOCUMENT["Statement"][2]["Condition"]["StringLike"]["oss:Prefix"] += [
    "session-attachments/", "session-attachments/*"
]
POLICY_DOCUMENT["Statement"][3]["Resource"] = [
    f"acs:oss:*:*:{BUCKET}/cloud-drive/*",
    f"acs:oss:*:*:{BUCKET}/session-attachments/*",
]


class ProvisionError(Exception):
    pass


class ResourceAbsent(ProvisionError):
    pass


def call_cli(service, action, *args):
    required = ("ALIBABA_CLOUD_ACCESS_KEY_ID", "ALIBABA_CLOUD_ACCESS_KEY_SECRET")
    if any(not os.environ.get(name) for name in required):
        raise ProvisionError("Missing existing administrator OSS credentials")
    env = {
        "PATH": "/usr/local/bin:/usr/bin:/bin",
        "HOME": "/root",
        "ALIBABA_CLOUD_ACCESS_KEY_ID": os.environ[required[0]],
        "ALIBABA_CLOUD_ACCESS_KEY_SECRET": os.environ[required[1]],
        "ALIBABA_CLOUD_IGNORE_PROFILE": "TRUE",
    }
    process = subprocess.run(
        [CLI, service, action, "--region", "cn-hangzhou", *args],
        capture_output=True,
        text=True,
        env=env,
        timeout=30,
        check=False,
    )
    if process.returncode:
        # CLI errors may contain identifiers. Never log full stdout/stderr.
        if service == "ram" and action in ("GetUser", "GetPolicy") and re.search(
            r"EntityNotExist(?:\.[A-Za-z]+)?", process.stderr or process.stdout
        ):
            raise ResourceAbsent(f"{action} resource is absent")
        raise ProvisionError(f"{service} {action} failed (exit {process.returncode})")
    try:
        return json.loads(process.stdout)
    except ValueError as error:
        raise ProvisionError(f"{service} {action} returned invalid JSON") from error


def _require_key(value):
    if not isinstance(value, str) or not re.fullmatch(r"[A-Za-z0-9_+/=.-]+", value):
        raise ProvisionError("AccessKey response contains an unsupported value")
    return value


def _write_env_atomic(path, key_id, secret):
    if path.exists():
        raise ProvisionError("Cloud-drive environment file already exists")
    contents = (
        "MAICHAT_CLOUD_DRIVE_ENABLED=0\n"
        f"MAICHAT_CLOUD_DRIVE_OSS_BUCKET={BUCKET}\n"
        "MAICHAT_CLOUD_DRIVE_OSS_REGION=oss-cn-guangzhou\n"
        f"MAICHAT_CLOUD_DRIVE_OSS_ACCESS_KEY_ID={_require_key(key_id)}\n"
        f"MAICHAT_CLOUD_DRIVE_OSS_ACCESS_KEY_SECRET={_require_key(secret)}\n"
    )
    fd, temporary = tempfile.mkstemp(prefix=".cloud-drive.env-", dir=path.parent)
    linked = False
    try:
        os.fchmod(fd, 0o600)
        with os.fdopen(fd, "w", encoding="utf-8") as stream:
            stream.write(contents)
            stream.flush()
            os.fsync(stream.fileno())
        os.link(temporary, path)  # Atomic create, refusing to overwrite.
        linked = True
        os.unlink(temporary)
        directory_fd = os.open(path.parent, os.O_RDONLY | os.O_DIRECTORY)
        try:
            os.fsync(directory_fd)
        finally:
            os.close(directory_fd)
    except Exception:
        if linked:
            try:
                os.unlink(path)
            except FileNotFoundError:
                pass
        try:
            os.unlink(temporary)
        except FileNotFoundError:
            pass
        raise


def _new_key_ids(call):
    result = call("ram", "ListAccessKeys", "--UserName", USER)
    return [
        key.get("AccessKeyId")
        for key in result.get("AccessKeys", {}).get("AccessKey", [])
        if key.get("AccessKeyId")
    ]


def _lookup(call, action, *args):
    try:
        return call("ram", action, *args)
    except ResourceAbsent:
        return None


def _user(call):
    response = _lookup(call, "GetUser", "--UserName", USER)
    return response.get("User", {}) if response is not None else None


def _policy(call):
    response = _lookup(call, "GetPolicy", "--PolicyName", POLICY,
                       "--PolicyType", "Custom")
    return response.get("Policy", {}) if response is not None else None


def _policy_document(call, policy):
    version_id = policy.get("DefaultVersion")
    if not version_id:
        raise ProvisionError("New policy has no default version")
    version = call("ram", "GetPolicyVersion", "--PolicyName", POLICY,
                   "--PolicyType", "Custom", "--VersionId", version_id)
    document = version.get("PolicyVersion", {}).get("PolicyDocument")
    if isinstance(document, str):
        document = json.loads(urllib.parse.unquote(document))
    return document


def _attached_policies(call):
    response = call("ram", "ListPoliciesForUser", "--UserName", USER)
    return {
        (item.get("PolicyName"), item.get("PolicyType"))
        for item in response.get("Policies", {}).get("Policy", [])
    }


def _confirm_removed(call, action, is_present, *args):
    # The mutation response can be lost after the API has already committed.
    try:
        call("ram", action, *args)
    except Exception:
        pass
    if is_present():
        raise ProvisionError(f"{action} could not be confirmed")


def _rollback(call, marker):
    user = _user(call)
    policy = _policy(call)
    if user is not None and (
        user.get("UserName") != USER or user.get("Comments") != marker
    ):
        raise ProvisionError("RAM user identity does not match this run")
    if policy is not None and (
        policy.get("PolicyName") != POLICY
        or policy.get("Description") != marker
        or _policy_document(call, policy) != POLICY_DOCUMENT
    ):
        raise ProvisionError("RAM policy identity does not match this run")

    if user is not None:
        groups = call("ram", "ListGroupsForUser", "--UserName", USER)
        if groups.get("Groups", {}).get("Group", []):
            raise ProvisionError("New RAM user has unexpected groups")
        attached = _attached_policies(call)
        if attached - {(POLICY, "Custom")}:
            raise ProvisionError("New RAM user has unexpected policies")
        # The user did not exist at preflight. Even if CreateAccessKey timed
        # out, ListAccessKeys exposes IDs so every new key can be removed.
        for key_id in _new_key_ids(call):
            _confirm_removed(
                call, "DeleteAccessKey",
                lambda key_id=key_id: key_id in _new_key_ids(call),
                "--UserName", USER, "--UserAccessKeyId", key_id
            )
        if (POLICY, "Custom") in attached:
            _confirm_removed(
                call, "DetachPolicyFromUser",
                lambda: (POLICY, "Custom") in _attached_policies(call),
                "--UserName", USER, "--PolicyType", "Custom",
                "--PolicyName", POLICY
            )
    if policy is not None:
        _confirm_removed(
            call, "DeletePolicy", lambda: _policy(call) is not None,
            "--PolicyName", POLICY
        )
    if user is not None:
        _confirm_removed(
            call, "DeleteUser", lambda: _user(call) is not None,
            "--UserName", USER
        )


def provision(call=call_cli, destination=DESTINATION):
    identity = call("sts", "GetCallerIdentity")
    identity_fields = "|".join(
        str(identity.get(name, ""))
        for name in ("AccountId", "Arn", "IdentityType")
    )
    if hashlib.sha256(identity_fields.encode()).hexdigest() != ADMIN_IDENTITY_SHA256:
        raise ProvisionError("Unexpected administrator identity")
    if destination.exists() or _user(call) is not None or _policy(call) is not None:
        raise ProvisionError("Cloud-drive RAM name or environment file already exists")

    marker = "maichat-cloud-drive-provision:" + uuid.uuid4().hex
    try:
        call("ram", "CreateUser", "--UserName", USER,
             "--DisplayName", "MaiChat cloud drive signer",
             "--Comments", marker)
        call("ram", "CreatePolicy", "--PolicyName", POLICY,
             "--Description", marker,
             "--PolicyDocument",
             json.dumps(POLICY_DOCUMENT, separators=(",", ":")))
        current_policy = _policy(call)
        if current_policy is None or current_policy.get("Description") != marker or (
            _policy_document(call, current_policy) != POLICY_DOCUMENT
        ):
            raise ProvisionError("New RAM policy does not match expected document")
        call("ram", "AttachPolicyToUser", "--UserName", USER,
             "--PolicyType", "Custom", "--PolicyName", POLICY)

        if _attached_policies(call) != {(POLICY, "Custom")}:
            raise ProvisionError("New RAM user has unexpected policies")
        groups = call("ram", "ListGroupsForUser", "--UserName", USER)
        if groups.get("Groups", {}).get("Group", []):
            raise ProvisionError("New RAM user has unexpected groups")
        if _new_key_ids(call):
            raise ProvisionError("New RAM user already has an AccessKey")

        response = call("ram", "CreateAccessKey", "--UserName", USER)
        access_key = response.get("AccessKey", {})
        _write_env_atomic(
            destination, access_key.get("AccessKeyId"),
            access_key.get("AccessKeySecret"))
    except Exception as original:
        try:
            _rollback(call, marker)
        except Exception as cleanup:
            raise ProvisionError(
                "Provisioning failed and rollback is incomplete; inspect RAM "
                "user, policy, and keys before retrying"
            ) from cleanup
        raise ProvisionError(
            "Provisioning failed; cleanup was checked once. Recheck RAM state "
            "before retrying because the failed API outcome may be delayed"
        ) from original
    return {"user": USER, "policy": POLICY, "destination": str(destination)}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--execute", metavar="BUCKET")
    args = parser.parse_args()
    if args.execute != BUCKET or os.geteuid() != 0:
        parser.error(f"Run as root with --execute={BUCKET}")
    result = provision()
    print(f"RAM user {result['user']} provisioned; credential file written "
          f"to {result['destination']} (0600). No service was restarted.")


if __name__ == "__main__":
    try:
        main()
    except ProvisionError as error:
        print(str(error), file=sys.stderr)
        sys.exit(1)
