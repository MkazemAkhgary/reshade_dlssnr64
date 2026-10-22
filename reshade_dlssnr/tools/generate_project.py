#!/usr/bin/env python3
"""Regenerate reshade_dlssnr.vcxproj file lists and reshade_dlssnr.sln.

Scans the project directory for .cpp and .h files (skipping third_party/,
build/, generated/, shaders/, and dot directories) and shaders/ for shader
sources, then rewrites the discovery-driven ItemGroups. PropertyGroup,
ItemDefinitionGroup, Import, Target, UpToDateCheckOutput, and
ProjectReference blocks are left untouched: they describe settings and build
logic, not file lists, and are not derivable from disk contents.

The script does not run git. Whether to commit the regenerated files, and
whether they should be tracked at all, is the caller's decision.

Usage:
  python generate_project.py           # regenerate in place
  python generate_project.py --check   # exit 1 if regeneration would change
                                       # either file (useful in CI or hooks)

Directory ordering:
  Top-level files come first, then files under the directories listed in
  HEADER_DIR_ORDER / SOURCE_DIR_ORDER in that order. Any top-level directory
  that is not listed but contains relevant files is placed alphabetically
  after the known ones, and its name is reported. To pin a new directory to
  a specific position, add it to the relevant list.
"""

import argparse
import difflib
import re
import sys
from pathlib import Path

PROJECT_DIR = Path(__file__).resolve().parent.parent
VCXPROJ = PROJECT_DIR / "reshade_dlssnr.vcxproj"
SLN = PROJECT_DIR / "reshade_dlssnr.sln"

# ---------------------------------------------------------------------------
# Configuration
# ---------------------------------------------------------------------------

SKIP_DIRS = {
    "third_party", "build", "generated", "shaders",
    ".git", ".vs", ".vscode", "__pycache__",
}

GENERATED_HEADERS = [
    "generated/shader_sources.h",
    "generated/shader_bytecode.h",
]
GENERATED_SOURCES = [
    "generated/shader_sources.cpp",
    "generated/shader_bytecode.cpp",
]

PCH_CREATE = "pch.cpp"
PCH_NOT_USING_PLAIN = set(GENERATED_SOURCES)
PCH_NOT_USING_CONDITIONAL = {
    "third_party/tracy/public/TracyClient.cpp": "'$(DlssnrTracyEnabled)' == 'true'",
}

NON_COMPILE_SOURCES = {
    "ctl/ctl_acceptance.cpp",
    "ctl/test_imgui_stubs.cpp",
    "ctl/test_row_stubs.cpp",
}

SPECIAL_HEADERS = [
    "project_constants.h",
    "pch.h",
    "profiling/tracy_integration.h",
    "profiling/profile.h",
    *GENERATED_HEADERS,
]
SPECIAL_SOURCES = [
    "addon.cpp",
    "pch.cpp",
    "profiling/tracy_integration.cpp",
    "third_party/tracy/public/TracyClient.cpp",
    *GENERATED_SOURCES,
]

HEADER_DIR_ORDER = [
    "", "profiling", "generated", "gpu", "hooks", "pipeline", "debug",
    "model", "hdr", "compose", "grade", "temporal", "importance_mask",
    "state", "ui", "ctl", "fragments", "feature",
]
SOURCE_DIR_ORDER = [
    "", "profiling", "gpu", "hooks", "pipeline", "debug", "model", "hdr",
    "compose", "grade", "temporal", "importance_mask", "state", "ui", "ctl",
    "fragments", "feature",
]


# ---------------------------------------------------------------------------
# Discovery
# ---------------------------------------------------------------------------

def discover(root: Path, exts):
    out = []
    for f in sorted(root.rglob("*")):
        if not f.is_file() or f.suffix not in exts:
            continue
        rel = f.relative_to(root)
        if any(part in SKIP_DIRS for part in rel.parts):
            continue
        out.append(rel.as_posix())
    return out


