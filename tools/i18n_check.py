#!/usr/bin/env python3
"""Check that every Korean UI string that goes through Tr() / TrKey() has a translation entry.

UI text in this app is Korean: `Tr("<Korean>")` (src/core/I18n.h) looks the Korean key up in the
English / Japanese / Chinese tables (src/core/I18nEn.cpp, I18nJa.cpp, I18nZh.cpp). A key missing from
a table shows Korean in that UI, so this check fails the build instead:

    (a) a key used in src/**/*.cpp|*.h with no entry in a table,
    (b) the same Korean key twice in one table,
    (c) printf conversions that differ between a key and its translation (the ordered list of
        %[flags][width][.precision][length]conversion must be equal: a mismatched format crashes
        printf), e.g. "키 %d개" -> "%d keys" is fine, "%s %d" -> "%d %s" is not,
    (d) an empty translation.

Keys are the string literals passed to `Tr(...)` and `TrKey(...)`: labels that reach Tr() through a
variable (a table of labels) are marked with TrKey(), so they are seen here too. Keys without any
Hangul character need no translation and are skipped. Entries no key uses are reported as info only
(data-driven names reach Tr() as well).

Usage:
    python -I tools/i18n_check.py [--root DIR]

The default root is the project folder (the script's parent's parent). Standard library only
(python 3). Exits 1 on an error. The CMake target `i18n_check` runs it as part of the build.
"""
import argparse
import os
import re
import sys

TABLES = ("En", "Ja", "Zh")
TABLE_FILES = {"En": "src/core/I18nEn.cpp", "Ja": "src/core/I18nJa.cpp", "Zh": "src/core/I18nZh.cpp"}
# Tables live in src/core/I18n<T>.cpp: their own entries are data, not uses.
SKIP_FILE = re.compile(r"^I18n[A-Za-z]*\.cpp$")

TR_CALL = re.compile(r"(?<![A-Za-z0-9_])(TrKey|Tr)\s*\(")
ENTRY = re.compile(r"\{\s*((?:\"(?:[^\"\\]|\\.)*\"\s*)+),\s*((?:\"(?:[^\"\\]|\\.)*\"\s*)+)\}")
STRING = re.compile(r"\"((?:[^\"\\]|\\.)*)\"")
HANGUL = re.compile("[\uac00-\ud7a3]")
PRINTF = re.compile(r"%(?:[-+ #0]*)(?:\d+|\*)?(?:\.(?:\d+|\*))?(?:hh|h|ll|l|j|z|t|L)?[diouxXeEfgGaAcspn]")


def strip_comments(text):
    """Blank out // and /* */ comments, keeping string literals (and line numbers) intact."""
    out = []
    i, n = 0, len(text)
    while i < n:
        c = text[i]
        if c == '"':
            j = i + 1
            while j < n:
                if text[j] == "\\":
                    j += 2
                    continue
                if text[j] == '"':
                    j += 1
                    break
                j += 1
            out.append(text[i:j])
            i = j
        elif c == "/" and i + 1 < n and text[i + 1] == "/":
            j = text.find("\n", i)
            if j < 0:
                break
            i = j
        elif c == "/" and i + 1 < n and text[i + 1] == "*":
            j = text.find("*/", i + 2)
            if j < 0:
                break
            out.append("\n" * text.count("\n", i, j + 2))
            i = j + 2
        else:
            out.append(c)
            i += 1
    return "".join(out)


def literals_at(text, i):
    """Concatenated string literals starting at i (whitespace allowed between), or None."""
    parts = []
    n = len(text)
    while True:
        while i < n and text[i] in " \t\r\n":
            i += 1
        if i >= n or text[i] != '"':
            break
        j = i + 1
        while j < n:
            if text[j] == "\\":
                j += 2
                continue
            if text[j] == '"':
                j += 1
                break
            j += 1
        else:
            return None
        parts.append(text[i + 1 : j - 1])
        i = j
    if not parts:
        return None
    while i < n and text[i] in " \t\r\n":
        i += 1
    if i >= n or text[i] != ")":
        return None  # not purely literals: skipped
    return "".join(parts)


def source_files(root):
    src = os.path.join(root, "src")
    for base, dirs, files in os.walk(src):
        dirs.sort()
        for name in sorted(files):
            if name.endswith((".cpp", ".h")) and not SKIP_FILE.match(name):
                yield os.path.join(base, name)


def collect_keys(root):
    """{key: "relpath:line"} for the first use of every translated key."""
    keys = {}
    for path in source_files(root):
        with open(path, encoding="utf-8", errors="replace") as f:
            text = strip_comments(f.read())
        for m in TR_CALL.finditer(text):
            key = literals_at(text, m.end())
            if key is None or not HANGUL.search(key):
                continue
            if key not in keys:
                line = text.count("\n", 0, m.start()) + 1
                keys[key] = "%s:%d" % (os.path.relpath(path, root).replace(os.sep, "/"), line)
    return keys


def collect_table(root, lang):
    path = os.path.join(root, TABLE_FILES[lang])
    with open(path, encoding="utf-8", errors="replace") as f:
        text = strip_comments(f.read())
    entries = []
    for m in ENTRY.finditer(text):
        ko = "".join(STRING.findall(m.group(1)))
        tr = "".join(STRING.findall(m.group(2)))
        line = text.count("\n", 0, m.start()) + 1
        entries.append((ko, tr, line))
    return entries


def main():
    try:
        sys.stdout.reconfigure(encoding="utf-8")
    except (AttributeError, ValueError):
        pass

    ap = argparse.ArgumentParser(description="Check Tr() / TrKey() keys against the translation tables.")
    ap.add_argument("--root", default=os.path.dirname(os.path.dirname(os.path.abspath(__file__))),
                    help="project root (default: the folder above tools/)")
    args = ap.parse_args()
    root = os.path.abspath(args.root)

    keys = collect_keys(root)
    errors = 0
    entries = {lang: collect_table(root, lang) for lang in TABLES}
    table_keys = {lang: {ko for ko, _, _ in entries[lang]} for lang in TABLES}

    for key, where in keys.items():
        missing = [lang for lang in TABLES if key not in table_keys[lang]]
        if missing:
            print('missing [%s] "%s" (%s)' % (",".join(missing), key, where))
            errors += 1

    for lang in TABLES:
        seen = set()
        for ko, tr, line in entries[lang]:
            if ko in seen:
                print('duplicate [%s] "%s"' % (lang, ko))
                errors += 1
            seen.add(ko)

    for lang in TABLES:
        for ko, tr, line in entries[lang]:
            if not tr.strip():
                print('empty [%s] "%s"' % (lang, ko))
                errors += 1
                continue
            want, got = PRINTF.findall(ko.replace("%%", "")), PRINTF.findall(tr.replace("%%", ""))
            if want != got:
                print('printf [%s] "%s" -> "%s" (%s:%d)' % (lang, ko, tr, TABLE_FILES[lang], line))
                errors += 1

    unused = [sum(1 for ko, _, _ in entries[lang] if ko not in keys) for lang in TABLES]
    print("unused: %s per table" % " / ".join(str(n) for n in unused))

    print("i18n_check: %d keys, %s entries, %d errors" % (
        len(keys), " / ".join("%s %d" % (lang, len(entries[lang])) for lang in TABLES), errors))
    return 1 if errors else 0


if __name__ == "__main__":
    sys.exit(main())
