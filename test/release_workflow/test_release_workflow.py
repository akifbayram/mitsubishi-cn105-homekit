#!/usr/bin/env python3
"""Exercise the release workflow with the real validator and local Git remotes."""
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest

import yaml

ROOT = Path(__file__).resolve().parents[2]
PRODUCT = "homekit"
DISTRIBUTION = Path(os.environ.get("SERIN_DISTRIBUTION_ROOT", ROOT.parent / "serin-cn105"))
WORKFLOW = yaml.safe_load((ROOT / ".github/workflows/firmware-release.yml").read_text())
STEPS = WORKFLOW["jobs"]["deploy"]["steps"]
GENERATE = next(i for i, step in enumerate(STEPS) if step["name"] == "Generate manifest.json")
DEPLOY = next(i for i, step in enumerate(STEPS) if step["name"] == "Deploy firmware files")


class ReleaseWorkflowTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory(prefix="release-workflow-")
        self.addCleanup(self.tmp.cleanup)
        self.workspace = Path(self.tmp.name)
        self.checkout = self.workspace / "serin-cn105"
        self.remote = self.workspace / "remote.git"
        self.real_git = shutil.which("git")
        self.env = dict(os.environ, GIT_CONFIG_NOSYSTEM="1", GIT_CONFIG_GLOBAL="/dev/null",
                        GIT_TERMINAL_PROMPT="0", VERSION="0.9.0", TAG="v0.9.0",
                        CHANNEL="stable", CHANNEL_DIR=f"firmware/{PRODUCT}")
        # Run the same interpreter for the workflow's python3 and this test.
        self.bin = self.workspace / "bin"
        self.bin.mkdir()
        self.env["PATH"] = os.pathsep.join((str(self.bin), str(Path(sys.executable).parent),
                                          self.env["PATH"]))
        self.git(self.workspace, "init", "--bare", "--initial-branch=main", str(self.remote))
        self.git(self.workspace, "clone", str(self.remote), str(self.checkout))
        self.identity(self.checkout)
        if not (DISTRIBUTION / "scripts/validate-manifests.py").is_file():
            self.fail("Set SERIN_DISTRIBUTION_ROOT to a checkout with scripts/validate-manifests.py")
        shutil.copytree(DISTRIBUTION / "scripts", self.checkout / "scripts",
                        ignore=shutil.ignore_patterns("__pycache__"))
        self.other_product(self.checkout, "0.1.0")
        self.commit(self.checkout)
        self.git(self.checkout, "push", "origin", "HEAD:main")
        self.initial = self.remote_head()
        for board in ("nanoc6", "m5atoms3-lite"):
            directory = self.workspace / "artifacts" / f"firmware-{board}"
            directory.mkdir(parents=True)
            for part in ("bootloader.bin", "partitions.bin", "boot_app0.bin", "firmware.bin"):
                (directory / part).write_bytes(f"{board}/{part}".encode())

    def git(self, directory, *args):
        result = subprocess.run([self.real_git, "-C", str(directory), *args], env=self.env,
                                text=True, capture_output=True, timeout=30)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        return result.stdout.strip()

    def identity(self, directory):
        self.git(directory, "config", "user.name", "Release Test")
        self.git(directory, "config", "user.email", "release-test@example.invalid")

    def commit(self, directory):
        self.git(directory, "add", ".")
        self.git(directory, "commit", "-m", "Test candidate")

    def remote_head(self):
        return self.git(self.remote, "rev-parse", "main")

    def other_product(self, directory, version, valid=True):
        directory = directory / "firmware/esphome"
        directory.mkdir(parents=True, exist_ok=True)
        data = b"ESPHome fixture"
        (directory / "firmware.bin").write_bytes(data)
        (directory / "manifest.json").write_text(json.dumps({
            "name": "ESPHome", "version": version, "builds": [{
                "board": "nanoc6", "chipFamily": "ESP32-C6",
                "sha256": hashlib.sha256(data).hexdigest() if valid else "0" * 64,
                "parts": [{"path": "firmware.bin", "offset": 0}]}]}))

    def run_steps(self, steps):
        output = ""
        for step in steps:
            if "run" not in step:
                continue
            result = subprocess.run(["bash", "--noprofile", "--norc", "-eo", "pipefail",
                                     "-c", step["run"]], cwd=self.workspace,
                                    env=self.env, text=True, capture_output=True, timeout=30)
            output += result.stdout + result.stderr
            if result.returncode:
                return result.returncode, output
        return 0, output

    def prepare(self, beta=False):
        if beta:
            self.env.update(VERSION="0.9.0-beta.1", TAG="v0.9.0-beta.1", CHANNEL="beta",
                            CHANNEL_DIR=f"firmware/{PRODUCT}/beta")
        code, output = self.run_steps(STEPS[DEPLOY:GENERATE + 1])
        self.assertEqual(code, 0, output)

    def publish(self):
        return self.run_steps(STEPS[GENERATE + 1:])

    def intercept_push(self, competitor=None, reject=False):
        # Inject a competing publisher at the network boundary. Clone, commit,
        # non-fast-forward rejection, rebase and validation all use real Git/files.
        self.attempts = self.workspace / "push-attempts"
        wrapper = f"""#!{sys.executable}
import os
from pathlib import Path
import subprocess
import sys
real = {self.real_git!r}
counter = Path({str(self.attempts)!r})
if len(sys.argv) > 1 and sys.argv[1] == "push":
    count = int(counter.read_text()) if counter.exists() else 0
    counter.write_text(str(count + 1))
    if count == 0 and {str(competitor) if competitor else None!r}:
        subprocess.run([real, "-C", {str(competitor)!r}, "push", "origin", "HEAD:main"], check=True)
    if {reject!r}:
        sys.exit(1)
os.execv(real, [real, *sys.argv[1:]])
"""
        path = self.bin / "git"
        path.write_text(wrapper)
        path.chmod(0o755)

    def competitor(self, valid):
        directory = self.workspace / "competitor"
        self.git(self.workspace, "clone", str(self.remote), str(directory))
        self.identity(directory)
        self.other_product(directory, "0.2.0", valid)
        self.commit(directory)
        self.intercept_push(competitor=directory)
        return self.git(directory, "rev-parse", "HEAD")

    def test_valid_release_reaches_distribution(self):
        self.prepare()
        code, output = self.publish()
        self.assertEqual(code, 0, output)
        manifest = json.loads(self.git(self.remote, "show", f"main:firmware/{PRODUCT}/manifest.json"))
        self.assertEqual(manifest["version"], "0.9.0")
        self.assertEqual({build["board"] for build in manifest["builds"]}, {"nanoc6", "m5atoms3-lite"})

    def test_generated_manifest_hashes_every_part(self):
        self.prepare()
        path = self.checkout / f"firmware/{PRODUCT}/manifest.json"
        manifest = json.loads(path.read_text())
        for build in manifest["builds"]:
            for part in build["parts"]:
                data = (path.parent / part["path"]).read_bytes()
                self.assertEqual(part.get("sha256"), hashlib.sha256(data).hexdigest(), part["path"])

    def test_corrupt_bootloader_cannot_publish(self):
        self.prepare()
        (self.checkout / f"firmware/{PRODUCT}/nanoc6/bootloader.bin").write_bytes(b"corruption")
        code, output = self.publish()
        self.assertNotEqual(code, 0, output)
        self.assertIn("sha256 does not match", output)
        self.assertEqual(self.remote_head(), self.initial)

    def test_corrupt_candidate_cannot_publish(self):
        self.prepare()
        (self.checkout / f"firmware/{PRODUCT}/nanoc6/firmware.bin").write_bytes(b"corruption")
        code, output = self.publish()
        self.assertNotEqual(code, 0, output)
        self.assertIn("sha256 does not match", output)
        self.assertEqual(self.remote_head(), self.initial)

    def test_missing_validator_cannot_publish(self):
        self.prepare()
        (self.checkout / "scripts/validate-manifests.py").unlink()
        code, output = self.publish()
        self.assertNotEqual(code, 0, output)
        self.assertEqual(self.remote_head(), self.initial)

    def test_unchanged_release_succeeds(self):
        self.prepare()
        code, output = self.publish()
        self.assertEqual(code, 0, output)
        published = self.remote_head()
        code, output = self.publish()
        self.assertEqual(code, 0, output)
        self.assertEqual(self.remote_head(), published)

    def test_valid_concurrent_publication_is_preserved(self):
        self.prepare()
        self.competitor(valid=True)
        code, output = self.publish()
        self.assertEqual(code, 0, output)
        self.assertEqual(self.attempts.read_text(), "2")
        manifest = json.loads(self.git(self.remote, "show", "main:firmware/esphome/manifest.json"))
        self.assertEqual(manifest["version"], "0.2.0")
        self.assertEqual(self.git(self.remote, "show", f"main:firmware/{PRODUCT}/nanoc6/firmware.bin"),
                         "nanoc6/firmware.bin")

    def test_invalid_concurrent_publication_stops_retry(self):
        self.prepare()
        competing_commit = self.competitor(valid=False)
        code, output = self.publish()
        self.assertNotEqual(code, 0, output)
        self.assertIn("sha256 does not match", output)
        self.assertEqual(self.attempts.read_text(), "1")
        self.assertEqual(self.remote_head(), competing_commit)

    def test_rejected_pushes_stop_after_three_attempts(self):
        self.prepare()
        self.intercept_push(reject=True)
        code, output = self.publish()
        self.assertNotEqual(code, 0, output)
        self.assertEqual(self.attempts.read_text(), "3")
        self.assertEqual(self.remote_head(), self.initial)

    def test_conflicting_publication_stops_without_overwriting_remote(self):
        self.prepare()
        directory = self.workspace / "competitor"
        self.git(self.workspace, "clone", str(self.remote), str(directory))
        self.identity(directory)
        shutil.copytree(self.checkout / f"firmware/{PRODUCT}",
                        directory / f"firmware/{PRODUCT}")
        path = directory / f"firmware/{PRODUCT}/manifest.json"
        manifest = json.loads(path.read_text())
        manifest["version"] = "0.8.0"
        path.write_text(json.dumps(manifest))
        self.commit(directory)
        competing_commit = self.git(directory, "rev-parse", "HEAD")
        self.intercept_push(competitor=directory)
        code, output = self.publish()
        self.assertNotEqual(code, 0, output)
        self.assertIn("CONFLICT", output)
        self.assertEqual(self.attempts.read_text(), "1")
        self.assertEqual(self.remote_head(), competing_commit)

    def test_release_assets_wait_for_deployment_and_only_version_tags_publish(self):
        jobs = WORKFLOW["jobs"]
        dependencies = jobs["release"]["needs"]
        if isinstance(dependencies, str):
            dependencies = [dependencies]
        self.assertIn("deploy", dependencies)
        for job in ("deploy", "release"):
            self.assertEqual(jobs[job]["if"],
                             "github.ref_type == 'tag' && startsWith(github.ref_name, 'v')")
        checkout = next(step["with"] for step in STEPS if step["name"] == "Checkout serin-cn105")
        self.assertEqual(checkout["ref"], "main")
        self.assertEqual(checkout["fetch-depth"], 0)

    def test_beta_publication_is_safe(self):
        self.prepare(beta=True)
        code, output = self.publish()
        if PRODUCT == "matter":
            # Matter still writes prereleases to stable without channel: beta.
            # Until its routing is fixed, the validator must stop publication.
            self.assertNotEqual(code, 0, output)
            self.assertIn("channel does not match release version", output)
            self.assertEqual(self.remote_head(), self.initial)
        else:
            self.assertEqual(code, 0, output)
            manifest = json.loads(self.git(self.remote, "show",
                                           "main:firmware/homekit/beta/manifest.json"))
            self.assertEqual(manifest["channel"], "beta")


if __name__ == "__main__":
    unittest.main(verbosity=2)