def discover_shaders(root: Path):
    shader_root = root / "shaders"
    return [
        f.relative_to(shader_root).as_posix()
        for f in sorted(shader_root.rglob("*"))
        if f.is_file() and f.suffix in {".h", ".hlsli", ".hlsl"}
    ]


def top_dirs(paths):
    """Top-level directory of each path ("" for root-level files)."""
    return {p.split("/")[0] if "/" in p else "" for p in paths}


def report_unknown_dirs(headers, sources):
    """Report top-level directories discovered but not in the priority lists.
    They still sort after the known ones alphabetically; this is informational
    so a new directory is visible in the output rather than silent."""
    known_h = set(HEADER_DIR_ORDER)
    known_s = set(SOURCE_DIR_ORDER)
    new_h = top_dirs(headers) - known_h
    new_s = top_dirs(sources) - known_s
    if new_h:
        print(
            f"note: unlisted header dir(s) will sort after the known ones: "
            f"{', '.join(sorted(new_h))}",
            file=sys.stderr,
        )
    if new_s:
        print(
            f"note: unlisted source dir(s) will sort after the known ones: "
            f"{', '.join(sorted(new_s))}",
            file=sys.stderr,
        )


def sort_key(path: str, order):
    parts = path.split("/")
    d = parts[0] if len(parts) > 1 else ""
    try:
        pri = order.index(d)
    except ValueError:
        pri = len(order)
    return (pri, path)


def to_backslash(p: str) -> str:
    return p.replace("/", "\\")


# ---------------------------------------------------------------------------
# Emit
# ---------------------------------------------------------------------------

def emit_cl_includes(headers):
    special = set(SPECIAL_HEADERS)
    rest = sorted(
        (h for h in headers if h not in special),
        key=lambda p: sort_key(p, HEADER_DIR_ORDER),
    )
    return [
        f'    <ClInclude Include="{to_backslash(h)}" />'
        for h in SPECIAL_HEADERS + rest
    ]


def emit_cl_compiles(sources):
    special = set(SPECIAL_SOURCES)
    rest = sorted(
        (s for s in sources
         if s not in special and s not in NON_COMPILE_SOURCES),
        key=lambda p: sort_key(p, SOURCE_DIR_ORDER),
    )
    lines = []
    for path in SPECIAL_SOURCES:
        ws = to_backslash(path)
        if path == PCH_CREATE:
            lines += [
                f'    <ClCompile Include="{ws}">',
                "      <PrecompiledHeader>Create</PrecompiledHeader>",
                "      <ForcedIncludeFiles></ForcedIncludeFiles>",
                "    </ClCompile>",
            ]
        elif path in PCH_NOT_USING_CONDITIONAL:
            cond = PCH_NOT_USING_CONDITIONAL[path]
            lines += [
                f'    <ClCompile Include="{ws}" Condition="{cond}">',
                "      <PrecompiledHeader>NotUsing</PrecompiledHeader>",
                "      <ForcedIncludeFiles></ForcedIncludeFiles>",
                "    </ClCompile>",
            ]
        elif path in PCH_NOT_USING_PLAIN:
            lines += [
                f'    <ClCompile Include="{ws}">',
                "      <PrecompiledHeader>NotUsing</PrecompiledHeader>",
                "      <ForcedIncludeFiles></ForcedIncludeFiles>",
                "    </ClCompile>",
            ]
        else:
            lines.append(f'    <ClCompile Include="{ws}" />')
    for path in rest:
        lines.append(f'    <ClCompile Include="{to_backslash(path)}" />')
    return lines


def emit_shader_sources(shaders):
    return [
        f'    <ShaderSourceFiles Include="{to_backslash(s)}" />' for s in shaders
    ]


