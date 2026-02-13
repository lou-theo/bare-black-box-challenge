#!/usr/bin/env python3
from __future__ import annotations

import argparse
import difflib
import subprocess
import sys
from dataclasses import dataclass
from pathlib import Path

try:
    import tomllib
except ModuleNotFoundError as exc:
    raise SystemExit("error: Python >= 3.11 is required (missing tomllib)") from exc


class SpecError(Exception):
    pass


@dataclass(frozen=True)
class TestCase:
    category: str
    category_description: str
    case_id: str
    description: str
    input_text: str
    expected_output: str
    expected_code: int
    source_file: Path


def _die(msg: str) -> int:
    print(f"error: {msg}", file=sys.stderr)
    return 1


def _assert_non_empty_str(data: dict, key: str, *, source: Path) -> str:
    value = data.get(key)
    if not isinstance(value, str) or value.strip() == "":
        raise SpecError(f"{source}: '{key}' must be a non-empty string")
    return value


def _assert_str(data: dict, key: str, *, source: Path) -> str:
    value = data.get(key)
    if not isinstance(value, str):
        raise SpecError(f"{source}: '{key}' must be a string")
    return value


def _assert_code(data: dict, key: str, *, source: Path) -> int:
    value = data.get(key)
    if not isinstance(value, int) or value < 0:
        raise SpecError(f"{source}: '{key}' must be an integer >= 0")
    return value


def load_specs(spec_dir: Path) -> list[TestCase]:
    spec_files = sorted(spec_dir.glob("*.toml"))
    if not spec_files:
        raise SpecError(f"no spec file found in '{spec_dir}'")

    cases: list[TestCase] = []
    seen_ids: dict[str, Path] = {}

    for spec_file in spec_files:
        with spec_file.open("rb") as fh:
            data = tomllib.load(fh)

        if not isinstance(data, dict):
            raise SpecError(f"{spec_file}: root must be a TOML table")

        category = _assert_non_empty_str(data, "category", source=spec_file)
        category_description = _assert_non_empty_str(
            data, "category_description", source=spec_file
        )

        raw_cases = data.get("case")
        if not isinstance(raw_cases, list) or not raw_cases:
            raise SpecError(f"{spec_file}: must declare at least one [[case]] entry")

        for idx, raw_case in enumerate(raw_cases, start=1):
            if not isinstance(raw_case, dict):
                raise SpecError(f"{spec_file}: case #{idx} must be a TOML table")

            case_id = _assert_non_empty_str(raw_case, "id", source=spec_file)
            description = _assert_non_empty_str(raw_case, "description", source=spec_file)
            input_text = _assert_str(raw_case, "input", source=spec_file)
            expected_output = _assert_str(raw_case, "expected_output", source=spec_file)
            expected_code = _assert_code(raw_case, "expected_code", source=spec_file)

            if case_id in seen_ids:
                previous = seen_ids[case_id]
                raise SpecError(
                    f"duplicate case id '{case_id}' in {spec_file} (already defined in {previous})"
                )
            seen_ids[case_id] = spec_file

            cases.append(
                TestCase(
                    category=category,
                    category_description=category_description,
                    case_id=case_id,
                    description=description,
                    input_text=input_text,
                    expected_output=expected_output,
                    expected_code=expected_code,
                    source_file=spec_file,
                )
            )

    return cases


def filter_cases(
    cases: list[TestCase], category: str | None, case_id: str | None
) -> list[TestCase]:
    selected = cases

    if category is not None:
        selected = [case for case in selected if case.category == category]
        if not selected:
            raise SpecError(f"no case found for category '{category}'")

    if case_id is not None:
        selected = [case for case in selected if case.case_id == case_id]
        if not selected:
            raise SpecError(f"no case found for id '{case_id}'")

    return selected


def _hex_dump(payload: bytes) -> str:
    if not payload:
        return "  <empty>"
    lines: list[str] = []
    for i in range(0, len(payload), 16):
        chunk = payload[i : i + 16]
        lines.append("  " + " ".join(f"{byte:02x}" for byte in chunk))
    return "\n".join(lines)


