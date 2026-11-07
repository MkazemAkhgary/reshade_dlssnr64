#!/usr/bin/env python3
"""Localization authoring and build tool.

Scans the C++ sources for localized strings, checks them against the
translation files, and emits the tables the addon compiles in.

A localized string is declared in source as

    NSLOC_TEXT("pass", "intensity", "Intensity")

or, with #define LOC_NAMESPACE "pass" at the top of the file,

    LOC_TEXT("intensity", "Intensity")

The namespace and key identify the entry in every translation file; the last
argument is the English text, which is also the fallback when a language has no
translation for that key.

Translation files live in localization/translations/<code>.lang and hold one
entry per line:

    # language: de
    # name: Deutsch
    pass.intensity = Intensität

Values use C string escapes (\\n, \\t, \\", \\\\) and are copied verbatim into
the generated table, so a translated tooltip keeps its line breaks.

Commands:

    scan      list every declared string and report duplicate keys
    verify    scan, then compare against the translation files
    build     verify and write generated/localization_tables.cpp
    new CODE  scaffold localization/translations/CODE.lang from the current keys

Exit status is 1 when a duplicate key or a malformed declaration is found, so a
build step can treat that as a compile error. Missing translations are
warnings: they never fail a build.
"""

from __future__ import annotations

import argparse
import os
import re
import sys
from dataclasses import dataclass, field
from pathlib import Path

SOURCE_SUFFIXES = {".h", ".hpp", ".cpp"}
# Build output, vendored code, and editor state are not authored sources.
SKIP_DIRS = {"generated", "build", "third_party", ".vs", "tools", "translations"}

IDENTIFIER = re.compile(r"^[A-Za-z0-9_]+$")
# A key may carry dots to group related strings; the namespace may not, so the
# first dot in a translation file always separates the two.
KEY = re.compile(r"^[A-Za-z0-9_]+(?:\.[A-Za-z0-9_]+)*$")
NAMESPACE_DECL = re.compile(r'#\s*define\s+LOC_NAMESPACE\s+"([^"]*)"')
NAMESPACE_UNDEF = re.compile(r"^\s*#\s*undef\s+LOC_NAMESPACE\b", re.MULTILINE)
MACRO_CALL = re.compile(r"\b(NSLOC_TEXT|LOC_TEXT)\s*\(")
LANG_NAME = re.compile(r"^#\s*name\s*:\s*(.+)$", re.MULTILINE)
LANG_CODE = re.compile(r"^#\s*language\s*:\s*(\S+)$", re.MULTILINE)


@dataclass
class Entry:
    """One declared string."""

    ns: str
    key: str
    text: str
    path: Path
    line: int

    @property
    def ident(self) -> str:
        return f"{self.ns}.{self.key}"


@dataclass
class Problem:
    path: Path
    line: int
    message: str


@dataclass
class ScanResult:
    entries: list[Entry] = field(default_factory=list)
    problems: list[Problem] = field(default_factory=list)


def read_text(path: Path) -> str:
    return path.read_text(encoding="utf-8", errors="replace")


def iter_sources(root: Path):
    for dirpath, dirnames, filenames in os.walk(root):
        dirnames[:] = sorted(d for d in dirnames if d not in SKIP_DIRS)
        for name in sorted(filenames):
            if Path(name).suffix in SOURCE_SUFFIXES:
                yield Path(dirpath) / name


def split_arguments(text: str, start: int) -> tuple[list[str], int] | None:
    """Split the argument list that begins at `start`, just past the '('.

    Returns the raw arguments and the offset just past the closing ')', or None
    when the parentheses never balance.
    """
    args: list[str] = []
    depth = 1
    current_start = start
    i = start
    length = len(text)
    while i < length:
        ch = text[i]
        if ch == '"':
            i += 1
            while i < length and text[i] != '"':
                i += 2 if text[i] == "\\" else 1
            i += 1
            continue
        if ch == "(":
            depth += 1
        elif ch == ")":
            depth -= 1
            if depth == 0:
                args.append(text[current_start:i])
                return args, i + 1
        elif ch == "," and depth == 1:
            args.append(text[current_start:i])
            current_start = i + 1
        i += 1
    return None


def string_value(argument: str) -> str | None:
    """The raw text of a C string literal, or of adjacent literals joined.

    The inner text is kept exactly as written, escapes included: it is what a
    translation file stores and what the generator emits back into C++.
    """
    parts: list[str] = []
    i = 0
    length = len(argument)
    found = False
    while i < length:
        ch = argument[i]
        if ch in " \t\r\n":
            i += 1
            continue
        if ch != '"':
            return None
        i += 1
        begin = i
        while i < length and argument[i] != '"':
            i += 2 if argument[i] == "\\" else 1
        if i >= length:
            return None
        parts.append(argument[begin:i])
        i += 1
        found = True
    return "".join(parts) if found else None


