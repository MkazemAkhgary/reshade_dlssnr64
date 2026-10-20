#!/usr/bin/env python3
"""Embed reusable shader source fragments and materialize roots for FXC tooling."""
from pathlib import Path
import argparse
import re
import shutil

INCLUDE_RE = re.compile(r'^\s*#\s*include\s*["<]([^">]+)[">]')
PRAGMA_ONCE_RE = re.compile(r'^\s*#\s*pragma\s+once\b')

UTF8_BOM = b"\xef\xbb\xbf"


def normalize_lf(text: str) -> str:
    return text.replace("\r\n", "\n").replace("\r", "\n")


def normalize_crlf(text: str) -> str:
    return normalize_lf(text).replace("\n", "\r\n")


def write_crlf(path: Path, text: str) -> None:
    """Write text unconditionally, LF normalized to CRLF.

    Reserved for expanded_shaders, which is FXC-only input and is regenerated
    from scratch on every run regardless.
    """
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open("w", encoding="utf-8", newline="") as f:
        f.write(normalize_crlf(text))


def write_crlf_if_changed(path: Path, text: str) -> bool:
    """Write text with LF normalized to CRLF, but only when the file changes.

    The four files this is used for (shader_sources.h/.cpp and
    shader_bytecode.h/.cpp) are C++ inputs; restamping them on every
    regeneration cascades into a full ClCompile of their TUs and every TU that
    includes their headers. Comparing content first means a shader edit that
    produces byte-identical generated output leaves their mtimes alone, and
    MSBuild's incremental state matches what is on disk.

    A UTF-8 BOM is stripped before comparison so a file previously written by
    a generator that emitted one still compares equal when its payload is
    unchanged. The write path never emits a BOM.

    Returns True when the file was written.
    """
    normalized = normalize_crlf(text)
    try:
        existing_bytes = path.read_bytes()
    except OSError:
        existing_bytes = None
    if existing_bytes is not None:
        if existing_bytes.startswith(UTF8_BOM):
            existing_bytes = existing_bytes[len(UTF8_BOM):]
        try:
            if existing_bytes.decode("utf-8") == normalized:
                return False
        except UnicodeDecodeError:
            pass
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open("w", encoding="utf-8", newline="") as f:
        f.write(normalized)
    return True


def raw_literal(text: str) -> str:
    text = normalize_crlf(text)
    delimiter = "DLSSNR_SHADER"
    while f")" + delimiter + '"' in text:
        delimiter += "_X"
    return f'R"{delimiter}({text}){delimiter}"'


def shader_name(path: Path, shader_root: Path) -> str:
    path = path.resolve()
    try:
        return path.relative_to(shader_root.resolve()).as_posix()
    except ValueError:
        cas_root = (shader_root.parent / "third_party" / "FidelityFX-CAS" / "ffx-cas").resolve()
        try:
            return (Path("third_party/FidelityFX-CAS/ffx-cas") /
                    path.relative_to(cas_root)).as_posix()
        except ValueError:
            return path.name


def resolve_include(include_name: str, including_file: Path, shader_root: Path) -> Path:
    """Resolve shader-local includes and the explicitly allowed FidelityFX CAS headers."""
    normalized = include_name.replace("\\", "/")
    candidates = [including_file.parent / normalized, shader_root / normalized]
    allowed_roots = [
        shader_root.resolve(),
        (shader_root.parent / "third_party" / "FidelityFX-CAS" / "ffx-cas").resolve(),
    ]
    for candidate in candidates:
        try:
            resolved = candidate.resolve(strict=True)
        except OSError:
            continue
        allowed = False
        for root in allowed_roots:
            try:
                resolved.relative_to(root)
                allowed = True
                break
            except ValueError:
                continue
        if allowed and resolved.is_file():
            return resolved
    raise FileNotFoundError(
        f'Cannot resolve #include "{include_name}" from '
        f'"{shader_name(including_file, shader_root)}". '
        "External shader includes are allowed only from third_party/FidelityFX-CAS/ffx-cas."
    )


