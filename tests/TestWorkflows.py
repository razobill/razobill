#!/usr/bin/env python3
"""Configuration regression tests for the GitHub Actions dependency upgrades.

Requires PyYAML. Run from the repository root with:
    python -m unittest discover -s tests -p TestWorkflows.py -v

These tests inspect workflow contracts without running builds or publishing.
"""

from fnmatch import fnmatchcase
from itertools import product
from pathlib import Path
import unittest

import yaml


ROOT = Path(__file__).resolve().parent.parent
ACTION_VERSIONS = {
    "actions/checkout": "v7",
    "actions/setup-python": "v7",
    "actions/upload-artifact": "v7",
    "actions/download-artifact": "v8",
    "docker/setup-qemu-action": "v4",
    "softprops/action-gh-release": "v3",
    "codecov/codecov-action": "v7",
    "dependabot/fetch-metadata": "v3",
    "actions/configure-pages": "v6",
    "actions/upload-pages-artifact": "v5",
    "actions/deploy-pages": "v5",
}
BUILD_ACTIONS = {"actions/checkout", "actions/setup-python"}
WORKFLOW_ACTIONS = {
    "CI.yml": BUILD_ACTIONS,
    "DuckdbExtension.yml": BUILD_ACTIONS,
    "Release.yml": BUILD_ACTIONS
    | {
        "actions/upload-artifact",
        "actions/download-artifact",
        "docker/setup-qemu-action",
        "softprops/action-gh-release",
    },
    "Sanitizers.yml": BUILD_ACTIONS,
    "codeql.yml": BUILD_ACTIONS,
    "coverage.yml": BUILD_ACTIONS | {"codecov/codecov-action"},
    "dependabot-automation.yml": {"dependabot/fetch-metadata"},
    "docs-lint.yml": BUILD_ACTIONS,
    "pages.yml": {
        "actions/checkout",
        "actions/configure-pages",
        "actions/upload-pages-artifact",
        "actions/deploy-pages",
    },
}