def _print_diff(expected: bytes, actual: bytes) -> None:
    expected_text = expected.decode("utf-8", errors="replace").splitlines(keepends=True)
    actual_text = actual.decode("utf-8", errors="replace").splitlines(keepends=True)
    diff = "".join(
        difflib.unified_diff(
            expected_text,
            actual_text,
            fromfile="expected",
            tofile="actual",
            lineterm="",
        )
    )
    if diff:
        print("  diff (- expected, + actual):")
        print(diff)
    else:
        print("  diff unavailable (binary-only difference)")

    print("  expected bytes (hex):")
    print(_hex_dump(expected))
    print("  actual bytes (hex):")
    print(_hex_dump(actual))


def list_cases(cases: list[TestCase]) -> int:
    categories: dict[str, tuple[str, list[TestCase]]] = {}
    for case in cases:
        if case.category not in categories:
            categories[case.category] = (case.category_description, [])
        categories[case.category][1].append(case)

    for category in sorted(categories):
        description, category_cases = categories[category]
        print(f"{category}: {description}")
        for case in category_cases:
            print(f"  {case.case_id} - {case.description}")
    return 0


def run_cases(cases: list[TestCase], binary_path: Path) -> int:
    if not binary_path.is_file():
        return _die(f"missing executable '{binary_path}' (run 'make bbr')")

    total = 0
    passed = 0
    failed = 0
    summary_by_category: dict[str, list[int]] = {}

    for case in cases:
        total += 1
        if case.category not in summary_by_category:
            summary_by_category[case.category] = [0, 0]
        summary_by_category[case.category][1] += 1

        proc = subprocess.run(
            [str(binary_path)],
            input=case.input_text.encode("utf-8"),
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            check=False,
        )

        actual_code = proc.returncode
        actual_output = proc.stdout
        expected_output = case.expected_output.encode("utf-8")

        case_ok = True
        header_printed = False

        if actual_code != case.expected_code:
            if not header_printed:
                print(f"not ok {case.case_id} [{case.category}]")
                print(f"  description: {case.description}")
                header_printed = True
            print(
                f"  exit code mismatch: expected {case.expected_code}, got {actual_code}"
            )
            case_ok = False

        if actual_output != expected_output:
            if not header_printed:
                print(f"not ok {case.case_id} [{case.category}]")
                print(f"  description: {case.description}")
                header_printed = True
            print("  stdout mismatch")
            _print_diff(expected_output, actual_output)
            case_ok = False

        if case_ok:
            print(f"ok {case.case_id} [{case.category}]")
            passed += 1
            summary_by_category[case.category][0] += 1
        else:
            failed += 1

    print(f"summary: {passed}/{total} passed, {failed} failed")
    print("by category:")
    for category in sorted(summary_by_category):
        cat_passed, cat_total = summary_by_category[category]
        print(f"  {category}: {cat_passed}/{cat_total}")

    return 0 if failed == 0 else 1


def parse_args(argv: list[str]) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="Run bbr binary tests from TOML specs")
    parser.add_argument(
        "--list",
        action="store_true",
        help="List available categories and test cases without executing",
    )
    parser.add_argument(
        "--category",
        help="Run only a specific category",
    )
    parser.add_argument(
        "--case-id",
        help="Run only a specific case id",
    )
    return parser.parse_args(argv)


def main(argv: list[str]) -> int:
    args = parse_args(argv)

    repo_root = Path(__file__).resolve().parents[1]
    spec_dir = repo_root / "tests" / "spec"
    binary_path = repo_root / "bin" / "bbr"

    try:
        cases = load_specs(spec_dir)
        selected_cases = filter_cases(cases, args.category, args.case_id)
    except SpecError as exc:
        return _die(str(exc))

    if args.list:
        return list_cases(selected_cases)

    return run_cases(selected_cases, binary_path)


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
