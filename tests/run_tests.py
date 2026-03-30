#!/usr/bin/env python3

from __future__ import annotations

import pathlib
import subprocess
import sys
import tempfile


def read_optional_text(path: pathlib.Path) -> str | None:
    if not path.exists():
        return None
    return path.read_text(encoding="utf-8")


def read_optional_lines(path: pathlib.Path) -> list[str]:
    text = read_optional_text(path)
    if text is None:
        return []
    return [line for line in text.splitlines() if line.strip()]


def require_substrings(
    text: str, substrings: list[str], *, label: str, case_name: str
) -> list[str]:
    failures: list[str] = []
    for substring in substrings:
        if substring not in text:
            failures.append(
                f"{case_name}: missing {label} substring {substring!r}"
            )
    return failures


def forbid_substrings(
    text: str, substrings: list[str], *, label: str, case_name: str
) -> list[str]:
    failures: list[str] = []
    for substring in substrings:
        if substring in text:
            failures.append(
                f"{case_name}: unexpected {label} substring {substring!r}"
            )
    return failures


def run_compiler(
    binary: pathlib.Path, path: pathlib.Path, *extra_args: str
) -> subprocess.CompletedProcess[str]:
    return subprocess.run(
        [str(binary), str(path), *extra_args],
        capture_output=True,
        text=True,
        check=False,
    )


def run_case(binary: pathlib.Path, path: pathlib.Path) -> tuple[bool, str]:
    expected = path.with_suffix(".expected")
    want_ok = expected.read_text(encoding="utf-8").strip() == "ok"
    expected_stderr = read_optional_lines(path.with_suffix(".stderr"))
    expected_ir = read_optional_lines(path.with_suffix(".ir"))
    forbidden_ir = read_optional_lines(path.with_suffix(".irnot"))
    expected_run = read_optional_text(path.with_suffix(".run"))
    support_c = path.with_suffix(".c")

    check_proc = run_compiler(binary, path, "--check")
    compiler_output = check_proc.stderr + check_proc.stdout
    ok = check_proc.returncode == 0
    if ok != want_ok:
        return (
            False,
            f"{path.name}: expected {'ok' if want_ok else 'error'}, "
            f"got {check_proc.returncode}\n{compiler_output}",
        )

    stderr_failures = require_substrings(
        compiler_output,
        expected_stderr,
        label="diagnostic",
        case_name=path.name,
    )
    if stderr_failures:
        return False, "\n".join(stderr_failures + [compiler_output])

    if not want_ok:
        return True, ""

    with tempfile.TemporaryDirectory() as temp_dir_text:
        temp_dir = pathlib.Path(temp_dir_text)
        llvm_path = temp_dir / f"{path.stem}.ll"
        emit_llvm_proc = run_compiler(
            binary, path, "--emit-llvm", "-o", str(llvm_path)
        )
        if emit_llvm_proc.returncode != 0:
            output = emit_llvm_proc.stderr + emit_llvm_proc.stdout
            return (
                False,
                f"{path.name}: semantic check passed but LLVM emission failed\n"
                f"{output}",
            )

        llvm_ir = llvm_path.read_text(encoding="utf-8")
        ir_failures = require_substrings(
            llvm_ir, expected_ir, label="LLVM IR", case_name=path.name
        )
        ir_failures.extend(
            forbid_substrings(
                llvm_ir,
                forbidden_ir,
                label="LLVM IR",
                case_name=path.name,
            )
        )
        if ir_failures:
            return False, "\n".join(ir_failures + [llvm_ir])

        if expected_run is None:
            return True, ""

        object_path = temp_dir / f"{path.stem}.o"
        emit_object_proc = run_compiler(binary, path, "-o", str(object_path))
        if emit_object_proc.returncode != 0:
            output = emit_object_proc.stderr + emit_object_proc.stdout
            return (
                False,
                f"{path.name}: object emission failed before runtime check\n"
                f"{output}",
            )

        exe_path = temp_dir / path.stem
        link_command = ["cc", str(object_path)]
        if support_c.exists():
            link_command.append(str(support_c))
        link_command.extend(["-o", str(exe_path)])
        link_proc = subprocess.run(
            link_command,
            capture_output=True,
            text=True,
            check=False,
        )
        if link_proc.returncode != 0:
            output = link_proc.stderr + link_proc.stdout
            return False, f"{path.name}: failed to link runtime test\n{output}"

        run_proc = subprocess.run(
            [str(exe_path)],
            capture_output=True,
            text=True,
            check=False,
        )
        expected_exit = int(expected_run.strip())
        if run_proc.returncode != expected_exit:
            output = run_proc.stdout + run_proc.stderr
            return (
                False,
                f"{path.name}: expected runtime exit code {expected_exit}, "
                f"got {run_proc.returncode}\n{output}",
            )

    return True, ""


def main() -> int:
    binary = pathlib.Path(sys.argv[1])
    tests_dir = pathlib.Path(sys.argv[2])
    cases = sorted(tests_dir.glob("*.cyan"))
    failures: list[str] = []
    for case in cases:
        passed, detail = run_case(binary, case)
        if not passed:
            failures.append(detail)

    if failures:
        print("\n".join(failures))
        return 1
    print(f"passed {len(cases)} test cases")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
