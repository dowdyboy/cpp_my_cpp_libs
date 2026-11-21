#!/usr/bin/env python3
"""Generate or check the canonical test-count table.

The test binary is the single source of truth: `mylib_tests --list` prints one
`suite:case` line per registered MYTEST_CASE. Docs used to hard-code these
numbers and they drifted every release, so the table now lives in
`docs/test-counts.md` and CI fails when it no longer matches the binary.

Usage:
    python tools/gen_test_counts.py --exe build/ci-cv/bin/mylib_tests --write
    python tools/gen_test_counts.py --exe build/ci-cv/bin/mylib_tests --check

`--write` regenerates docs/test-counts.md; `--check` (the default) compares it
with the binary and exits non-zero on drift.
"""

from __future__ import annotations

import argparse
import collections
import pathlib
import subprocess
import sys

DEFAULT_OUTPUT = pathlib.Path(__file__).resolve().parent.parent / "docs" / "test-counts.md"

HEADER = """# 测试用例数（自动生成，不要手改）

> 单一来源：`mylib_tests --list` 的输出。
> 重新生成：`python tools/gen_test_counts.py --exe <mylib_tests> --write`
> CI：`python tools/gen_test_counts.py --exe <mylib_tests> --check`（数字漂移直接失败）。
>
> 这张表是"全组件构建"（含 cv）的数字；关掉 cv 的构建少 12 条。
> 其它文档引用用例数时请指向本文件，不要手写。

| 套件 | 用例数 |
|---|---:|
"""

FOOTER = """
合计：**{total}** 条（`mylib_tests --list` 的去重计数；CTest 另加 2 条守卫）。

可选用例：`cv`（12 条）只在 `MYLIB_ENABLE_CV=ON` 时注册；`ml` 在没有推理后端时
会整组 SKIP，但条数不变。
"""


def parse_args() -> argparse.Namespace:
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--exe", required=True, help="path to the mylib_tests binary")
    p.add_argument("--output", default=str(DEFAULT_OUTPUT), help="canonical markdown file")
    mode = p.add_mutually_exclusive_group()
    mode.add_argument("--write", action="store_true", help="regenerate the canonical file")
    mode.add_argument("--check", action="store_true", help="fail if the file is out of date")
    return p.parse_args()


def collect(exe: str) -> tuple[collections.OrderedDict[str, int], int]:
    try:
        out = subprocess.run(
            [exe, "--list"], check=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True
        ).stdout
    except FileNotFoundError:
        sys.exit(f"error: test binary not found: {exe}")
    except subprocess.CalledProcessError as exc:
        sys.exit(f"error: `{exe} --list` failed (exit {exc.returncode}):\n{exc.stdout}")

    counts: collections.OrderedDict[str, int] = collections.OrderedDict()
    for line in out.splitlines():
        line = line.strip()
        if not line or ":" not in line:
            continue
        suite, _case = line.split(":", 1)
        counts[suite] = counts.get(suite, 0) + 1
    if not counts:
        sys.exit("error: no `suite:case` lines in --list output")
    return counts, sum(counts.values())


def render(counts: collections.OrderedDict[str, int]) -> str:
    rows = "".join(f"| `{suite}` | {n} |\n" for suite, n in sorted(counts.items()))
    return HEADER + rows + FOOTER.format(total=sum(counts.values()))


def main() -> int:
    args = parse_args()
    counts, total = collect(args.exe)
    expected = render(counts)
    path = pathlib.Path(args.output)

    if args.write:
        path.write_text(expected, encoding="utf-8", newline="\n")
        print(f"wrote {path} ({total} cases, {len(counts)} suites)")
        return 0

    if not path.exists():
        print(f"error: {path} does not exist; run with --write first", file=sys.stderr)
        return 1
    actual = path.read_text(encoding="utf-8")
    if actual.replace("\r\n", "\n") != expected:
        print(f"error: {path} is out of date with the test binary", file=sys.stderr)
        print("       run: python tools/gen_test_counts.py --exe <mylib_tests> --write", file=sys.stderr)
        # Show a compact diff of suite counts to make the failure actionable.
        import re

        def table_counts(text: str) -> dict[str, int]:
            return {m.group(1): int(m.group(2)) for m in re.finditer(r"^\| `([^`]+)` \| (\d+) \|$", text, re.M)}

        want, have = counts, table_counts(actual)
        for suite in sorted(set(want) | set(have)):
            if want.get(suite) != have.get(suite):
                print(f"       {suite}: binary={want.get(suite)} doc={have.get(suite)}", file=sys.stderr)
        return 1
    print(f"ok: {path} matches the binary ({total} cases, {len(counts)} suites)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