def line_of(text: str, offset: int) -> int:
    return text.count("\n", 0, offset) + 1


def preprocessor_ranges(text: str) -> list[tuple[int, int]]:
    """Offsets covered by preprocessor directives, continuations included.

    The macro definitions name the macros they define, and a continuation line
    of a #define carries no '#' of its own, so the whole directive has to be
    skipped rather than just the line that starts it.
    """
    ranges: list[tuple[int, int]] = []
    offset = 0
    in_directive = False
    directive_start = 0
    for line in text.splitlines(keepends=True):
        stripped = line.lstrip()
        if in_directive:
            in_directive = line.rstrip().endswith("\\")
        elif stripped.startswith("#"):
            in_directive = line.rstrip().endswith("\\")
            directive_start = offset
        else:
            offset += len(line)
            continue
        if not in_directive:
            ranges.append((directive_start, offset + len(line)))
        offset += len(line)
    return ranges


def in_ranges(offset: int, ranges: list[tuple[int, int]]) -> bool:
    return any(start <= offset < end for start, end in ranges)


def scan_file(path: Path, result: ScanResult) -> None:
    text = read_text(path)
    directives = preprocessor_ranges(text)
    # A namespace belongs to one file. LOC_NAMESPACE must be undefined at the
    # end of the file, but tracking the undef here keeps a file that forgot
    # from leaking into nothing worse than its own declarations.
    undef = NAMESPACE_UNDEF.search(text)
    limit = undef.start() if undef else len(text)
    header = NAMESPACE_DECL.search(text, 0, limit)
    namespace = header.group(1) if header else None

    for match in MACRO_CALL.finditer(text):
        macro = match.group(1)
        if in_ranges(match.start(), directives):
            continue
        parsed = split_arguments(text, match.end())
        if parsed is None:
            result.problems.append(
                Problem(path, line_of(text, match.start()), f"{macro}: unbalanced arguments")
            )
            continue
        args, _ = parsed
        args = [a.strip() for a in args]
        if macro == "NSLOC_TEXT":
            if len(args) != 3:
                result.problems.append(
                    Problem(
                        path,
                        line_of(text, match.start()),
                        "NSLOC_TEXT takes a namespace, a key and the English text",
                    )
                )
                continue
            ns_arg, key_arg, text_arg = args
        else:
            if len(args) != 2:
                result.problems.append(
                    Problem(
                        path,
                        line_of(text, match.start()),
                        "LOC_TEXT takes a key and the English text",
                    )
                )
                continue
            if namespace is None:
                result.problems.append(
                    Problem(
                        path,
                        line_of(text, match.start()),
                        "LOC_TEXT used without a LOC_NAMESPACE above it",
                    )
                )
                continue
            ns_arg, key_arg, text_arg = f'"{namespace}"', args[0], args[1]

        ns = string_value(ns_arg)
        key = string_value(key_arg)
        value = string_value(text_arg)
        where = line_of(text, match.start())
        if ns is None or key is None or value is None:
            result.problems.append(
                Problem(
                    path,
                    where,
                    f"{macro}: namespace, key and text must be string literals",
                )
            )
            continue
        if not IDENTIFIER.match(ns) or not KEY.match(key):
            result.problems.append(
                Problem(
                    path,
                    where,
                    f"{macro}: the namespace is limited to letters, digits and '_', "
                    "the key to those plus '.'",
                )
            )
            continue
        result.entries.append(Entry(ns, key, value, path, where))


def scan(root: Path) -> ScanResult:
    result = ScanResult()
    for path in iter_sources(root):
        scan_file(path, result)
    result.entries.sort(key=lambda e: (e.ident, str(e.path), e.line))
    return result


def duplicates(result: ScanResult) -> list[Problem]:
    """Keys declared more than once. Two declarations of one key are two
    sources of truth for the same translation, so the build refuses them."""
    first: dict[str, Entry] = {}
    problems: list[Problem] = []
    for entry in result.entries:
        previous = first.get(entry.ident)
        if previous is None:
            first[entry.ident] = entry
            continue
        problems.append(
            Problem(
                entry.path,
                entry.line,
                f"duplicate localization key '{entry.ident}' "
                f"(first declared at {previous.path}:{previous.line})",
            )
        )
    return problems


# ---------------------------------------------------------------------------
# Translation files
# ---------------------------------------------------------------------------


@dataclass
class LanguageFile:
    code: str
    name: str
    path: Path
    values: dict[str, str] = field(default_factory=dict)


