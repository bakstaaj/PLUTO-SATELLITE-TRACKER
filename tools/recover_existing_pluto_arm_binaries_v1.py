#!/usr/bin/env python3
# Recover/reuse existing Pluto Satellite Tracker ARM binaries and build-tool evidence.
#
# This does not build anything.
#
# It scans:
#   - current repo
#   - ~/sdrdev
#   - ~/sdrdev sibling Pluto project folders
#
# If it finds ARM ELF binaries named:
#   pluto_sat_tracker
#   pluto_fm_receiver
# it copies them into:
#   ./build/pluto_sat_tracker
#   ./build/pluto_fm_receiver
#
# It also reports likely existing build scripts/Dockerfiles so we can reuse the
# exact known-good build path instead of inventing another Docker flow.
#
# Run:
#   python tools/recover_existing_pluto_arm_binaries_v1.py .

from __future__ import annotations

import os
import shutil
import stat
import sys
from pathlib import Path

TARGETS = {"pluto_sat_tracker", "pluto_fm_receiver"}

BUILD_SCRIPT_KEYWORDS = (
    "build",
    "cross",
    "docker",
    "pluto",
    "arm",
    "armhf",
    "gnueabihf",
)

SKIP_PARTS = {
    ".git",
    ".venv",
    "venv",
    "__pycache__",
    "node_modules",
    ".pytest_cache",
    ".mypy_cache",
}


def is_skipped(path: Path) -> bool:
    return any(part in SKIP_PARTS for part in path.parts)


def elf_machine(path: Path) -> str:
    try:
        data = path.read_bytes()[:64]
    except Exception:
        return "unreadable"
    if len(data) < 20 or data[:4] != b"\x7fELF":
        return "not_elf"
    endian = data[5]
    if endian == 1:
        machine = int.from_bytes(data[18:20], "little")
    elif endian == 2:
        machine = int.from_bytes(data[18:20], "big")
    else:
        return "elf_unknown_endian"
    machines = {
        3: "x86",
        40: "ARM",
        62: "x86_64",
        183: "AARCH64",
    }
    return machines.get(machine, f"ELF_MACHINE_{machine}")


def executable(path: Path) -> bool:
    try:
        mode = path.stat().st_mode
    except Exception:
        return False
    return bool(mode & (stat.S_IXUSR | stat.S_IXGRP | stat.S_IXOTH))


def search_roots(repo: Path) -> list[Path]:
    roots = [repo]
    home = Path.home()
    sdrdev = home / "sdrdev"
    if sdrdev.exists():
        roots.append(sdrdev)
    # Avoid duplicates while preserving order.
    out = []
    seen = set()
    for r in roots:
        try:
            rp = r.resolve()
        except Exception:
            rp = r
        if rp not in seen and rp.exists():
            seen.add(rp)
            out.append(rp)
    return out


def find_binaries(repo: Path) -> dict[str, list[Path]]:
    found: dict[str, list[Path]] = {name: [] for name in TARGETS}
    for root in search_roots(repo):
        for path in root.rglob("*"):
            if is_skipped(path):
                continue
            if not path.is_file():
                continue
            if path.name in TARGETS:
                found[path.name].append(path)
    return found


def score_binary(path: Path) -> int:
    score = 0
    m = elf_machine(path)
    if m == "ARM":
        score += 100
    if executable(path):
        score += 20
    s = str(path).lower()
    if "/build/" in s or "\\build\\" in s:
        score += 10
    if "/dist/" in s or "\\dist\\" in s:
        score += 8
    if "/bin/" in s or "\\bin\\" in s:
        score += 6
    if ".bak" in s:
        score -= 20
    return score


def find_build_scripts(repo: Path) -> list[Path]:
    candidates = []
    for root in search_roots(repo):
        for path in root.rglob("*"):
            if is_skipped(path):
                continue
            if not path.is_file():
                continue
            name = path.name.lower()
            if name.startswith("dockerfile") or name.endswith(".dockerfile"):
                candidates.append(path)
                continue
            if path.suffix.lower() not in {".sh", ".py", ".mk", ".cmake", ""}:
                continue
            if all(keyword in name for keyword in ("build", "pluto")):
                candidates.append(path)
                continue
            if any(keyword in name for keyword in BUILD_SCRIPT_KEYWORDS) and ("build" in name or "cross" in name):
                candidates.append(path)
    # Prefer current repo tools first.
    candidates = sorted(set(candidates), key=lambda p: (0 if repo in p.parents or p == repo else 1, str(p)))
    return candidates


def copy_best(found: dict[str, list[Path]], repo: Path) -> bool:
    build_dir = repo / "build"
    build_dir.mkdir(exist_ok=True)
    ok = True

    for name in sorted(TARGETS):
        choices = sorted(found[name], key=score_binary, reverse=True)
        arm_choices = [p for p in choices if elf_machine(p) == "ARM"]
        if not arm_choices:
            print(f"FAIL: no ARM ELF candidate found for {name}")
            if choices:
                print(f"  non-ARM candidates:")
                for p in choices[:8]:
                    print(f"    {p} machine={elf_machine(p)} executable={executable(p)} score={score_binary(p)}")
            ok = False
            continue

        best = arm_choices[0]
        dest = build_dir / name
        if best.resolve() != dest.resolve():
            shutil.copy2(best, dest)
        dest.chmod(dest.stat().st_mode | stat.S_IXUSR | stat.S_IXGRP | stat.S_IXOTH)
        print(f"PASS: copied {name}")
        print(f"  from: {best}")
        print(f"  to:   {dest}")
        print(f"  machine={elf_machine(dest)} executable={executable(dest)} size={dest.stat().st_size}")

    return ok


def main() -> int:
    repo = Path(sys.argv[1]) if len(sys.argv) > 1 else Path(".")
    repo = repo.resolve()

    print("Recover existing Pluto ARM binaries")
    print(f"Repo: {repo}")
    print()

    found = find_binaries(repo)
    for name in sorted(TARGETS):
        print(f"Candidates for {name}: {len(found[name])}")
        for p in sorted(found[name], key=score_binary, reverse=True)[:12]:
            print(f"  score={score_binary(p):3d} machine={elf_machine(p):8s} exec={str(executable(p)):5s} {p}")
        print()

    copied_ok = copy_best(found, repo)

    print()
    print("Likely existing build scripts / Dockerfiles:")
    scripts = find_build_scripts(repo)
    if scripts:
        for p in scripts[:80]:
            print(f"  {p}")
    else:
        print("  none found")

    print()
    if copied_ok:
        print("PASS: recovered ARM binaries into ./build/")
        print("Next:")
        print("  ./tools/validate_pluto_arm_binaries_v2.sh .")
        print("  ./tools/build_sd_card_installer_folder_v1.sh")
        print("  ./tools/validate_sd_card_installer_folder_v1.sh dist/sd_card/PlutoSatelliteTrackerInstall")
        return 0

    print("FAIL: could not recover both ARM binaries.")
    print("Use the likely build-script list above to rerun the exact previous build path, or provide the path to the old dist/bin folder.")
    return 1


if __name__ == "__main__":
    raise SystemExit(main())