def assemble_fragments(root_path: Path, shader_root: Path) -> list[str]:
    """Return ordered source chunks with include/pragma directives removed.

    Include recursion is resolved into a fragment recipe, not expanded into a
    duplicated source body. #pragma once is honored while creating each root's
    recipe. Chunks are split at includes so declarations/macros surrounding an
    include retain the same relative order as in the original source.
    """
    fragments: list[str] = []
    expanded_once: set[Path] = set()
    stack: list[Path] = []

    def flush_chunk(lines: list[str]) -> None:
        if not lines:
            return
        chunk = "".join(lines)
        if not chunk.strip():
            lines.clear()
            return
        if not chunk.endswith("\n"):
            chunk += "\n"
        fragments.append(chunk)
        lines.clear()

    def visit(current: Path) -> None:
        current = current.resolve()
        source = normalize_lf(current.read_text(encoding="utf-8"))
        lines = source.splitlines(keepends=True)
        has_pragma_once = any(PRAGMA_ONCE_RE.match(line) for line in lines)

        if has_pragma_once and current in expanded_once:
            return
        if current in stack:
            if has_pragma_once:
                return
            chain = " -> ".join(shader_name(item, shader_root) for item in stack + [current])
            raise RuntimeError(f"Include cycle without #pragma once: {chain}")
        if has_pragma_once:
            expanded_once.add(current)

        stack.append(current)
        chunk: list[str] = []
        for line in lines:
            if PRAGMA_ONCE_RE.match(line):
                continue
            match = INCLUDE_RE.match(line)
            if match is None:
                chunk.append(line)
                continue

            flush_chunk(chunk)
            dependency = resolve_include(match.group(1), current, shader_root)
            visit(dependency)

        flush_chunk(chunk)
        stack.pop()

    visit(root_path)
    return fragments