def load_language(path: Path) -> LanguageFile:
    text = read_text(path)
    code_match = LANG_CODE.search(text)
    name_match = LANG_NAME.search(text)
    language = LanguageFile(
        code=code_match.group(1) if code_match else path.stem,
        name=name_match.group(1).strip() if name_match else path.stem,
        path=path,
    )
    for raw in text.splitlines():
        line = raw.strip()
        if not line or line.startswith("#"):
            continue
        if "=" not in line:
            continue
        ident, value = line.split("=", 1)
        ident = ident.strip()
        if ident:
            language.values[ident] = value.strip()
    return language


def load_languages(directory: Path) -> list[LanguageFile]:
    if not directory.is_dir():
        return []
    return [load_language(p) for p in sorted(directory.glob("*.lang"))]


def write_language(language: LanguageFile) -> None:
    lines = [
        f"# language: {language.code}",
        f"# name: {language.name}",
        "#",
        "# Values are C string escapes: \\n is a line break, \\\" a quote.",
        "# An empty value means the English text is used.",
        "",
    ]
    current_ns = None
    for ident in sorted(language.values):
        ns = ident.split(".", 1)[0]
        if ns != current_ns:
            if current_ns is not None:
                lines.append("")
            lines.append(f"# {ns}")
            current_ns = ns
        lines.append(f"{ident} = {language.values[ident]}")
    lines.append("")
    language.path.write_text("\n".join(lines), encoding="utf-8")


# ---------------------------------------------------------------------------
# Commands
# ---------------------------------------------------------------------------


def report_problems(problems: list[Problem]) -> None:
    for problem in problems:
        print(f"{problem.path}({problem.line}): error LOC0001: {problem.message}")


def command_scan(root: Path, args: argparse.Namespace) -> int:
    result = scan(root)
    for entry in result.entries:
        print(f"{entry.ident}\t{entry.path}:{entry.line}\t{entry.text}")
    report_problems(result.problems + duplicates(result))
    print(f"\n{len(result.entries)} declared strings in {root}")
    return 1 if result.problems or duplicates(result) else 0


def compare(result: ScanResult, language: LanguageFile) -> tuple[list[str], list[str]]:
    declared = {entry.ident: entry for entry in result.entries}
    missing = [
        ident
        for ident, entry in sorted(declared.items())
        if not language.values.get(ident, "")
    ]
    unknown = sorted(ident for ident in language.values if ident not in declared)
    return missing, unknown


def command_verify(root: Path, translations: Path, args: argparse.Namespace) -> int:
    result = scan(root)
    problems = result.problems + duplicates(result)
    report_problems(problems)

    total = len({entry.ident for entry in result.entries})
    failed = bool(problems)
    for language in load_languages(translations):
        missing, unknown = compare(result, language)
        if missing:
            preview = ", ".join(missing[:8])
            more = "" if len(missing) <= 8 else f", ... ({len(missing) - 8} more)"
            print(
                f"localization: warning: {language.code} is missing "
                f"{len(missing)} of {total} translations: {preview}{more}"
            )
        if unknown:
            preview = ", ".join(unknown[:8])
            more = "" if len(unknown) <= 8 else f", ... ({len(unknown) - 8} more)"
            print(
                f"localization: warning: {language.code} has {len(unknown)} keys "
                f"no source declares: {preview}{more}"
            )
        if args.strict and (missing or unknown):
            failed = True

    print(f"localization: {total} declared strings, no duplicate keys"
          if not problems else f"localization: {len(problems)} problem(s)")
    return 1 if failed else 0


def cpp_identifier(code: str) -> str:
    return "".join(ch if ch.isalnum() else "_" for ch in code)


