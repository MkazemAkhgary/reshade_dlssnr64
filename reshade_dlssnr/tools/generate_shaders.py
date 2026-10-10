#!/usr/bin/env python3
"""Embed shaders and expand nested local includes for runtime compilation."""
from pathlib import Path
import argparse
import re
import shutil

INCLUDE_RE = re.compile(r'^\s*#\s*include\s*["<]([^">]+)[">]')
PRAGMA_ONCE_RE = re.compile(r'^\s*#\s*pragma\s+once\b')
PRAGMA_ONCE_LINE_RE = re.compile(r'(?m)^[ \t]*#[ \t]*pragma[ \t]+once\b[^\r\n]*(?:\r?\n|$)')


def normalize_crlf(text: str) -> str:
    """Normalize text to Windows CRLF line endings."""
    return text.replace("\r\n", "\n").replace("\r", "\n").replace("\n", "\r\n")


def write_crlf(path: Path, text: str) -> None:
    # Keep generated C++ and embedded source line endings deterministic.
    with path.open("w", encoding="utf-8", newline="") as f:
        f.write(normalize_crlf(text))


def raw_literal(text: str) -> str:
    text = normalize_crlf(text)
    delim = "DLSSNR_SHADER"
    while f")" + delim + '"' in text:
        delim += "_X"
    return f'R"{delim}({text}){delim}"'


def has_pragma_once(lines: list[str]) -> bool:
    return any(PRAGMA_ONCE_RE.match(line) for line in lines)


def shader_name(path: Path, shader_root: Path) -> str:
    return path.relative_to(shader_root).as_posix()


def resolve_include(include_name: str, including_file: Path, shader_root: Path) -> Path:
    """Resolve sibling-relative includes first, then shader-root-relative paths."""
    normalized = include_name.replace("\\", "/")
    candidates = [including_file.parent / normalized, shader_root / normalized]
    root_resolved = shader_root.resolve()
    for candidate in candidates:
        try:
            resolved = candidate.resolve(strict=True)
            resolved.relative_to(root_resolved)
        except (OSError, ValueError):
            continue
        if resolved.is_file():
            return resolved
    raise FileNotFoundError(
        f'Cannot resolve #include "{include_name}" from '
        f'"{shader_name(including_file, shader_root)}"'
    )


def expand_shader(path: Path, shader_root: Path) -> str:
    """Inline local includes in order, honoring #pragma once."""
    expanded_once: set[Path] = set()
    stack: list[Path] = []

    def expand(current: Path, is_root: bool = False) -> str:
        current = current.resolve()
        source = current.read_text(encoding="utf-8")
        lines = source.replace("\r\n", "\n").replace("\r", "\n").splitlines()
        pragma_once = has_pragma_once(lines)

        if pragma_once and current in expanded_once:
            return ""
        if current in stack:
            if pragma_once:
                return ""
            chain = " -> ".join(shader_name(item, shader_root) for item in stack + [current])
            raise RuntimeError(f"Include cycle without #pragma once: {chain}")
        if pragma_once:
            expanded_once.add(current)

        stack.append(current)
        current_name = shader_name(current, shader_root)
        output: list[str] = []
        for index, line in enumerate(lines):
            if PRAGMA_ONCE_RE.match(line):
                continue
            match = INCLUDE_RE.match(line)
            if match is None:
                output.append(line)
                continue

            dependency = resolve_include(match.group(1), current, shader_root)
            dependency_name = shader_name(dependency, shader_root)
            expanded = expand(dependency)
            if expanded:
                output.append(f'#line 1 "{dependency_name}"')
                output.extend(expanded.rstrip("\r\n").split("\n"))
            # Resume source locations on the line after the include directive.
            output.append(f'#line {index + 2} "{current_name}"')

        stack.pop()
        # Keep one newline at EOF. The caller's generator normalizes it to CRLF.
        return "\n".join(output) + "\n"

    return expand(path, is_root=True)


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

    header = """#pragma once
#include <cstddef>

namespace reshade_dlssnr {
struct EmbeddedShader { const char* name; const char* source; std::size_t size; };
extern const EmbeddedShader kEmbeddedShaders[];
extern const std::size_t kEmbeddedShaderCount;
const EmbeddedShader* FindEmbeddedShader(const char* name);
}
"""
    write_crlf(generated / "shader_sources.h", header)

    out = ['#include "shader_sources.h"', '#include <cstring>', 'namespace reshade_dlssnr {']
    entries = []
    for path in files:
        rel = shader_name(path, shader_root)
        safe = re.sub(r"[^A-Za-z0-9_]", "_", rel)
        symbol = "kEmbeddedShaderSource_" + safe
        raw_source = path.read_text(encoding="utf-8")
        # Process include-once semantics before removing the compiler-unsupported
        # pragma from the emitted source. Root entries and FXC inputs are expanded;
        # helper entries retain their authored includes but are pragma-free.
        expanded_source = expand_shader(path, shader_root)
        is_root_shader = path.suffix.lower() == ".hlsl"
        source = expanded_source if is_root_shader else raw_source
        source = PRAGMA_ONCE_LINE_RE.sub("", source)
        if is_root_shader:
            prepared = expanded_root / rel
            prepared.parent.mkdir(parents=True, exist_ok=True)
            write_crlf(prepared, source)
        out.append(f"static constexpr char {symbol}[] = {raw_literal(source)};")
        entries.append((rel, symbol))

    out.append("const EmbeddedShader kEmbeddedShaders[] = {")
    for name, symbol in entries:
        out.append(f'  {{"{name}", {symbol}, sizeof({symbol}) - 1}},')
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
    write_crlf(generated / "shader_sources.cpp", "\n".join(out) + "\n")

    # Runtime-default stub. The opt-in precompile target replaces this file.
    write_crlf(generated / "shader_bytecode.h", """#pragma once
#include <cstddef>

namespace reshade_dlssnr {
struct EmbeddedShaderBytecode { const char* name; const unsigned char* data; std::size_t size; };
extern const EmbeddedShaderBytecode kEmbeddedShaderBytecodes[];
extern const std::size_t kEmbeddedShaderBytecodeCount;
const EmbeddedShaderBytecode* FindEmbeddedShaderBytecode(const char* name);
}
""")
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
    write_crlf(generated / "shader_bytecode.cpp", bytecode_stub)
    print(f"Embedded {len(files)} shader/header files; expanded includes for "
          f"{sum(1 for p in files if p.suffix.lower() == '.hlsl')} roots.")


if __name__ == "__main__":
    main()