def emit_none_tools():
    entries = []
    tools_dir = PROJECT_DIR / "tools"
    if tools_dir.is_dir():
        for f in sorted(tools_dir.iterdir()):
            if f.is_file():
                entries.append(f"tools/{f.name}")
    ctl_dir = PROJECT_DIR / "ctl"
    if ctl_dir.is_dir():
        for f in sorted(ctl_dir.glob("*.cpp")):
            name = f.name
            if "_acceptance" in name or name.startswith("test_"):
                entries.append(f"ctl/{name}")
    return [f'    <None Include="{to_backslash(p)}" />' for p in entries]


# ---------------------------------------------------------------------------
# ItemGroup replacement
# ---------------------------------------------------------------------------

def replace_compiled_group(text, header_lines, source_lines):
    """Replace every ItemGroup containing <ClInclude or <ClCompile with one
    combined group. The current file mixes the two in one block; consolidating
    here means the anchor exists exactly once afterwards."""
    pat = re.compile(
        r'[ \t]*<ItemGroup>\s*(?:<ClInclude\s|<ClCompile\s).*?</ItemGroup>',
        re.DOTALL,
    )
    matches = list(pat.finditer(text))
    if not matches:
        sys.exit("no ClInclude / ClCompile ItemGroup found")

    new_group = (
        "  <ItemGroup>\n"
        + "\n".join(header_lines) + "\n"
        + "\n".join(source_lines) + "\n"
        + "  </ItemGroup>"
    )

    for m in reversed(matches[1:]):
        text = text[:m.start()] + text[m.end():]
    m = matches[0]
    return text[:m.start()] + new_group + text[m.end():]


def replace_group_body(text, anchor, new_lines):
    pat = re.compile(r'(<ItemGroup>)(.*?)(</ItemGroup>)', re.DOTALL)
    for m in pat.finditer(text):
        if anchor in m.group(2):
            body = "\n".join(new_lines)
            return (
                text[:m.start()]
                + "<ItemGroup>\n" + body + "\n  </ItemGroup>"
                + text[m.end():]
            )
    sys.exit(f"anchor not found in .vcxproj: {anchor!r}")


# ---------------------------------------------------------------------------
# Solution
# ---------------------------------------------------------------------------

SLN_TEMPLATE = r"""Microsoft Visual Studio Solution File, Format Version 12.00
# Visual Studio Version 18
VisualStudioVersion = 18.0.0.0
MinimumVisualStudioVersion = 10.0.40219.1
Project("{8BC9CEB8-8B4A-11D0-8D11-00A0C91BC942}") = "reshade_dlssnr", "reshade_dlssnr.vcxproj", "{8F9A1C2D-4E5F-4A1B-9C2D-1234567890AB}"
EndProject
Project("{8BC9CEB8-8B4A-11D0-8D11-00A0C91BC942}") = "Detours", "third_party\Detours\Detours.vcxproj", "{a7c0f123-4b1e-4d2a-9c3e-d1e5f1b8c7a9}"
EndProject
Global
	GlobalSection(SolutionConfigurationPlatforms) = preSolution
		Debug|x64 = Debug|x64
		Release|x64 = Release|x64
	EndGlobalSection
	GlobalSection(ProjectConfigurationPlatforms) = postSolution
		{a7c0f123-4b1e-4d2a-9c3e-d1e5f1b8c7a9}.Debug|x64.ActiveCfg = Debug|x64
		{a7c0f123-4b1e-4d2a-9c3e-d1e5f1b8c7a9}.Debug|x64.Build.0 = Debug|x64
		{a7c0f123-4b1e-4d2a-9c3e-d1e5f1b8c7a9}.Release|x64.ActiveCfg = Release|x64
		{a7c0f123-4b1e-4d2a-9c3e-d1e5f1b8c7a9}.Release|x64.Build.0 = Release|x64
		{8F9A1C2D-4E5F-4A1B-9C2D-1234567890AB}.Debug|x64.ActiveCfg = Debug|x64
		{8F9A1C2D-4E5F-4A1B-9C2D-1234567890AB}.Debug|x64.Build.0 = Debug|x64
		{8F9A1C2D-4E5F-4A1B-9C2D-1234567890AB}.Release|x64.ActiveCfg = Release|x64
		{8F9A1C2D-4E5F-4A1B-9C2D-1234567890AB}.Release|x64.Build.0 = Release|x64
	EndGlobalSection
	GlobalSection(SolutionProperties) = preSolution
		HideSolutionNode = FALSE
	EndGlobalSection
EndGlobal
"""


