from __future__ import annotations

from pathlib import Path

import yaml


REPO_ROOT = Path(__file__).resolve().parents[2]
WORKFLOW_PATH = REPO_ROOT / ".github" / "workflows" / "test.yml"


def load_test_workflow() -> dict[object, object]:
    return yaml.safe_load(WORKFLOW_PATH.read_text(encoding="utf-8"))


def get_workflow_on_block(workflow: dict[object, object]) -> dict[str, object]:
    on_block = workflow.get("on")
    if isinstance(on_block, dict):
        return on_block
    legacy_on_block = workflow.get(True)
    if isinstance(legacy_on_block, dict):
        return legacy_on_block
    raise AssertionError("Workflow is missing the top-level 'on' block.")


def get_job(workflow: dict[object, object], job_name: str) -> dict[str, object]:
    jobs = workflow.get("jobs")
    if not isinstance(jobs, dict):
        raise AssertionError("Workflow is missing the jobs block.")
    job = jobs.get(job_name)
    if not isinstance(job, dict):
        raise AssertionError(f"Workflow is missing the '{job_name}' job.")
    return job


def get_step(job: dict[str, object], step_name: str) -> dict[str, object]:
    steps = job.get("steps")
    if not isinstance(steps, list):
        raise AssertionError("Job is missing steps.")
    for step in steps:
        if isinstance(step, dict) and step.get("name") == step_name:
            return step
    raise AssertionError(f"Job is missing step '{step_name}'.")


def test_ci_workflow_exposes_opt_in_release_candidate_dispatch_input() -> None:
    workflow = load_test_workflow()
    on_block = get_workflow_on_block(workflow)
    workflow_dispatch = on_block.get("workflow_dispatch")

    assert isinstance(workflow_dispatch, dict)
    assert isinstance(workflow_dispatch.get("inputs"), dict)

    release_candidate_input = workflow_dispatch["inputs"].get("run_release_candidate_gate")
    assert isinstance(release_candidate_input, dict)
    assert release_candidate_input["type"] == "boolean"
    assert release_candidate_input["required"] is False
    assert release_candidate_input["default"] is False


def test_ci_workflow_defines_dedicated_self_hosted_release_candidate_job() -> None:
    workflow = load_test_workflow()
    job = get_job(workflow, "cemu-release-candidate")

    assert job["name"] == "CEmu Release Candidate Gate"
    assert job["runs-on"] == ["self-hosted", "windows", "cemu"]
    assert "vars.CEMU_RELEASE_CANDIDATE_GATE_ENABLED == 'true'" in str(job["if"])
    assert "github.event.inputs.run_release_candidate_gate == 'true'" in str(job["if"])


def test_ci_workflow_release_candidate_job_runs_gate_and_uploads_artifacts() -> None:
    workflow = load_test_workflow()
    job = get_job(workflow, "cemu-release-candidate")

    prepare_step = get_step(job, "Prepare release candidate artifact directory")
    gate_step = get_step(job, "Run release candidate confidence gate")
    upload_step = get_step(job, "Upload release candidate artifacts")

    prepare_script = str(prepare_step["run"])
    gate_script = str(gate_step["run"])

    assert "calcengine-cemu-release-candidate" in prepare_script
    assert "CALCENGINE_CEMU_RELEASE_CANDIDATE_ROOT" in prepare_script
    assert "CALCENGINE_CEMU_RELEASE_CANDIDATE_CONFIDENCE_PATH" in prepare_script
    assert "CALCENGINE_CEMU_RELEASE_CANDIDATE_EVIDENCE_DIR" in prepare_script
    assert "python .\\scripts\\cemu\\release_candidate_gate.py" in gate_script
    assert "--output \"$env:CALCENGINE_CEMU_RELEASE_CANDIDATE_CONFIDENCE_PATH\"" in gate_script
    assert "--evidence-dir \"$env:CALCENGINE_CEMU_RELEASE_CANDIDATE_EVIDENCE_DIR\"" in gate_script
    assert upload_step["uses"] == "actions/upload-artifact@v4"
    assert upload_step["with"]["name"] == "cemu-release-candidate-artifacts"
    assert upload_step["with"]["path"] == "${{ env.CALCENGINE_CEMU_RELEASE_CANDIDATE_ROOT }}"


def test_ci_workflow_python_job_installs_yaml_for_workflow_validation_tests() -> None:
    workflow = load_test_workflow()
    job = get_job(workflow, "cemu-python")
    install_step = get_step(job, "Install Python test dependencies")

    assert "pyyaml" in str(install_step["run"]).lower()
