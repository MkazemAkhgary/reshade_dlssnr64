#!/usr/bin/env python3
from pathlib import Path
import argparse
import re


def normalize_crlf(text: str) -> str:
    """Normalize text to Windows CRLF line endings."""
    return text.replace("\r\n", "\n").replace("\r", "\n").replace("\n", "\r\n")


def write_crlf(path: Path, text: str) -> None:
    # newline="" prevents platform-dependent translation. The generated files
    # themselves, and the shader source embedded inside raw string literals,
    # are intentionally CRLF so Visual Studio does not report mixed line endings.
    with path.open("w", encoding="utf-8", newline="") as f:
        f.write(normalize_crlf(text))


def raw_literal(text: str) -> str:
    text = normalize_crlf(text)
    delim = "DLSSNR_SHADER"
    while f")" + delim + '"' in text:
        delim += "_X"
    return f'R"{delim}({text}){delim}"'


def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument("--project-dir", required=True)
    ap.add_argument("--generated-dir", required=True)
    args = ap.parse_args()

    project = Path(args.project_dir)
    generated = Path(args.generated_dir)
    shaders = project / "shaders"
    generated.mkdir(parents=True, exist_ok=True)

    files = sorted(
        p for p in shaders.rglob("*")
        if p.is_file() and p.suffix.lower() in {".hlsl", ".hlsli", ".h"}
    )

    header = """#pragma once\n#include <cstddef>\n\nnamespace reshade_dlssnr {\nstruct EmbeddedShader { const char* name; const char* source; std::size_t size; };\nextern const EmbeddedShader kEmbeddedShaders[];\nextern const std::size_t kEmbeddedShaderCount;\nconst EmbeddedShader* FindEmbeddedShader(const char* name);\n}\n"""
    write_crlf(generated / "shader_sources.h", header)

    out = ['#include "shader_sources.h"', '#include <cstring>', 'namespace reshade_dlssnr {']
    entries = []
    for path in files:
        rel = path.relative_to(shaders).as_posix()
        safe = re.sub(r"[^A-Za-z0-9_]", "_", rel)
        symbol = "kEmbeddedShaderSource_" + safe
        out.append(f"static constexpr char {symbol}[] = {raw_literal(path.read_text(encoding='utf-8'))};")
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
    write_crlf(generated / "shader_bytecode.h", """#pragma once\n#include <cstddef>\n\nnamespace reshade_dlssnr {\nstruct EmbeddedShaderBytecode { const char* name; const unsigned char* data; std::size_t size; };\nextern const EmbeddedShaderBytecode kEmbeddedShaderBytecodes[];\nextern const std::size_t kEmbeddedShaderBytecodeCount;\nconst EmbeddedShaderBytecode* FindEmbeddedShaderBytecode(const char* name);\n}\n""")
    bytecode_stub = """#include \"shader_bytecode.h\"

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


if __name__ == "__main__":
    main()
