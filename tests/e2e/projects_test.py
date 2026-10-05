"""Client-visible startup and REST behavior on isolated emulator processes."""
import contextlib
import json
import os
from pathlib import Path
import subprocess
import tempfile

BINARY = str(Path("bazel-bin/bigquery-emulator-duckdb").resolve())


@contextlib.contextmanager
def server(*args, cwd=None):
    process = subprocess.Popen(
        [BINARY, "--host", "127.0.0.1", "--port", "0", *args],
        cwd=cwd, stderr=subprocess.PIPE, text=True,
    )
    try:
        for line in process.stderr:
            if " listening on " in line:
                yield line.split(" listening on ", 1)[1].strip()
                break
        else:
            raise AssertionError(f"emulator exited before listening: {process.wait()}")
    finally:
        process.terminate()
        process.communicate(timeout=15)
        assert process.returncode == 0, process.returncode


def runbook(url, name):
    env = dict(os.environ, PROJECTS_API=url)
    subprocess.run(["runn", "run", "tests/e2e/projects/" + name],
                   env=env, stdin=subprocess.DEVNULL, check=True, timeout=90)


def rejected(*args):
    result = subprocess.run([BINARY, *args], capture_output=True, text=True, timeout=15)
    assert result.returncode != 0, args
    assert "listening on" not in result.stderr, args


with server() as url:
    runbook(url, "rest_empty.yml")

with tempfile.TemporaryDirectory() as tmp:
    directory = Path(tmp)
    project = {"projectId": "z-configured", "numericId": "123456789012", "friendlyName": ""}
    (directory / "project.json").write_text(json.dumps(project))
    args = ["--data-dir", str(directory / "data"), "--project=@project.json"]
    args += ["--project=" + json.dumps({"projectId": f"p{i:02}"}) for i in range(51)]
    with server(*args, cwd=tmp) as url:
        runbook(url, "rest_projects.yml")

    # Restore empty registrations and metadata without supplying --project again.
    with server("--data-dir", str(directory / "data")) as url:
        runbook(url, "rest_restore.yml")

    # An explicit registration replaces metadata; omitted projects are retained.
    with server("--data-dir", str(directory / "data"),
                '--project={"projectId":"z-configured","friendlyName":"Updated"}') as url:
        runbook(url, "rest_update.yml")

    # Invalid startup configurations must not alter persisted registrations.
    saved = (directory / "data" / "projects.json").read_bytes()
    rejected("--data-dir", str(directory / "data"),
             '--project={"projectId":"a","numericId":"42"}',
             '--project={"projectId":"b","numericId":"42"}')
    assert (directory / "data" / "projects.json").read_bytes() == saved
    (directory / "bad.json").write_text('{"projectId":"bad"} trailing')
    rejected("--project=@" + str(directory / "bad.json"))
    rejected("--project=" + str(directory / "project.json"))
    rejected("--project=@" + str(directory / "missing.json"))

for value in ["[]", "null", "{}", '{"projectId":""}', '{"projectId":"a/b"}',
              '{"projectId":"p","other":"x"}', '{"projectId":123}',
              '{"projectId":"p","friendlyName":null}',
              '{"projectId":"p","numericId":123}',
              '{"projectId":"p","numericId":"0"}',
              '{"projectId":"p","numericId":"01"}',
              '{"projectId":"p","numericId":"18446744073709551616"}']:
    rejected("--project=" + value)
rejected('--project={"projectId":"p"}', '--project={"projectId":"p"}')
rejected('--project={"projectId":"p","numericId":"42"}', '--project={"projectId":"42"}')
rejected("--project=@-")
rejected("--project=@")
