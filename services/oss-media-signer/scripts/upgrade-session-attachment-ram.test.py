import hashlib
import importlib.util
import unittest
from pathlib import Path
from types import SimpleNamespace


path = Path(__file__).with_name("upgrade-session-attachment-ram.py")
spec = importlib.util.spec_from_file_location("session_ram_upgrade", path)
module = importlib.util.module_from_spec(spec)
spec.loader.exec_module(module)


class UpgradeTests(unittest.TestCase):
    def fixture(self):
        identity = {"AccountId": "test", "Arn": "test-arn",
                    "IdentityType": "test-type"}
        digest = hashlib.sha256(b"test|test-arn|test-type").hexdigest()
        state = {"version": "v1", "document": {"old": True}, "writes": 0}
        plan = SimpleNamespace(
            ADMIN_IDENTITY_SHA256=digest, USER="signer", POLICY="policy",
            LEGACY_POLICY_DOCUMENT={"old": True},
            POLICY_DOCUMENT={"old": True, "session": True},
            ProvisionError=ValueError,
            _user=lambda _call: {"UserName": "signer"},
            _policy=lambda _call: {"PolicyName": "policy",
                                   "DefaultVersion": state["version"]},
            _policy_document=lambda _call, _policy: state["document"],
            _attached_policies=lambda _call: {("policy", "Custom")},
        )

        def call(service, action, *args):
            if (service, action) == ("sts", "GetCallerIdentity"):
                return identity
            if (service, action) == ("ram", "CreatePolicyVersion"):
                self.assertEqual(args[-2:], ("--SetAsDefault", "true"))
                state["writes"] += 1
                state["version"] = "v2"
                state["document"] = plan.POLICY_DOCUMENT
                return {"PolicyVersion": {"VersionId": "v2"}}
            raise AssertionError((service, action))

        return state, plan, call

    def test_dry_run_upgrade_and_idempotence(self):
        state, plan, call = self.fixture()
        self.assertEqual(module.upgrade(call=call, plan=plan), "ready-to-upgrade")
        self.assertEqual(state["writes"], 0)
        self.assertEqual(module.upgrade(call=call, apply=True, pause=lambda _: None,
                                        plan=plan), "updated")
        self.assertEqual(state["writes"], 1)
        self.assertEqual(module.upgrade(call=call, apply=True, plan=plan),
                         "already-updated")
        self.assertEqual(state["writes"], 1)

    def test_unknown_policy_is_never_replaced(self):
        state, plan, call = self.fixture()
        state["document"] = {"unexpected": True}
        with self.assertRaisesRegex(ValueError, "differs"):
            module.upgrade(call=call, apply=True, plan=plan)
        self.assertEqual(state["writes"], 0)


if __name__ == "__main__":
    unittest.main()