class TestWorkflowActions(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        # BaseLoader preserves "on", version numbers, and action inputs as
        # strings, avoiding PyYAML's YAML 1.1 boolean and float conversions.
        cls.workflows = {
            name: yaml.load((ROOT / ".github/workflows" / name).read_text(encoding="utf-8"), Loader=yaml.BaseLoader)
            for name in WORKFLOW_ACTIONS
        }

    def action_step(self, workflow, job_id, action):
        steps = self.workflows[workflow]["jobs"][job_id]["steps"]
        matches = [step for step in steps if step.get("uses", "").split("@")[0] == action]
        self.assertEqual(len(matches), 1, f"Expected one {action} step in {workflow}:{job_id}")
        return matches[0]

    def release_uploads(self):
        for job_id, job in self.workflows["Release.yml"]["jobs"].items():
            for step in job["steps"]:
                if step.get("uses", "").startswith("actions/upload-artifact@"):
                    yield job_id, job, step["with"]

    def expanded_release_artifacts(self):
        artifacts = []
        for job_id, job, inputs in self.release_uploads():
            matrix = job.get("strategy", {}).get("matrix", {})
            for values in product(*matrix.values()):
                name = inputs["name"]
                for dimension, value in zip(matrix, values):
                    name = name.replace("${{ matrix." + dimension + " }}", value)
                self.assertNotIn("${{", name, f"Unresolved artifact name in {job_id}")
                artifacts.append((name, job_id))
        return artifacts

    def test_all_changed_workflows_use_the_upgraded_actions(self):
        for name, expected_actions in WORKFLOW_ACTIONS.items():
            seen = set()
            for job_id, job in self.workflows[name]["jobs"].items():
                for step in job["steps"]:
                    action, _, version = step.get("uses", "").partition("@")
                    if action in ACTION_VERSIONS:
                        with self.subTest(workflow=name, job=job_id, action=action):
                            self.assertEqual(version, ACTION_VERSIONS[action])
                        seen.add(action)
            with self.subTest(workflow=name):
                self.assertEqual(seen, expected_actions)

    def test_checkout_precedes_repository_operations(self):
        for name, workflow in self.workflows.items():
            if "actions/checkout" not in WORKFLOW_ACTIONS[name]:
                continue
            for job_id, job in workflow["jobs"].items():
                with self.subTest(workflow=name, job=job_id):
                    checkout = self.action_step(name, job_id, "actions/checkout")
                    self.assertIs(job["steps"][0], checkout)
                    self.assertNotEqual(
                        checkout.get("with", {}).get("allow-unsafe-pr-checkout", "false"), "true"
                    )

    def test_history_dependent_jobs_keep_full_checkout_history(self):
        for name, job_id in (("Release.yml", "validate-and-lint"), ("coverage.yml", "coverage")):
            with self.subTest(workflow=name):
                checkout = self.action_step(name, job_id, "actions/checkout")
                self.assertEqual(checkout["with"]["fetch-depth"], "0")

    def test_python_setup_retains_explicit_or_matrix_versions(self):
        for name, workflow in self.workflows.items():
            for job_id, job in workflow["jobs"].items():
                for step in job["steps"]:
                    if not step.get("uses", "").startswith("actions/setup-python@"):
                        continue
                    with self.subTest(workflow=name, job=job_id):
                        inputs = step["with"]
                        version = inputs["python-version"]
                        versions = (
                            job["strategy"]["matrix"]["python-version"]
                            if version == "${{ matrix.python-version }}"
                            else [version]
                        )
                        self.assertTrue(versions)
                        for selected in versions:
                            self.assertRegex(selected, r"^3\.\d+(?:\.\d+)?$")
                        self.assertEqual(inputs.get("cache", "pip"), "pip")
                        # setup-python v7 removed this input.
                        self.assertNotIn("pip-install", inputs)
                        first_command = next(
                            index for index, candidate in enumerate(job["steps"]) if "run" in candidate
                        )
                        self.assertLess(job["steps"].index(step), first_command)

    def test_release_upload_names_are_unique_across_platform_and_python_matrices(self):
        artifacts = self.expanded_release_artifacts()
        names = [name for name, _ in artifacts]
        self.assertTrue(names)
        self.assertEqual(len(names), len(set(names)), "Matrix jobs must not overwrite the same artifact")

    def test_release_uploads_keep_binary_and_wheel_paths(self):
        expected_paths = {
            "build-matrix": "release-staging/*",
            "build-wheels-windows": "lib/python/dist/*.whl",
            "build-wheels-linux": "./wheelhouse/*.whl",
            "build-wheels-macos": "lib/python/dist/*.whl",
        }
        uploads = list(self.release_uploads())
        self.assertCountEqual([job_id for job_id, _, _ in uploads], expected_paths)
        for job_id, _, inputs in uploads:
            with self.subTest(job=job_id):
                self.assertEqual(inputs["path"], expected_paths[job_id])
                if job_id == "build-matrix":
                    self.assertEqual(inputs["if-no-files-found"], "ignore")
                else:
                    self.assertNotEqual(inputs.get("if-no-files-found"), "ignore")

    def test_downloads_wait_for_all_artifact_producers(self):
        producers = {job_id for job_id, _, _ in self.release_uploads()}
        self.assertTrue(producers)
        for job_id in ("publish-release", "publish-pypi"):
            with self.subTest(job=job_id):
                job = self.workflows["Release.yml"]["jobs"][job_id]
                self.assertTrue(producers.issubset(job["needs"]))
                download = self.action_step("Release.yml", job_id, "actions/download-artifact")
                self.assertEqual(download["with"]["merge-multiple"], "true")

    def test_pypi_download_includes_every_wheel_and_excludes_native_binaries(self):
        download = self.action_step("Release.yml", "publish-pypi", "actions/download-artifact")["with"]
        artifacts = self.expanded_release_artifacts()
        wheels = {name for name, job_id in artifacts if job_id.startswith("build-wheels-")}
        selected = {name for name, _ in artifacts if fnmatchcase(name, download["pattern"])}
        self.assertTrue(wheels)
        self.assertEqual(selected, wheels)
        self.assertNotIn("name", download, "A name filter overrides the wheel pattern")
        self.assertEqual(download["path"], "lib/python/dist")

    def test_github_release_download_path_matches_published_files(self):
        download = self.action_step("Release.yml", "publish-release", "actions/download-artifact")
        publish = self.action_step("Release.yml", "publish-release", "softprops/action-gh-release")
        self.assertNotIn("name", download["with"])
        self.assertNotIn("pattern", download["with"])
        self.assertIn(download["with"]["path"] + "/*", publish["with"]["files"].splitlines())
        job = self.workflows["Release.yml"]["jobs"]["publish-release"]
        self.assertLess(job["steps"].index(download), job["steps"].index(publish))
        self.assertEqual(self.workflows["Release.yml"]["permissions"]["contents"], "write")
        self.assertTrue((ROOT / publish["with"]["body_path"]).is_file())

    def test_qemu_is_ready_before_building_arm64_wheels(self):
        job = self.workflows["Release.yml"]["jobs"]["build-wheels-linux"]
        qemu = self.action_step("Release.yml", "build-wheels-linux", "docker/setup-qemu-action")
        self.assertIn("arm64", qemu["with"]["platforms"].split(","))
        builds = [step for step in job["steps"] if "python -m cibuildwheel " in step.get("run", "")]
        self.assertEqual(len(builds), 1)
        self.assertIn("aarch64", builds[0]["env"]["CIBW_ARCHS_LINUX"].split())
        self.assertLess(job["steps"].index(qemu), job["steps"].index(builds[0]))

    def test_codecov_keeps_both_reports_and_non_blocking_uploads(self):
        upload = self.action_step("coverage.yml", "coverage", "codecov/codecov-action")
        self.assertEqual(set(upload["with"]["files"].split(",")), {"coverage_cpp.info", "coverage_py.xml"})
        self.assertEqual(upload["with"]["fail_ci_if_error"], "false")
        self.assertEqual(upload["env"]["CODECOV_TOKEN"], "${{ secrets.CODECOV_TOKEN }}")

    def test_dependabot_metadata_output_remains_wired_to_the_major_update_guard(self):
        workflow = self.workflows["dependabot-automation.yml"]
        job = workflow["jobs"]["dependabot"]
        metadata = self.action_step("dependabot-automation.yml", "dependabot", "dependabot/fetch-metadata")
        self.assertIn("pull_request_target", workflow["on"])
        self.assertEqual(job["if"], "github.actor == 'dependabot[bot]'")
        self.assertEqual(metadata["with"]["github-token"], "${{ secrets.GITHUB_TOKEN }}")
        consumers = [step for step in job["steps"] if "run" in step]
        self.assertTrue(consumers)
        for step in consumers:
            with self.subTest(step=step.get("name")):
                self.assertEqual(
                    step["if"], f"steps.{metadata['id']}.outputs.update-type != 'version-update:semver-major'"
                )
                self.assertLess(job["steps"].index(metadata), job["steps"].index(step))
        self.assertFalse(
            any(step.get("uses", "").startswith("actions/checkout@") for step in job["steps"])
        )

    def test_pages_actions_preserve_upload_deploy_order_and_environment_output(self):
        workflow = self.workflows["pages.yml"]
        job = workflow["jobs"]["deploy"]
        configure = self.action_step("pages.yml", "deploy", "actions/configure-pages")
        upload = self.action_step("pages.yml", "deploy", "actions/upload-pages-artifact")
        deploy = self.action_step("pages.yml", "deploy", "actions/deploy-pages")
        self.assertLess(job["steps"].index(configure), job["steps"].index(upload))
        self.assertLess(job["steps"].index(upload), job["steps"].index(deploy))
        self.assertEqual(upload["with"]["path"], "./docs/web")
        self.assertTrue((ROOT / upload["with"]["path"] / "index.html").is_file())
        self.assertEqual(
            upload.get("with", {}).get("name", "github-pages"),
            deploy.get("with", {}).get("artifact_name", "github-pages"),
        )
        self.assertEqual(job["environment"]["name"], "github-pages")
        self.assertEqual(job["environment"]["url"], "${{ steps." + deploy["id"] + ".outputs.page_url }}")
        self.assertEqual(workflow["permissions"]["pages"], "write")
        self.assertEqual(workflow["permissions"]["id-token"], "write")


if __name__ == "__main__":
    unittest.main()
