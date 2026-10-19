#!/usr/bin/env python3
# ==============================================================================
# github actions workflow configuration regression tests
#
# verifies action versions, matrix expansions, artifact routing,
# release dependencies, security guards, and deployment contracts
# ==============================================================================

"""workflow configuration regression test suite for CI and release workflows"""

from fnmatch import fnmatchcase
from itertools import product
from pathlib import Path
import unittest

import yaml

# root directory of repository
ROOT = Path(__file__).resolve().parent.parent

# pinned action major versions expected across workflow definitions
ACTION_VERSIONS = {
    "actions/checkout": "v7",
    "actions/setup-python": "v7",
    "actions/setup-node": "v4",
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

# core build actions shared across standard continuous integration workflows
BUILD_ACTIONS = {"actions/checkout", "actions/setup-python"}

# expected action sets for each workflow configuration file
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
        "actions/setup-node",
        "actions/configure-pages",
        "actions/upload-pages-artifact",
        "actions/deploy-pages",
    },
}


class TestWorkflowActions(unittest.TestCase):
    """regression tests asserting workflow configuration contracts

    validates action version pinning, matrix handling, and permissions
    """

    @classmethod
    def setUpClass(cls):
        # BaseLoader preserves "on", version numbers, and action inputs as
        # strings, avoiding PyYAML's YAML 1.1 boolean and float conversions.
        cls.workflows = {
            name: yaml.load(
                (ROOT / ".github/workflows" / name).read_text(encoding="utf-8"),
                Loader=yaml.BaseLoader,
            )
            for name in WORKFLOW_ACTIONS
        }

    def action_step(self, workflow, job_id, action):
        """extract unique step matching target action prefix in specified job"""
        steps = self.workflows[workflow]["jobs"][job_id]["steps"]
        matches = [step for step in steps if step.get("uses", "").split("@")[0] == action]
        self.assertEqual(
            len(matches),
            1,
            f"Expected one {action} step in {workflow}:{job_id}",
        )
        return matches[0]

    def release_uploads(self):
        """yield all upload-artifact steps across release workflow jobs"""
        for job_id, job in self.workflows["Release.yml"]["jobs"].items():
            for step in job["steps"]:
                if step.get("uses", "").startswith("actions/upload-artifact@"):
                    yield job_id, job, step["with"]

    def expanded_release_artifacts(self):
        """expand matrix combinations into concrete release artifact names"""
        artifacts = []
        for job_id, job, inputs in self.release_uploads():
            matrix = job.get("strategy", {}).get("matrix", {})
            for values in product(*matrix.values()):
                name = inputs["name"]
                for dimension, value in zip(matrix, values):
                    placeholder = "${{ matrix." + dimension + " }}"
                    name = name.replace(placeholder, value)
                self.assertNotIn("${{", name, f"Unresolved artifact name in {job_id}")
                artifacts.append((name, job_id))
        return artifacts

    def test_all_changed_workflows_use_the_upgraded_actions(self):
        """verify workflow steps use pinned major versions of actions"""
        # scans each job step and asserts version matches pinned ACTION_VERSIONS
        for name, expected_actions in WORKFLOW_ACTIONS.items():
            seen = set()
            for job_id, job in self.workflows[name]["jobs"].items():
                for step in job["steps"]:
                    action, _, version = step.get("uses", "").partition("@")
                    if action in ACTION_VERSIONS:
                        with self.subTest(workflow=name, job=job_id, action=action):
                            self.assertEqual(version, ACTION_VERSIONS[action])
                        seen.add(action)
            # ensures expected actions are actually present in workflow
            with self.subTest(workflow=name):
                self.assertEqual(seen, expected_actions)

    def test_checkout_precedes_repository_operations(self):
        """ensure checkout is the initial step and forbids unsafe PR checkout"""
        for name, workflow in self.workflows.items():
            if "actions/checkout" not in WORKFLOW_ACTIONS[name]:
                continue
            for job_id, job in workflow["jobs"].items():
                if name == "pages.yml" and job_id == "deploy":
                    continue
                with self.subTest(workflow=name, job=job_id):
                    # verifies checkout is the first operational step in job
                    checkout = self.action_step(name, job_id, "actions/checkout")
                    self.assertIs(job["steps"][0], checkout)
                    # checks allow-unsafe-pr-checkout is not enabled
                    self.assertNotEqual(
                        checkout.get("with", {}).get("allow-unsafe-pr-checkout", "false"),
                        "true",
                    )

    def test_history_dependent_jobs_keep_full_checkout_history(self):
        """verify jobs requiring git history retain fetch-depth 0"""
        for name, job_id in (
            ("Release.yml", "validate-and-lint"),
            ("coverage.yml", "coverage"),
        ):
            with self.subTest(workflow=name):
                # asserts fetch-depth is 0 so git describe and coverage work
                checkout = self.action_step(name, job_id, "actions/checkout")
                self.assertEqual(checkout["with"]["fetch-depth"], "0")

    def test_python_setup_retains_explicit_or_matrix_versions(self):
        """validate setup-python configs, matrix versions, and pip cache"""
        for name, workflow in self.workflows.items():
            for job_id, job in workflow["jobs"].items():
                for step in job["steps"]:
                    uses = step.get("uses", "")
                    if not uses.startswith("actions/setup-python@"):
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
                        # checks python-version matches expected semver pattern
                        for selected in versions:
                            self.assertRegex(selected, r"^3\.\d+(?:\.\d+)?$")
                        # verifies pip cache is on and pip-install is absent
                        self.assertEqual(inputs.get("cache", "pip"), "pip")
                        self.assertNotIn("pip-install", inputs)
                        # confirms setup-python runs before any command step
                        first_command = next(
                            index for index, candidate in enumerate(job["steps"]) if "run" in candidate
                        )
                        self.assertLess(job["steps"].index(step), first_command)

    def test_release_upload_names_are_unique_across_platform_and_python_matrices(self):
        """verify matrix combinations produce unique artifact upload names"""
        # expands all matrix dimensions to ensure zero collision among artifacts
        artifacts = self.expanded_release_artifacts()
        names = [name for name, _ in artifacts]
        self.assertTrue(names)
        self.assertEqual(
            len(names),
            len(set(names)),
            "Matrix jobs must not overwrite the same artifact",
        )

    def test_release_uploads_keep_binary_and_wheel_paths(self):
        """verify upload paths and missing-file handling for release assets"""
        expected_paths = {
            "build-matrix": "release-staging/*",
            "build-wheels-windows": "lib/python/dist/*.whl",
            "build-wheels-linux": "./wheelhouse/*.whl",
            "build-wheels-macos": "lib/python/dist/*.whl",
        }
        uploads = list(self.release_uploads())
        self.assertCountEqual([job_id for job_id, _, _ in uploads], expected_paths)
        # validates artifact upload path and failure behavior per job
        for job_id, _, inputs in uploads:
            with self.subTest(job=job_id):
                self.assertEqual(inputs["path"], expected_paths[job_id])
                if job_id == "build-matrix":
                    self.assertEqual(inputs["if-no-files-found"], "ignore")
                else:
                    self.assertNotEqual(inputs.get("if-no-files-found"), "ignore")

    def test_downloads_wait_for_all_artifact_producers(self):
        """ensure release publishing jobs wait on all artifact producer jobs"""
        producers = {job_id for job_id, _, _ in self.release_uploads()}
        self.assertTrue(producers)
        # asserts publish jobs depend on all artifact producers in needs
        for job_id in ("publish-release", "publish-pypi"):
            with self.subTest(job=job_id):
                job = self.workflows["Release.yml"]["jobs"][job_id]
                self.assertTrue(producers.issubset(job["needs"]))
                download = self.action_step("Release.yml", job_id, "actions/download-artifact")
                self.assertEqual(download["with"]["merge-multiple"], "true")

    def test_pypi_download_includes_every_wheel_and_excludes_native_binaries(self):
        """ensure PyPI publication downloads wheels and excludes binaries"""
        download = self.action_step("Release.yml", "publish-pypi", "actions/download-artifact")["with"]
        artifacts = self.expanded_release_artifacts()
        wheels = {name for name, job_id in artifacts if job_id.startswith("build-wheels-")}
        selected = {name for name, _ in artifacts if fnmatchcase(name, download["pattern"])}
        self.assertTrue(wheels)
        # validates wheel pattern selection matches wheels exactly
        self.assertEqual(selected, wheels)
        self.assertNotIn("name", download, "A name filter overrides the wheel pattern")
        self.assertEqual(download["path"], "lib/python/dist")

    def test_github_release_download_path_matches_published_files(self):
        """validate GitHub release action publishes downloaded staging assets"""
        download = self.action_step("Release.yml", "publish-release", "actions/download-artifact")
        publish = self.action_step("Release.yml", "publish-release", "softprops/action-gh-release")
        self.assertNotIn("name", download["with"])
        self.assertNotIn("pattern", download["with"])
        # verifies downloaded files match release published glob
        published_files = publish["with"]["files"].splitlines()
        self.assertIn(download["with"]["path"] + "/*", published_files)
        job = self.workflows["Release.yml"]["jobs"]["publish-release"]
        download_idx = job["steps"].index(download)
        publish_idx = job["steps"].index(publish)
        self.assertLess(download_idx, publish_idx)
        # verifies write permissions for release creation
        self.assertEqual(self.workflows["Release.yml"]["permissions"]["contents"], "write")
        self.assertTrue((ROOT / publish["with"]["body_path"]).is_file())

    def test_qemu_is_ready_before_building_arm64_wheels(self):
        """verify QEMU multi-architecture setup precedes ARM64 cibuildwheel"""
        job = self.workflows["Release.yml"]["jobs"]["build-wheels-linux"]
        qemu = self.action_step("Release.yml", "build-wheels-linux", "docker/setup-qemu-action")
        # confirms qemu configures arm64 platform support
        self.assertIn("arm64", qemu["with"]["platforms"].split(","))
        builds = [step for step in job["steps"] if "python -m cibuildwheel " in step.get("run", "")]
        self.assertEqual(len(builds), 1)
        self.assertIn("aarch64", builds[0]["env"]["CIBW_ARCHS_LINUX"].split())
        # asserts qemu initialization precedes cibuildwheel run
        self.assertLess(job["steps"].index(qemu), job["steps"].index(builds[0]))

    def test_codecov_keeps_both_reports_and_non_blocking_uploads(self):
        """verify codecov action uploads both C++ and Python coverage reports"""
        upload = self.action_step("coverage.yml", "coverage", "codecov/codecov-action")
        # verifies both C++ and Python coverage report files are uploaded
        self.assertEqual(
            set(upload["with"]["files"].split(",")),
            {"coverage_cpp.info", "coverage_py.xml"},
        )
        # verifies non-blocking error behavior and token configuration
        self.assertEqual(upload["with"]["fail_ci_if_error"], "false")
        self.assertEqual(upload["env"]["CODECOV_TOKEN"], "${{ secrets.CODECOV_TOKEN }}")

    def test_dependabot_metadata_output_remains_wired_to_the_major_update_guard(self):
        """verify dependabot automerge guard blocks semver-major updates"""
        workflow = self.workflows["dependabot-automation.yml"]
        job = workflow["jobs"]["dependabot"]
        metadata = self.action_step(
            "dependabot-automation.yml",
            "dependabot",
            "dependabot/fetch-metadata",
        )
        # validates pull_request_target trigger and bot actor condition
        self.assertIn("pull_request_target", workflow["on"])
        self.assertEqual(job["if"], "github.actor == 'dependabot[bot]'")
        self.assertEqual(metadata["with"]["github-token"], "${{ secrets.GITHUB_TOKEN }}")
        consumers = [step for step in job["steps"] if "run" in step]
        self.assertTrue(consumers)
        # verifies steps guard against semver-major dependency updates
        guard = f"steps.{metadata['id']}.outputs.update-type != 'version-update:semver-major'"
        for step in consumers:
            with self.subTest(step=step.get("name")):
                self.assertEqual(step["if"], guard)
                self.assertLess(job["steps"].index(metadata), job["steps"].index(step))
        # confirms job avoids repository checkout to prevent untrusted execution
        self.assertFalse(any(step.get("uses", "").startswith("actions/checkout@") for step in job["steps"]))

    def test_pages_actions_preserve_upload_deploy_order_and_environment_output(self):
        """verify Pages configure, upload, and deploy sequence and config"""
        workflow = self.workflows["pages.yml"]
        build_job = workflow["jobs"]["build"]
        deploy_job = workflow["jobs"]["deploy"]
        configure = self.action_step("pages.yml", "build", "actions/configure-pages")
        upload = self.action_step("pages.yml", "build", "actions/upload-pages-artifact")
        deploy = self.action_step("pages.yml", "deploy", "actions/deploy-pages")
        # verifies configure -> upload step execution sequence in build job
        self.assertLess(build_job["steps"].index(configure), build_job["steps"].index(upload))
        # verifies web build artifact path and presence of package.json
        self.assertEqual(upload["with"]["path"], "web/dist")
        package_json = ROOT / "web" / "package.json"
        self.assertTrue(package_json.is_file())
        self.assertEqual(
            upload.get("with", {}).get("name", "github-pages"),
            deploy.get("with", {}).get("artifact_name", "github-pages"),
        )
        # verifies deployment environment name, URL output binding, and dependencies
        self.assertEqual(deploy_job["environment"]["name"], "github-pages")
        self.assertEqual(
            deploy_job["environment"]["url"],
            "${{ steps." + deploy["id"] + ".outputs.page_url }}",
        )
        self.assertEqual(deploy_job["needs"], "build")
        self.assertEqual(workflow["permissions"]["pages"], "write")
        self.assertEqual(workflow["permissions"]["id-token"], "write")


if __name__ == "__main__":
    unittest.main()