def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument("--project-dir", required=True)
    ap.add_argument("--generated-dir", required=True)
    args = ap.parse_args()

    project = Path(args.project_dir).resolve()
    generated = Path(args.generated_dir).resolve()
    shader_root = project / "shaders"
    generated.mkdir(parents=True, exist_ok=True)
    expanded_root = generated / "expanded_shaders"
    if expanded_root.exists():
        shutil.rmtree(expanded_root)
    expanded_root.mkdir(parents=True, exist_ok=True)

    files = sorted(
        (p for p in shader_root.rglob("*")
         if p.is_file() and p.suffix.lower() in {".hlsl", ".hlsli", ".h"}),
        key=lambda p: p.as_posix().lower(),
    )
    roots = [p for p in files if p.suffix.lower() == ".hlsl"]

    written = 0
    skipped = 0

    header = """#pragma once
#include <cstddef>

namespace reshade_dlssnr {
struct EmbeddedShaderFragment { const char* source; std::size_t size; };
struct EmbeddedShader {
  const char* name;
  const EmbeddedShaderFragment* const* fragments;
  std::size_t fragment_count;
};
extern const EmbeddedShader kEmbeddedShaders[];
extern const std::size_t kEmbeddedShaderCount;
const EmbeddedShader* FindEmbeddedShader(const char* name);
}
"""
    if write_crlf_if_changed(generated / "shader_sources.h", header):
        written += 1
    else:
        skipped += 1

    fragment_ids: dict[str, int] = {}
    fragment_texts: list[str] = []
    root_entries: list[tuple[str, str, list[int]]] = []

    for path in roots:
        rel = shader_name(path, shader_root)
        pieces = assemble_fragments(path, shader_root)
        if not pieces:
            raise RuntimeError(f"No compilable source fragments for root shader: {rel}")

        recipe: list[int] = []
        for piece in pieces:
            index = fragment_ids.get(piece)
            if index is None:
                index = len(fragment_texts)
                fragment_ids[piece] = index
                fragment_texts.append(piece)
            recipe.append(index)

        safe = re.sub(r"[^A-Za-z0-9_]", "_", rel)
        root_entries.append((rel, safe, recipe))

        # Expanded root sources are FXC-only input for verify/precompile, so
        # they are rewritten unconditionally along with the surrounding
        # directory. Nothing in the C++ build reads them.
        prepared = expanded_root / rel
        write_crlf(prepared, "".join(pieces))

    out = [
        '#include "shader_sources.h"',
        '#include <cstring>',
        'namespace reshade_dlssnr {',
    ]
    for index, source in enumerate(fragment_texts):
        text_symbol = f"kEmbeddedShaderText_{index:03d}"
        fragment_symbol = f"kEmbeddedShaderFragment_{index:03d}"
        out.append(f"static constexpr char {text_symbol}[] = {raw_literal(source)};")
        out.append(
            f"static constexpr EmbeddedShaderFragment {fragment_symbol} = "
            f"{{{text_symbol}, sizeof({text_symbol}) - 1}};"
        )

    for rel, safe, recipe in root_entries:
        parts_symbol = f"kEmbeddedShaderParts_{safe}"
        out.append(f"static constexpr const EmbeddedShaderFragment* {parts_symbol}[] = {{")
        for index in recipe:
            out.append(f"  &kEmbeddedShaderFragment_{index:03d},")
        out.append("};")

    out.append("const EmbeddedShader kEmbeddedShaders[] = {")
    for rel, safe, recipe in root_entries:
        parts_symbol = f"kEmbeddedShaderParts_{safe}"
        out.append(
            f'  {{"{rel}", {parts_symbol}, '
            f"sizeof({parts_symbol}) / sizeof({parts_symbol}[0])}},"
        )
    out += [
        "};",
        "const std::size_t kEmbeddedShaderCount = sizeof(kEmbeddedShaders) / sizeof(kEmbeddedShaders[0]);",
        "const EmbeddedShader* FindEmbeddedShader(const char* name) {",
        "  if (name == nullptr) return nullptr;",
        "  for (std::size_t i = 0; i < kEmbeddedShaderCount; ++i)",
        "    if (std::strcmp(kEmbeddedShaders[i].name, name) == 0) return &kEmbeddedShaders[i];",
        "  return nullptr;",
        "}",
        "}",
    ]
    # Line separator and trailing newline must match the PowerShell generator's
    # WriteAllLines output byte-for-byte, so a project that regenerates on one
    # side and compares on the other does not see spurious diffs.
    if write_crlf_if_changed(
        generated / "shader_sources.cpp", "\n".join(out) + "\n"
    ):
        written += 1
    else:
        skipped += 1

    # Runtime-default stub. The opt-in precompile target replaces this file.
    bytecode_header = """#pragma once
#include <cstddef>

namespace reshade_dlssnr {
struct EmbeddedShaderBytecode { const char* name; const unsigned char* data; std::size_t size; };
extern const EmbeddedShaderBytecode kEmbeddedShaderBytecodes[];
extern const std::size_t kEmbeddedShaderBytecodeCount;
const EmbeddedShaderBytecode* FindEmbeddedShaderBytecode(const char* name);
}
"""
    if write_crlf_if_changed(generated / "shader_bytecode.h", bytecode_header):
        written += 1
    else:
        skipped += 1

    bytecode_stub = """#include "shader_bytecode.h"

namespace reshade_dlssnr {
static const unsigned char kNoEmbeddedShaderBytecode = 0;
const EmbeddedShaderBytecode kEmbeddedShaderBytecodes[] = {
  {nullptr, &kNoEmbeddedShaderBytecode, 0},
};
const std::size_t kEmbeddedShaderBytecodeCount = 0;
const EmbeddedShaderBytecode* FindEmbeddedShaderBytecode(const char*) { return nullptr; }
}
"""
    if write_crlf_if_changed(generated / "shader_bytecode.cpp", bytecode_stub):
        written += 1
    else:
        skipped += 1

    print(
        f"Embedded {len(fragment_texts)} unique source fragments from "
        f"{len(files)} shader/header files; assembled {len(roots)} shader "
        f"roots for verification/precompilation. Wrote {written}, skipped "
        f"{skipped} unchanged."
    )


if __name__ == "__main__":
    main()