def command_build(root: Path, translations: Path, args: argparse.Namespace) -> int:
    result = scan(root)
    problems = result.problems + duplicates(result)
    report_problems(problems)
    if problems:
        return 1

    languages = load_languages(translations)
    by_code = {language.code: language for language in languages}
    requested = [code.strip() for code in args.languages.split(",") if code.strip()]
    # English is always embedded: its text is the fallback every declaration
    # carries, so listing it explicitly changes nothing.
    embedded: list[LanguageFile] = [
        LanguageFile(code="en", name="English", path=translations / "en.lang")
    ]
    for code in requested:
        if code == "en":
            continue
        language = by_code.get(code)
        if language is None:
            print(f"localization: error LOC0002: no translation file for '{code}'")
            return 1
        embedded.append(language)

    out = Path(args.out)
    out.parent.mkdir(parents=True, exist_ok=True)
    lines = [
        "// Generated by tools/localization.py. Do not edit.",
        "//",
        f"// Languages: {', '.join(language.code for language in embedded)}",
        f"// Strings: {len(result.entries)}",
        "",
        '#include "localization/embedded.h"',
        "",
        "namespace reshade_dlssnr {",
        "namespace localization {",
        "",
        "namespace {",
        "",
    ]
    table_names: dict[str, str] = {}
    for language in embedded:
        if language.code == "en":
            continue
        ident = cpp_identifier(language.code)
        table = f"kTranslations_{ident}"
        table_names[language.code] = table
        declared = {entry.ident for entry in result.entries}
        entries = sorted(
            (name, text)
            for name, text in language.values.items()
            if name in declared and text
        )
        if not entries:
            lines.append(f"// {language.code}: no translated strings yet.")
            lines.append("")
            continue
        lines.append(f"const Translation {table}[] = {{")
        for name, text in entries:
            ns, key = name.split(".", 1)
            lines.append(f'    {{"{ns}", "{key}", "{text}"}},')
        lines.append("};")
        lines.append("")
    lines.append("}  // namespace")
    lines.append("")
    lines.append("const EmbeddedLanguage kEmbeddedLanguages[] = {")
    for language in embedded:
        table = table_names.get(language.code)
        if table is None:
            lines.append(f'    {{"{language.code}", "{language.name}", nullptr, 0}},')
        else:
            lines.append(
                f'    {{"{language.code}", "{language.name}", {table}, '
                f"sizeof({table}) / sizeof({table}[0])}},"
            )
    lines.append("};")
    lines.append(
        "const size_t kEmbeddedLanguageCount = "
        "sizeof(kEmbeddedLanguages) / sizeof(kEmbeddedLanguages[0]);"
    )
    lines.append("")
    lines.append("}  // namespace localization")
    lines.append("}  // namespace reshade_dlssnr")
    lines.append("")

    content = "\n".join(lines)
    if not out.exists() or read_text(out) != content:
        out.write_text(content, encoding="utf-8")
        print(f"localization: wrote {out}")
    for language in embedded:
        if language.code == "en":
            continue
        missing, _ = compare(result, language)
        if missing:
            print(
                f"localization: warning: {language.code} is missing "
                f"{len(missing)} of {len(result.entries)} translations"
            )
    return 0


def command_new(root: Path, translations: Path, args: argparse.Namespace) -> int:
    result = scan(root)
    problems = result.problems + duplicates(result)
    report_problems(problems)
    if problems:
        return 1
    path = translations / f"{args.code}.lang"
    if path.exists():
        print(f"localization: {path} already exists")
        return 1
    translations.mkdir(parents=True, exist_ok=True)
    language = LanguageFile(code=args.code, name=args.name, path=path)
    for entry in result.entries:
        language.values[entry.ident] = ""
    write_language(language)
    print(f"localization: wrote {path} with {len(result.entries)} keys")
    return 0


def main(argv: list[str]) -> int:
    default_root = Path(__file__).resolve().parent.parent
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument(
        "--root",
        type=Path,
        default=default_root,
        help="project root to scan (default: the directory above tools/)",
    )
    parser.add_argument(
        "--translations",
        type=Path,
        default=None,
        help="translation directory (default: <root>/localization/translations)",
    )
    sub = parser.add_subparsers(dest="command", required=True)

    sub.add_parser("scan", help="list declared strings and report duplicate keys")

    verify = sub.add_parser("verify", help="check declarations against translations")
    verify.add_argument(
        "--strict",
        action="store_true",
        help="fail on missing or unknown translations instead of warning",
    )

    build = sub.add_parser("build", help="write the embedded translation tables")
    build.add_argument(
        "--languages",
        default="en",
        help="comma-separated language codes to embed; English is always embedded",
    )
    build.add_argument(
        "--out",
        default=None,
        help="output file (default: <root>/generated/localization_tables.cpp)",
    )

    new = sub.add_parser("new", help="scaffold a translation file")
    new.add_argument("code", help="language code, e.g. de")
    new.add_argument("--name", default=None, help="language name, e.g. Deutsch")

    args = parser.parse_args(argv)
    root = args.root.resolve()
    translations = (
        args.translations.resolve()
        if args.translations
        else root / "localization" / "translations"
    )

    if args.command == "scan":
        return command_scan(root, args)
    if args.command == "verify":
        return command_verify(root, translations, args)
    if args.command == "build":
        if args.out is None:
            args.out = str(root / "generated" / "localization_tables.cpp")
        return command_build(root, translations, args)
    if args.command == "new":
        args.name = args.name or args.code
        return command_new(root, translations, args)
    return 1


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