# ---------------------------------------------------------------------------
# Compute (so --check can compare without writing)
# ---------------------------------------------------------------------------

def compute_vcxproj() -> bytes:
    if not VCXPROJ.is_file():
        sys.exit(f"not found: {VCXPROJ}")

    headers = discover(PROJECT_DIR, {".h"})
    sources = discover(PROJECT_DIR, {".cpp"})
    shaders = discover_shaders(PROJECT_DIR)
    report_unknown_dirs(headers, sources)

    raw = VCXPROJ.read_bytes()
    has_bom = raw.startswith(b"\xef\xbb\xbf")
    text = raw.decode("utf-8-sig").replace("\r\n", "\n")

    text = replace_compiled_group(
        text, emit_cl_includes(headers), emit_cl_compiles(sources)
    )
    text = replace_group_body(text, "<ShaderSourceFiles ",
                              emit_shader_sources(shaders))
    text = replace_group_body(
        text, "ShaderSourceFiles->",
        ['    <None Include="@(ShaderSourceFiles->\'shaders\\%(Identity)\')" />'],
    )
    text = replace_group_body(text, "tools\\generate_shaders.py",
                              emit_none_tools())

    return text.replace("\n", "\r\n").encode("utf-8-sig" if has_bom else "utf-8")


def compute_sln() -> bytes:
    has_bom = SLN.exists() and SLN.read_bytes().startswith(b"\xef\xbb\xbf")
    return SLN_TEMPLATE.replace("\n", "\r\n").encode("utf-8-sig" if has_bom else "utf-8")


# ---------------------------------------------------------------------------
# Main
# ---------------------------------------------------------------------------

def summarize(name, old_bytes, new_bytes):
    old = old_bytes.decode("utf-8", errors="replace").splitlines()
    new = new_bytes.decode("utf-8", errors="replace").splitlines()
    diff = list(difflib.unified_diff(old, new, lineterm=""))
    added = sum(1 for l in diff if l.startswith("+") and not l.startswith("+++"))
    removed = sum(1 for l in diff if l.startswith("-") and not l.startswith("---"))
    if added or removed:
        print(f"  {name}: +{added} -{removed}")
    return added, removed


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--check", action="store_true",
                        help="exit 1 if regeneration would change either file")
    args = parser.parse_args()

    new_vcx = compute_vcxproj()
    new_sln = compute_sln()

    if args.check:
        cur_vcx = VCXPROJ.read_bytes()
        cur_sln = SLN.read_bytes() if SLN.exists() else b""
        dirty = []
        if cur_vcx != new_vcx:
            dirty.append(VCXPROJ.name)
        if cur_sln != new_sln:
            dirty.append(SLN.name)
        if dirty:
            print(f"out of sync: {', '.join(dirty)}")
            sys.exit(1)
        print("in sync")
        return

    changed = False
    if new_vcx != VCXPROJ.read_bytes():
        cur_vcx = VCXPROJ.read_bytes()
        VCXPROJ.write_bytes(new_vcx)
        summarize(VCXPROJ.name, cur_vcx, new_vcx)
        changed = True
    if not SLN.exists() or new_sln != SLN.read_bytes():
        cur_sln = SLN.read_bytes() if SLN.exists() else b""
        SLN.write_bytes(new_sln)
        summarize(SLN.name, cur_sln, new_sln)
        changed = True

    print("regenerated" if changed else "no changes")


if __name__ == "__main__":
    main()
