#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""mme_scan.py - static feature scanner + compile test for MikuMikuEffect (MME) effect files.

Research tool: it never modifies the scanned files and is not part of the app build. It walks a
folder tree of MME effects and reports, per ``*.fx`` effect file, which MME features the effect uses
and whether its shaders compile

  (a) as a D3D9 effect  (``fxc /T fx_2_0``) and
  (b) per pass as Shader Model 5 (``fxc /T vs_5_0|ps_5_0 /Gec``).

This measures how far a "translate MME HLSL to SM5" runtime would get.

Parsing is regex + brace matching on comment-stripped text (no full HLSL parser). Strings may contain
``;`` and ``}``; quotes are respected when matching braces and splitting.

Usage:
  python tools/mme/mme_scan.py <root> [<root>...] --out <dir> [--fxc <path>] [--no-compile]
                              [--jobs N] [--limit N]
"""

import argparse
import codecs
import concurrent.futures as futures
import csv
import hashlib
import json
import os
import re
import shutil
import subprocess
import sys
import threading
import time
from collections import Counter
from pathlib import Path

# ---------------------------------------------------------------------------------------------
# Constants
# ---------------------------------------------------------------------------------------------

EFFECT_EXT = ".fx"
INCLUDE_EXTS = {".fxsub", ".fxh", ".hlsl", ".h"}
MAX_INCLUDE_DEPTH = 8
FXC_TIMEOUT_SEC = 60

STAGE_TARGET = {"vs": "vs_5_0", "ps": "ps_5_0"}

# Semantics we recognise (upper-cased). Anything else is reported as "unknown".
KNOWN_SEMANTICS = {
    "STANDARDSGLOBAL",
    "WORLD", "WORLDINVERSE", "WORLDTRANSPOSE", "WORLDINVERSETRANSPOSE",
    "VIEW", "VIEWINVERSE", "VIEWTRANSPOSE", "VIEWINVERSETRANSPOSE",
    "PROJECTION", "PROJECTIONINVERSE", "PROJECTIONTRANSPOSE", "PROJECTIONINVERSETRANSPOSE",
    "WORLDVIEW", "WORLDVIEWINVERSE", "WORLDVIEWTRANSPOSE", "WORLDVIEWINVERSETRANSPOSE",
    "VIEWPROJECTION", "VIEWPROJECTIONINVERSE", "VIEWPROJECTIONTRANSPOSE", "VIEWPROJECTIONINVERSETRANSPOSE",
    "WORLDVIEWPROJECTION", "WORLDVIEWPROJECTIONINVERSE",
    "WORLDVIEWPROJECTIONTRANSPOSE", "WORLDVIEWPROJECTIONINVERSETRANSPOSE",
    "DIRECTION", "POSITION", "CAMERAPOSITION",
    "DIFFUSE", "AMBIENT", "SPECULAR", "EMISSIVE", "SPECULARPOWER", "TOONCOLOR",
    "LIGHTDIFFUSE", "LIGHTAMBIENT", "LIGHTSPECULAR", "LIGHTDIRECTION",
    "MATERIALDIFFUSE", "MATERIALAMBIENT", "MATERIALSPECULAR", "MATERIALEMISSIVE",
    "TIME",
    "CONTROLOBJECT",
    "OFFSCREENRENDERTARGET", "RENDERCOLORTARGET", "RENDERDEPTHSTENCILTARGET",
    "MATERIALTEXTURE", "ANIMATEDTEXTURE", "ANIMATIONTEXTURE",
    # further standard MMD/mesh semantics
    "EDGECOLOR", "GROUNDSHADOWCOLOR", "GROUNDSHADOWCOLOR2",
    "MATERIALSPHEREMAP", "MATERIALTOONTEXTURE",
    "ADDINGTEXTURE", "MULTIPLYINGTEXTURE", "ADDINGTEXTUREALPHA", "MULTIPLYINGTEXTUREALPHA",
    "ADDINGSPHERETEXTURE", "MULTIPLYINGSPHERETEXTURE",
    "ADDINGSPHERETEXTUREALPHA", "MULTIPLYINGSPHERETEXTUREALPHA",
    "VIEWPORT", "VIEWPORTPIXELSIZE",
    "LIGHTVIEWPROJECTION", "LIGHTWORLDVIEWPROJECTION",
    "SELFCOLOR", "SELFCOLOR2", "SELFDEPTH", "SELFDEPTH2",
    "PARENTCOLOR", "CHILDRENCOLOR",
}

# Annotation names we recognise (lower-cased). Anything else is reported as "unknown".
KNOWN_ANNOTATIONS = {
    "object", "name", "item", "resourcename", "format",
    "viewportratio", "width", "height", "miplevels", "levels",
    "defaulteffect", "description", "clearcolor", "cleardepth", "antialias",
    "syncineditmode", "uiname", "uiwidget", "uimin", "uimax",
    "scriptclass", "scriptorder", "scriptoutput", "script",
    "mmdpass", "usetexture", "usespheremap", "usetoon", "useselfshadow", "subset",
    "offset", "speed", "output",
}

# By-name uniforms MME fills without a semantic.
BY_NAME_UNIFORMS = [
    "parthf", "transp", "spadd", "use_texture", "use_spheremap", "use_toon",
    "opadd", "use_subtexture",
]

# Shader-language intrinsics we look for.
INTRINSICS = [
    "tex2D", "tex2Dlod", "tex2Dbias", "tex2Dproj", "tex2Dgrad",
    "texCUBE", "tex3D", "tex2D",
]

VERTEX_ATTR_SEMANTICS = [
    "POSITION", "NORMAL", "TEXCOORD0", "TEXCOORD1", "TEXCOORD2", "TEXCOORD3",
    "TEXCOORD4", "TEXCOORD5", "TEXCOORD6", "TEXCOORD7",
    "COLOR", "BLENDWEIGHT", "BLENDINDICES", "_INDEX", "PSIZE",
]

IDENT = r"[A-Za-z_]\w*"
DECL_RE = re.compile(r"(?<![A-Za-z0-9_.])(" + IDENT + r")[ \t]+(" + IDENT + r")[ \t]*:[ \t]*(" + IDENT + r")")
TEX_RE = re.compile(r"(?<![A-Za-z0-9_])(texture(?:2D|3D|CUBE)?)[ \t]+(" + IDENT + r")", re.IGNORECASE)
SAMPLER_RE = re.compile(r"(?<![A-Za-z0-9_])(sampler(?:2D|3D|CUBE|STATE|_state)?)[ \t]+(" + IDENT + r")",
                        re.IGNORECASE)
TECH_RE = re.compile(r"(?<![A-Za-z0-9_])technique\b")
PASS_RE = re.compile(r"(?<![A-Za-z0-9_])pass\b")
COMPILE_RE = re.compile(
    r"(?P<stage>VertexShader|PixelShader)[ \t]*=[ \t]*compile[ \t]+(?P<profile>" + IDENT +
    r")[ \t]+(?P<fn>" + IDENT + r")[ \t]*\((?P<args>[^)]*)\)")
NULL_SHADER_RE = re.compile(r"(?P<stage>VertexShader|PixelShader)[ \t]*=[ \t]*NULL", re.IGNORECASE)
INCLUDE_RE = re.compile(r"^[ \t]*#[ \t]*include[ \t]*[<\"]([^>\"]+)[>\"].*$", re.MULTILINE)
DEFINE_RE = re.compile(r"^[ \t]*#[ \t]*define[ \t]+(" + IDENT + r")(.*)$", re.MULTILINE)
COND_RE = re.compile(r"^[ \t]*#[ \t]*(if|ifdef|ifndef|elif)[ \t]*(.*)$", re.MULTILINE)


# ---------------------------------------------------------------------------------------------
# Encoding / IO helpers
# ---------------------------------------------------------------------------------------------

def decode_bytes(raw):
    """Return (text, encoding_guess). Strips a UTF-8 BOM."""
    if raw.startswith(codecs.BOM_UTF8):
        try:
            return raw[len(codecs.BOM_UTF8):].decode("utf-8"), "utf-8"
        except UnicodeDecodeError:
            pass
    try:
        return raw.decode("utf-8"), "utf-8"
    except UnicodeDecodeError:
        pass
    try:
        return raw.decode("cp932"), "cp932"
    except (UnicodeDecodeError, LookupError):
        pass
    return raw.decode("latin-1"), "latin-1"


def encode_text(text, encoding):
    try:
        return text.encode(encoding, "replace")
    except LookupError:
        return text.encode("latin-1", "replace")


# ---------------------------------------------------------------------------------------------
# Comment stripping / brace matching / splitting
# ---------------------------------------------------------------------------------------------

def strip_comments(text):
    """Remove // and /* */ comments, respecting string literals."""
    out = []
    i = 0
    n = len(text)
    while i < n:
        c = text[i]
        if c == "/" and i + 1 < n and text[i + 1] == "/":
            while i < n and text[i] != "\n":
                i += 1
            continue
        if c == "/" and i + 1 < n and text[i + 1] == "*":
            i += 2
            while i + 1 < n and not (text[i] == "*" and text[i + 1] == "/"):
                i += 1
            i = min(n, i + 2)
            continue
        if c == '"':
            out.append(c)
            i += 1
            while i < n and text[i] != '"':
                if text[i] == "\\" and i + 1 < n:
                    out.append(text[i])
                    i += 1
                out.append(text[i])
                i += 1
            if i < n:
                out.append(text[i])
                i += 1
            continue
        out.append(c)
        i += 1
    return "".join(out)


def match_pair(text, start, open_ch, close_ch, depth=0):
    """Return index of the close_ch matching open_ch at ``start`` (quote aware), or -1."""
    n = len(text)
    i = start
    if i >= n or text[i] != open_ch:
        return -1
    depth = 1
    i += 1
    while i < n:
        c = text[i]
        if c == '"':
            i += 1
            while i < n and text[i] != '"':
                if text[i] == "\\":
                    i += 1
                i += 1
            i += 1
            continue
        if c == open_ch:
            depth += 1
        elif c == close_ch:
            depth -= 1
            if depth == 0:
                return i
        i += 1
    return -1


def split_top_level(text, sep=";"):
    """Split on ``sep`` at brace/paren depth 0, respecting strings."""
    parts = []
    depth = 0
    cur = []
    i = 0
    n = len(text)
    while i < n:
        c = text[i]
        if c == '"':
            cur.append(c)
            i += 1
            while i < n and text[i] != '"':
                if text[i] == "\\" and i + 1 < n:
                    cur.append(text[i])
                    i += 1
                cur.append(text[i])
                i += 1
            if i < n:
                cur.append(text[i])
                i += 1
            continue
        if c in "{(":
            depth += 1
        elif c in "})":
            if depth > 0:
                depth -= 1
        if c == sep and depth == 0:
            parts.append("".join(cur))
            cur = []
            i += 1
            continue
        cur.append(c)
        i += 1
    parts.append("".join(cur))
    return parts


def brace_depths(text):
    """Per-character brace depth, respecting strings (annotation {} included)."""
    depths = [0] * (len(text) + 1)
    depth = 0
    i = 0
    n = len(text)
    while i < n:
        c = text[i]
        if c == '"':
            depths[i] = depth
            i += 1
            while i < n and text[i] != '"':
                depths[i] = depth
                if text[i] == "\\":
                    i += 1
                    if i < n:
                        depths[i] = depth
                i += 1
            if i < n:
                depths[i] = depth
                i += 1
            continue
        if c == "{":
            depths[i] = depth
            depth += 1
            i += 1
            continue
        if c == "}":
            depth = max(0, depth - 1)
            depths[i] = depth
            i += 1
            continue
        depths[i] = depth
        i += 1
    depths[n] = depth
    return depths


# ---------------------------------------------------------------------------------------------
# Annotation parsing
# ---------------------------------------------------------------------------------------------

def unquote(s):
    s = s.strip()
    if len(s) >= 2 and s[0] == '"' and s[-1] == '"':
        inner = s[1:-1]
        return inner.replace('\\"', '"').replace("\\\\", "\\")
    return s


def parse_annotations(body):
    """Parse ``name = value; ...`` annotation text into an ordered dict name -> raw value string."""
    annotations = {}
    for part in split_top_level(body, ";"):
        if "=" not in part:
            continue
        name, _, value = part.partition("=")
        name = name.strip()
        # annotations are `type name = value`; keep the name (last identifier token)
        toks = name.split()
        if not toks:
            continue
        name = toks[-1]
        if not re.match(r"^[A-Za-z_]\w*$", name):
            continue
        annotations[name] = value.strip()
    return annotations


def read_annotation_block(text, pos):
    """If ``text[pos]`` starts a ``< ... >`` annotation block, return (body, newpos); else (None, pos)."""
    i = pos
    while i < len(text) and text[i] in " \t\r\n":
        i += 1
    if i < len(text) and text[i] == "<":
        close = match_pair(text, i, "<", ">")
        if close != -1:
            return text[i + 1:close], close + 1
    return None, pos


# ---------------------------------------------------------------------------------------------
# Include resolution (case-insensitive, mirroring MmeHints.cpp ResolveRel)
# ---------------------------------------------------------------------------------------------

def resolve_rel(base, rel):
    """Resolve ``rel`` against directory ``base`` case-insensitively. Returns Path or None."""
    rel = rel.replace("\\", "/")
    if not rel:
        return None
    if len(rel) >= 2 and rel[1] == ":":
        p = Path(rel)
        return p if p.exists() else None
    cur = Path(base)
    for comp in rel.split("/"):
        if comp in ("", "."):
            continue
        if comp == "..":
            cur = cur.parent
            continue
        cand = cur / comp
        if cand.exists():
            cur = cand
            continue
        found = None
        try:
            for entry in os.scandir(cur):
                if entry.name.lower() == comp.lower():
                    found = Path(entry.path)
                    break
        except OSError:
            return None
        if found is None:
            return None
        cur = found
    return cur if cur.exists() and cur.is_file() else None


# ---------------------------------------------------------------------------------------------
# Feature extraction
# ---------------------------------------------------------------------------------------------

def build_define_table(text):
    """Object-like #define table name -> value text (mirrors MmeHints' scatter #define table)."""
    defs = {}
    for m in DEFINE_RE.finditer(text):
        name = m.group(1)
        value = m.group(2).strip()
        if value:
            defs[name] = value
    return defs


def resolve_expr(expr, defs, stack=None, depth=0):
    """Resolve an MME expression to text: string literals and identifier defines collapse; mirroring
    MmeHints.cpp ResolveExpr. Macro cycles are broken by a visited stack."""
    if stack is None:
        stack = set()
    if depth > 16:
        return ""
    out = []
    i = 0
    n = len(expr)
    while i < n:
        c = expr[i]
        if c.isspace():
            i += 1
            continue
        if c == '"':
            i += 1
            while i < n and expr[i] != '"':
                if expr[i] == "\\" and i + 1 < n:
                    out.append(expr[i + 1])
                    i += 2
                    continue
                out.append(expr[i])
                i += 1
            i += 1
            continue
        if c.isalpha() or c == "_":
            s = i
            while i < n and (expr[i].isalnum() or expr[i] == "_"):
                i += 1
            ident = expr[s:i]
            low = ident.lower()
            if low in ("merge_strings", "concat", "string"):
                continue
            if ident in defs and ident not in stack:
                stack.add(ident)
                out.append(resolve_expr(defs[ident], defs, stack, depth + 1))
                stack.discard(ident)
            continue
        i += 1
    return "".join(out)


def extract_features(text):
    """Extract MME features from comment-stripped, include-inlined ``text``."""
    feats = {}
    depths = brace_depths(text)
    defs = build_define_table(text)

    # ---- defines and #if/#ifdef symbols -------------------------------------------------
    feats["defines"] = sorted(set(m.group(1) for m in DEFINE_RE.finditer(text)))
    cond_symbols = set()
    for m in COND_RE.finditer(text):
        cond_symbols.add(m.group(2).strip())
    feats["condition_symbols"] = sorted(s for s in cond_symbols if s)

    # ---- global effect class (STANDARDSGLOBAL) -------------------------------------------
    feats["global_class"] = None
    for m in DECL_RE.finditer(text):
        if m.group(3).upper() != "STANDARDSGLOBAL":
            continue
        ann, _ = read_annotation_block(text, m.end())
        if ann is None:
            continue
        a = parse_annotations(ann)
        script_class = resolve_expr(a.get("ScriptClass", ""), defs)
        script_order = resolve_expr(a.get("ScriptOrder", ""), defs)
        script_output = resolve_expr(a.get("ScriptOutput", ""), defs)
        script = resolve_expr(a.get("Script", ""), defs) if "Script" in a else ""
        lower = script_class.lower()
        if lower == "scene":
            kind = "post"
        elif lower == "object":
            kind = "object"
        elif lower == "sceneorobject":
            kind = "mixed"
        else:
            kind = lower or None
        feats["global_class"] = {
            "ScriptClass": script_class,
            "ScriptOrder": script_order,
            "ScriptOutput": script_output,
            "Script": script,
            "kind": kind,
            "annotations": a,
        }
        break

    # ---- techniques and passes -----------------------------------------------------------
    feats["techniques"] = []
    tech_count = 0
    for m in TECH_RE.finditer(text):
        pos = m.end()
        while pos < len(text) and text[pos] in " \t\r\n":
            pos += 1
        name = None
        nm = re.match(IDENT, text[pos:])
        if nm:
            name = nm.group(0)
            pos += len(name)
        ann, pos2 = read_annotation_block(text, pos)
        annotations = parse_annotations(ann) if ann else {}
        pos = pos2
        while pos < len(text) and text[pos] in " \t\r\n":
            pos += 1
        if pos >= len(text) or text[pos] != "{":
            continue
        close = match_pair(text, pos, "{", "}")
        if close == -1:
            continue
        body = text[pos + 1:close]
        tech_count += 1
        technique = {
            "name": name,
            "annotations": annotations,
            "MMDPass": resolve_expr(annotations.get("MMDPass", ""), defs) if "MMDPass" in annotations else "",
            "Script": resolve_expr(annotations.get("Script", ""), defs) if "Script" in annotations else "",
            "passes": [],
        }
        for pm in PASS_RE.finditer(body):
            ppos = pm.end()
            while ppos < len(body) and body[ppos] in " \t\r\n":
                ppos += 1
            pname = None
            pnm = re.match(IDENT, body[ppos:])
            if pnm:
                pname = pnm.group(0)
                ppos += len(pname)
            pann, ppos2 = read_annotation_block(body, ppos)
            pass_ann = parse_annotations(pann) if pann else {}
            ppos = ppos2
            while ppos < len(body) and body[ppos] in " \t\r\n":
                ppos += 1
            if ppos >= len(body) or body[ppos] != "{":
                continue
            pclose = match_pair(body, ppos, "{", "}")
            if pclose == -1:
                continue
            pbody = body[ppos + 1:pclose]
            pass_rec = {
                "name": pname,
                "annotations": pass_ann,
                "Script": resolve_expr(pass_ann.get("Script", ""), defs) if "Script" in pass_ann else "",
                "states": [],
                "scripts": [],
                "asm": bool(re.search(r"(?<![A-Za-z0-9_])asm[ \t]*\{", pbody)),
                "shaders": {},
            }
            for stmt in split_top_level(pbody, ";"):
                s = stmt.strip()
                if not s:
                    continue
                if re.search(r"VertexShader|PixelShader", s):
                    continue
                if "=" in s:
                    state = s.split("=", 1)[0].strip()
                    if re.match(r"^[A-Za-z_]\w*$", state):
                        pass_rec["states"].append({"name": state, "value": s.split("=", 1)[1].strip()})
            for cm in COMPILE_RE.finditer(pbody):
                stage = "vs" if cm.group("stage") == "VertexShader" else "ps"
                pass_rec["shaders"][stage] = {
                    "profile": cm.group("profile"),
                    "fn": cm.group("fn"),
                    "args": [a.strip() for a in split_top_level(cm.group("args"), ",") if a.strip()],
                    "null": False,
                }
            if "VertexShader" in pbody and NULL_SHADER_RE.search(pbody):
                for nm2 in NULL_SHADER_RE.finditer(pbody):
                    stage = "vs" if nm2.group("stage") == "VertexShader" else "ps"
                    pass_rec["shaders"].setdefault(stage, {"null": True})
                    pass_rec["shaders"][stage]["null"] = True
            technique["passes"].append(pass_rec)
        feats["techniques"].append(technique)
    feats["technique_count"] = tech_count

    # ---- script commands (from Script strings anywhere) ----------------------------------
    script_commands = Counter()
    for tech in feats["techniques"]:
        for src in [tech["Script"]] + [p["Script"] for p in tech["passes"]]:
            for cmd in split_top_level(src, ";"):
                cmd = cmd.strip()
                if not cmd:
                    continue
                name = cmd.split("=", 1)[0].strip()
                if name:
                    script_commands[name] += 1
    feats["script_commands"] = dict(script_commands)

    # ---- parameters with semantics -------------------------------------------------------
    parameters = []
    controlobject = []
    default_effects = []
    by_name = []
    for m in DECL_RE.finditer(text):
        if depths[m.start()] != 0:
            continue
        typ, name, semantic = m.group(1), m.group(2), m.group(3).upper()
        if typ.lower().startswith(("texture", "sampler")):
            continue  # texture / sampler declarations are reported separately
        pos = m.end()
        ann, pos = read_annotation_block(text, pos)
        a = parse_annotations(ann) if ann else {}
        # skip function parameters (terminator ',', ')' or '{') and locals
        j = pos
        while j < len(text) and text[j] in " \t\r\n":
            j += 1
        init = ""
        if j < len(text) and text[j] == "=":
            parts = split_top_level(text[j + 1:], ";")
            init = parts[0].strip()
            j = j + 1 + len(parts[0])
        while j < len(text) and text[j] in " \t\r\n":
            j += 1
        if j >= len(text) or text[j] != ";":
            continue
        rec = {
            "semantic": semantic,
            "type": typ,
            "name": name,
            "annotations": a,
            "init": init,
        }
        parameters.append(rec)
        if semantic == "CONTROLOBJECT":
            controlobject.append({
                "name": resolve_expr(a.get("name", ""), defs) if "name" in a else "",
                "item": resolve_expr(a.get("item", ""), defs) if "item" in a else "",
            })
        if semantic == "OFFSCREENRENDERTARGET":
            if "DefaultEffect" in a:
                default_effects.append(resolve_expr(a["DefaultEffect"], defs))
    feats["parameters"] = parameters
    feats["controlobject"] = controlobject
    feats["default_effects"] = default_effects

    # by-name uniforms without semantic
    for bnm in BY_NAME_UNIFORMS:
        if re.search(r"(?<![A-Za-z0-9_])" + re.escape(bnm) + r"(?![A-Za-z0-9_])", text):
            name_decl = re.search(
                r"(?<![A-Za-z0-9_])" + IDENT + r"[ \t]+" + re.escape(bnm) + r"[ \t]*(?:=|;)",
                text)
            if name_decl:
                by_name.append(bnm)
    feats["uniforms_by_name"] = sorted(set(by_name))

    # ---- textures ------------------------------------------------------------------------
    textures = []
    for m in TEX_RE.finditer(text):
        if depths[m.start()] != 0:
            continue
        typ = m.group(1)
        name = m.group(2)
        # optional `: SEMANTIC` then optional `< ... >` annotation block
        j = m.end()
        while j < len(text) and text[j] in " \t\r\n":
            j += 1
        semantic = ""
        if text[j:j + 1] == ":":
            j += 1
            while j < len(text) and text[j] in " \t\r\n":
                j += 1
            sm = re.match(IDENT, text[j:])
            if sm:
                semantic = sm.group(0).upper()
                j += len(sm.group(0))
        ann, _ = read_annotation_block(text, j)
        a = parse_annotations(ann) if ann else {}
        resname = resolve_expr(a.get("ResourceName", ""), defs) if "ResourceName" in a else ""
        if semantic == "OFFSCREENRENDERTARGET" and "DefaultEffect" in a:
            default_effects.append(resolve_expr(a["DefaultEffect"], defs))
        rec = {
            "type": typ,
            "name": name,
            "semantic": semantic,
            "annotations": a,
            "ResourceName": resname,
            "Format": resolve_expr(unquote(a.get("Format", "")), defs) if "Format" in a else "",
            "resolved": None,
            "missing": None,
        }
        textures.append(rec)
    feats["textures"] = textures

    # ---- samplers ------------------------------------------------------------------------
    samplers = []
    for m in SAMPLER_RE.finditer(text):
        if depths[m.start()] != 0:
            continue
        typ = m.group(1)
        name = m.group(2)
        pos = m.end()
        j = pos
        while j < len(text) and text[j] in " \t\r\n":
            j += 1
        register = None
        state_body = None
        if text[j:j + 1] == ":":
            rm = re.match(r":\s*register\s*\(\s*(\w+)\s*\)", text[j:])
            if rm:
                register = rm.group(1)
                j = j + rm.end()
        while j < len(text) and text[j] in " \t\r\n":
            j += 1
        if text[j:j + 1] == "=":
            j += 1
            while j < len(text) and text[j] in " \t\r\n":
                j += 1
            ob = text.find("{", j)
            if ob != -1:
                cb = match_pair(text, ob, "{", "}")
                if cb != -1:
                    state_body = text[ob + 1:cb]
        states = {}
        texture_binding = None
        if state_body:
            for part in split_top_level(state_body, ";"):
                if "=" not in part:
                    continue
                k, _, v = part.partition("=")
                k = k.strip()
                v = v.strip()
                if k.lower() == "texture":
                    mb = re.search(r"<([^>]*)>", v)
                    texture_binding = (mb.group(1).strip() if mb else v.strip())
                if k:
                    states[k] = v
        samplers.append({
            "type": typ,
            "name": name,
            "register": register,
            "texture": texture_binding,
            "states": states,
            "has_state_block": state_body is not None,
        })
    feats["samplers"] = samplers

    # ---- shader-language features --------------------------------------------------------
    profiles = Counter(m.group(0) for m in re.finditer(r"\b(?:vs|ps)_\d+_\w+", text))
    feats["profiles"] = dict(profiles)

    intrinsics = {}
    for intr in ["tex2D", "tex2Dlod", "tex2Dbias", "tex2Dproj", "tex2Dgrad", "texCUBE", "tex3D"]:
        intrinsics[intr] = len(re.findall(r"(?<![A-Za-z0-9_])" + intr + r"\s*\(", text))
    feats["intrinsics"] = intrinsics

    feats["vfce"] = bool(re.search(r"(?<![A-Za-z0-9_])VFACE(?![A-Za-z0-9_])", text))
    feats["vpos"] = bool(re.search(r"(?<![A-Za-z0-9_])VPOS(?![A-Za-z0-9_])", text))
    feats["clip"] = len(re.findall(r"(?<![A-Za-z0-9_])clip\s*\(", text))
    feats["ddx"] = len(re.findall(r"(?<![A-Za-z0-9_])ddx\s*\(", text))
    feats["ddy"] = len(re.findall(r"(?<![A-Za-z0-9_])ddy\s*\(", text))
    feats["unroll"] = len(re.findall(r"\[\s*unroll\s*\]", text, re.IGNORECASE))
    feats["loop_attr"] = len(re.findall(r"\[\s*loop\s*\]", text, re.IGNORECASE))
    feats["mrt_outputs"] = sorted(set(re.findall(r"COLOR([0-3])\b", text)))
    feats["depth_output"] = bool(re.search(r":\s*DEPTH\b", text))

    # ---- function signatures (for vertex semantics + wrapper generation) -----------------
    functions = {}
    for fm in re.finditer(r"(?<![A-Za-z0-9_])(" + IDENT + r")[ \t]+(" + IDENT + r")[ \t]*\(", text):
        if depths[fm.start()] != 0:
            continue
        fn_name = fm.group(2)
        op = text.find("(", fm.start())
        cp = match_pair(text, op, "(", ")")
        if cp == -1:
            continue
        k = cp + 1
        while k < len(text) and text[k] in " \t\r\n":
            k += 1
        ret_sem = ""
        if text[k:k + 1] == ":":
            sm = re.match(r"[ \t\r\n]*(" + IDENT + r")", text[k + 1:])
            if sm:
                ret_sem = sm.group(1)
        functions[fn_name] = {
            "ret_type": fm.group(1),
            "ret_sem": ret_sem,
            "params": text[op + 1:cp],
        }
    feats["functions"] = functions

    # vertex input semantics used by vertex shader entry functions
    vs_semantics = Counter()
    for tech in feats["techniques"]:
        for p in tech["passes"]:
            sh = p["shaders"].get("vs")
            if not sh or sh.get("null"):
                continue
            fdef = functions.get(sh["fn"])
            if not fdef:
                continue
            for prm in parse_param_list(fdef["params"]):
                if prm and prm["semantic"]:
                    vs_semantics[prm["semantic"].upper()] += 1
    feats["vertex_semantics"] = dict(vs_semantics)

    # resolved texture paths need per-file context; filled by caller
    return feats


def parse_param_list(params_text):
    """Parse a function parameter list into records."""
    out = []
    if not params_text.strip() or params_text.strip() == "void":
        return out
    for part in split_top_level(params_text, ","):
        p = part.strip()
        if not p:
            continue
        m = re.match(
            r"((?:(?:uniform|in|out|inout|const)\s+)*)"
            r"(" + IDENT + r")\s+(" + IDENT + r")\s*"
            r"(?::\s*(" + IDENT + r"))?\s*$", p)
        if not m:
            out.append(None)
            continue
        modifiers = m.group(1)
        out.append({
            "modifiers": modifiers,
            "type": m.group(2),
            "name": m.group(3),
            "semantic": m.group(4) or "",
            "uniform": "uniform" in modifiers,
            "raw": p,
        })
    return out


# ---------------------------------------------------------------------------------------------
# Include following
# ---------------------------------------------------------------------------------------------

def gather_effect(effect_path, root):
    """Read an effect and its includes. Returns a record dict (no compile data)."""
    rec = {
        "path": effective_relpath(effect_path, root),
        "root": str(root),
        "abs_path": str(effect_path),
        "size": 0,
        "sha256": "",
        "encoding": "",
        "includes": [],
        "missing_includes": [],
        "error": None,
        "features": {},
    }
    try:
        raw = Path(effect_path).read_bytes()
    except OSError as e:
        rec["error"] = "read: %s" % e
        return rec
    rec["size"] = len(raw)
    rec["sha256"] = hashlib.sha256(raw).hexdigest()
    text, encoding = decode_bytes(raw)
    rec["encoding"] = encoding
    rec["raw_text"] = text

    includes = []
    missing = []
    visited = set()
    parts = []

    def walk(path, depth):
        if depth > MAX_INCLUDE_DEPTH:
            return
        try:
            p = Path(path)
            key = os.path.normcase(os.path.abspath(str(p)))
            if key in visited:
                return
            visited.add(key)
            raw2 = p.read_bytes()
        except OSError:
            return
        t2, _ = decode_bytes(raw2)
        t2 = strip_comments(t2)
        parts.append(t2)
        for m in INCLUDE_RE.finditer(t2):
            rel = m.group(1).strip()
            if not rel:
                continue
            inc = resolve_rel(p.parent, rel)
            if inc is None:
                inc = resolve_rel(Path(effect_path).parent, rel)
            if inc is None:
                missing.append(rel)
                continue
            includes.append(effective_relpath(inc, root))
            walk(inc, depth + 1)

    walk(Path(effect_path), 1)
    rec["includes"] = sorted(set(includes))
    rec["missing_includes"] = sorted(set(missing))
    combined = "\n".join(parts)
    try:
        rec["features"] = extract_features(combined)
    except Exception as e:  # never crash on a bad file
        rec["error"] = "features: %s" % e
        rec["features"] = {}
    # resolve texture ResourceNames against the effect folder (and include folders)
    try:
        resolve_textures(rec, effect_path, root)
    except Exception as e:
        rec.setdefault("warnings", []).append("textures: %s" % e)
    return rec


def resolve_textures(rec, effect_path, root):
    feats = rec.get("features") or {}
    base = Path(effect_path).parent
    for tex in feats.get("textures", []):
        rn = tex.get("ResourceName") or ""
        if not rn or rn.startswith("("):
            continue
        # ResourceName may be a define; best effort: try literal
        for cand_base in [base] + [p.parent for p in [Path(effect_path)]]:
            got = resolve_rel(cand_base, rn)
            if got:
                tex["resolved"] = effective_relpath(got, root)
                break
        if not tex["resolved"]:
            tex["missing"] = rn


def effective_relpath(path, root):
    try:
        return os.path.relpath(str(path), str(root)).replace("\\", "/")
    except ValueError:
        return str(path)


# ---------------------------------------------------------------------------------------------
# fxc compile test
# ---------------------------------------------------------------------------------------------

def find_fxc():
    base = Path("C:/Program Files (x86)/Windows Kits/10/bin")
    if not base.is_dir():
        return None
    cands = []
    for d in base.glob("10.*"):
        p = d / "x64" / "fxc.exe"
        if p.is_file():
            try:
                key = tuple(int(x) for x in d.name.split("."))
            except ValueError:
                key = (0,)
            cands.append((key, p))
    if not cands:
        return None
    cands.sort(key=lambda kv: kv[0])
    return str(cands[-1][1])


def run_fxc(fxc, args):
    cmd = [fxc, "/nologo"] + args
    t0 = time.time()
    try:
        proc = subprocess.run(cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                              timeout=FXC_TIMEOUT_SEC)
        out = proc.stdout.decode("utf-8", "replace")
        return {
            "returncode": proc.returncode,
            "output": out,
            "elapsed_ms": int((time.time() - t0) * 1000),
            "timeout": False,
        }
    except subprocess.TimeoutExpired as e:
        out = ""
        if e.stdout:
            out = e.stdout.decode("utf-8", "replace")
        return {"returncode": -1, "output": out,
                "elapsed_ms": int((time.time() - t0) * 1000), "timeout": True}
    except OSError as e:
        return {"returncode": -1, "output": str(e),
                "elapsed_ms": int((time.time() - t0) * 1000), "timeout": False}


def parse_diags(output):
    errors = []
    warnings = 0
    for line in output.splitlines():
        low = line.lower()
        if re.search(r"\berror\b", low):
            errors.append(line.strip())
        elif re.search(r"\bwarning\b", low):
            warnings += 1
    return errors, warnings


def strip_techniques(text):
    """Remove technique blocks and asm{} blocks (techniques pass do not need them for SM5)."""
    def remove_blocks(src, kw_re):
        out = []
        i = 0
        while True:
            m = kw_re.search(src, i)
            if not m:
                out.append(src[i:])
                break
            out.append(src[i:m.start()])
            j = m.end()
            while j < len(src) and src[j] in " \t\r\n":
                j += 1
            name = re.match(IDENT, src[j:])
            if name:
                j += len(name.group(0))
            _, j2 = read_annotation_block(src, j)
            j = j2 if j2 != j else j
            while j < len(src) and src[j] in " \t\r\n":
                j += 1
            if j < len(src) and src[j] == "{":
                close = match_pair(src, j, "{", "}")
                if close != -1:
                    i = close + 1
                    continue
            # not a block: emit keyword and advance past it
            i = m.end()
        return "".join(out)

    t = remove_blocks(text, TECH_RE)
    asm_re = re.compile(r"(?<![A-Za-z0-9_])asm[ \t]*\{")
    out = []
    i = 0
    while True:
        m = asm_re.search(t, i)
        if not m:
            out.append(t[i:])
            break
        out.append(t[i:m.start()])
        ob = t.find("{", m.start())
        close = match_pair(t, ob, "{", "}") if ob != -1 else -1
        i = close + 1 if close != -1 else m.end()
    return "".join(out)


def build_wrapper(text, fn, args):
    """Build a wrapper entry function. Returns (wrapper_name, wrapper_text) or (None, reason)."""
    fm = re.search(r"(?<![A-Za-z0-9_])(" + IDENT + r")[ \t]+" + re.escape(fn) + r"[ \t]*\(", text)
    if not fm:
        return None, "signature"
    ret_type = fm.group(1)
    op = text.find("(", fm.start())
    cp = match_pair(text, op, "(", ")")
    if cp == -1:
        return None, "signature"
    params = parse_param_list(text[op + 1:cp])
    if any(p is None for p in params):
        return None, "signature"
    k = cp + 1
    while k < len(text) and text[k] in " \t\r\n":
        k += 1
    ret_sem = ""
    if text[k:k + 1] == ":":
        sm = re.match(r"[ \t\r\n]*(" + IDENT + r")", text[k + 1:])
        if sm:
            ret_sem = sm.group(1)
    uniforms = [p for p in params if p["uniform"]]
    if len(uniforms) != len(args):
        return None, "signature"
    arg_iter = iter(args)
    wrap_params = []
    call_args = []
    for p in params:
        if p["uniform"]:
            call_args.append(next(arg_iter))
        else:
            call_args.append(p["name"])
            seg = p["modifiers"] + p["type"] + " " + p["name"]
            if p["semantic"]:
                seg += " : " + p["semantic"]
            wrap_params.append(seg)
    wname = "__mme_wrap_" + re.sub(r"\W", "_", fn)
    head = ret_type + " " + wname + "(" + ", ".join(wrap_params) + ")"
    if ret_sem:
        head += " : " + ret_sem
    call = fn + "(" + ", ".join(call_args) + ")"
    if ret_type == "void":
        body = "    " + call + ";\n"
    else:
        body = "    return " + call + ";\n"
    return wname, "\n" + head + " {\n" + body + "}\n"


def compile_effect(rec, fxc, out_dir, tmp_dir, index):
    """Run (a) fx_2_0 and (b) per-pass SM5 compiles. Fills rec["compile"]."""
    result = {"fx2": None, "sm5": {"passes": [], "note": ""},
              "only_after_technique_strip": False}
    rec["compile"] = result
    if not fxc:
        result["note"] = "no fxc"
        return
    raw_path = rec["abs_path"]
    folder = str(Path(raw_path).parent)
    bin_a = str(Path(tmp_dir) / ("fx2_%d.bin" % index))

    # ---- (a) fx_2_0 on the original bytes ----
    ra = run_fxc(fxc, ["/T", "fx_2_0", "/Fo", bin_a, "/I", folder, raw_path])
    errs, warns = parse_diags(ra["output"])
    result["fx2"] = {
        "ok": ra["returncode"] == 0 and not ra["timeout"],
        "errors": errs[:3],
        "warning_count": warns,
        "elapsed_ms": ra["elapsed_ms"],
        "timeout": ra["timeout"],
        "returncode": ra["returncode"],
    }

    # ---- (b) per pass, per stage on a technique-stripped temp copy ----
    feats = rec.get("features") or {}
    text = strip_comments(rec.get("raw_text", ""))
    stripped = strip_techniques(text)
    encoding = rec.get("encoding", "utf-8")

    known_names = set(p["name"] for p in feats.get("parameters", []))
    known_names |= set(feats.get("defines", []))
    known_names |= set(BY_NAME_UNIFORMS)
    known_names |= {"true", "false", "TRUE", "FALSE"}

    sm5_ok = False
    for ti, tech in enumerate(feats.get("techniques", [])):
        for pi, p in enumerate(tech.get("passes", [])):
            pass_rec = {
                "technique": tech.get("name"),
                "pass": p.get("name"),
                "stages": [],
                "ok": None,
                "error": None,
                "skip": None,
            }
            stage_results = []
            for stage in ("vs", "ps"):
                sh = p.get("shaders", {}).get(stage)
                if not sh or sh.get("null"):
                    continue
                profile = sh.get("profile", "")
                if not profile.lower().startswith(stage):
                    # profile stage does not match slot; skip
                    continue
                target = STAGE_TARGET[stage]
                args = sh.get("args", [])
                fn = sh.get("fn")
                sr = {"stage": stage, "profile": profile, "target": target, "entry": fn,
                      "wrapped": False, "skip": None, "ok": None, "error": None,
                      "timeout": False, "elapsed_ms": 0}
                tmp_src = stripped
                entry = fn
                if args:
                    known = all(a in known_names or not re.match(r"^[A-Za-z_]\w*$", a)
                                for a in args)
                    if not known:
                        sr["skip"] = "unresolved-arg"
                        stage_results.append(sr)
                        continue
                    wname, wtext = build_wrapper(stripped, fn, args)
                    if not wname:
                        sr["skip"] = "signature"
                        stage_results.append(sr)
                        continue
                    tmp_src = stripped + "\n" + wtext
                    entry = wname
                    sr["wrapped"] = True
                    sr["entry"] = wname
                tmp_path = Path(tmp_dir) / ("sm5_%d_%d_%d_%s.hlsl" % (index, ti, pi, stage))
                bin_b = Path(tmp_dir) / ("sm5_%d_%d_%d_%s.bin" % (index, ti, pi, stage))
                try:
                    tmp_path.write_bytes(encode_text(tmp_src, encoding))
                except OSError as e:
                    sr["skip"] = "tmpwrite"
                    sr["error"] = str(e)
                    stage_results.append(sr)
                    continue
                rb = run_fxc(fxc, ["/T", target, "/Gec", "/E", entry, "/Fo", str(bin_b),
                                   "/I", folder, str(tmp_path)])
                berrs, _ = parse_diags(rb["output"])
                sr["ok"] = rb["returncode"] == 0 and not rb["timeout"]
                sr["timeout"] = rb["timeout"]
                sr["elapsed_ms"] = rb["elapsed_ms"]
                sr["error"] = (berrs[0] if berrs else (rb["output"].splitlines()[-1].strip()
                                                       if rb["output"].strip() else None))
                if sr["ok"]:
                    sm5_ok = True
                stage_results.append(sr)
            pass_rec["stages"] = stage_results
            non_skip = [s for s in stage_results if not s["skip"]]
            if not non_skip:
                pass_rec["ok"] = None
                pass_rec["skip"] = (stage_results[0]["skip"] if stage_results else "noshader")
            elif all(s["ok"] for s in non_skip):
                pass_rec["ok"] = True
            else:
                pass_rec["ok"] = False
                fail = next((s for s in non_skip if not s["ok"]), None)
                pass_rec["error"] = fail["error"] if fail else None
            result["sm5"]["passes"].append(pass_rec)

    if result["fx2"] and not result["fx2"]["ok"] and sm5_ok:
        result["only_after_technique_strip"] = True


# ---------------------------------------------------------------------------------------------
# Output writing
# ---------------------------------------------------------------------------------------------

def write_scan_json(recs, out_path):
    with open(out_path, "w", encoding="utf-8") as f:
        json.dump(recs, f, ensure_ascii=False, indent=1, default=lambda o: sorted(o) if isinstance(o, set) else str(o))


def write_csv(recs, out_path):
    cols = ["root", "path", "size", "sha256", "encoding", "kind", "script_class", "script_order",
            "technique_count", "pass_count", "fx2_ok", "sm5_ok", "sm5_fail", "sm5_skip", "error"]
    with open(out_path, "w", encoding="utf-8", newline="") as f:
        w = csv.writer(f)
        w.writerow(cols)
        for r in recs:
            feats = r.get("features") or {}
            comp = r.get("compile") or {}
            fx2 = (comp.get("fx2") or {})
            passes = (comp.get("sm5") or {}).get("passes", [])
            sm5_ok = sum(1 for p in passes if p.get("ok") is True)
            sm5_fail = sum(1 for p in passes if p.get("ok") is False)
            sm5_skip = sum(1 for p in passes if p.get("ok") is None)
            gc = feats.get("global_class") or {}
            w.writerow([
                r.get("root", ""), r.get("path", ""), r.get("size", 0), r.get("sha256", ""),
                r.get("encoding", ""), gc.get("kind", ""), gc.get("ScriptClass", ""),
                gc.get("ScriptOrder", ""), feats.get("technique_count", 0),
                sum(len(t.get("passes", [])) for t in feats.get("techniques", [])),
                int(bool(fx2.get("ok"))), sm5_ok, sm5_fail, sm5_skip, r.get("error") or "",
            ])


def write_summary(recs, out_path):
    lines = []
    lines.append("# MME scan summary")
    lines.append("")
    lines.append("Effects scanned: %d" % len(recs))
    errs = [r for r in recs if r.get("error")]
    lines.append("Effects with scan errors: %d" % len(errs))
    lines.append("")

    # compile pass rates
    fx2_ok = fx2_fail = fx2_timeout = 0
    pass_ok = pass_fail = pass_skip = 0
    stage_ok = stage_fail = stage_skip = stage_timeout = 0
    strip_notes = 0
    for r in recs:
        comp = r.get("compile") or {}
        fx2 = comp.get("fx2")
        if fx2:
            if fx2.get("timeout"):
                fx2_timeout += 1
            elif fx2.get("ok"):
                fx2_ok += 1
            else:
                fx2_fail += 1
        for p in (comp.get("sm5") or {}).get("passes", []):
            if p.get("ok") is True:
                pass_ok += 1
            elif p.get("ok") is False:
                pass_fail += 1
            else:
                pass_skip += 1
            for s in p.get("stages", []):
                if s.get("timeout"):
                    stage_timeout += 1
                elif s.get("skip"):
                    stage_skip += 1
                elif s.get("ok"):
                    stage_ok += 1
                else:
                    stage_fail += 1
        if comp.get("only_after_technique_strip"):
            strip_notes += 1

    lines.append("## Compile results")
    lines.append("")
    lines.append("| metric | count |")
    lines.append("|---|---|")
    lines.append("| fx_2_0 ok | %d |" % fx2_ok)
    lines.append("| fx_2_0 fail | %d |" % fx2_fail)
    lines.append("| fx_2_0 timeout | %d |" % fx2_timeout)
    lines.append("| SM5 pass ok | %d |" % pass_ok)
    lines.append("| SM5 pass fail | %d |" % pass_fail)
    lines.append("| SM5 pass skip | %d |" % pass_skip)
    lines.append("| SM5 stage ok | %d |" % stage_ok)
    lines.append("| SM5 stage fail | %d |" % stage_fail)
    lines.append("| SM5 stage skip | %d |" % stage_skip)
    lines.append("| SM5 stage timeout | %d |" % stage_timeout)
    lines.append("| effects needing technique strip | %d |" % strip_notes)
    lines.append("")

    # feature frequency
    sem_counter = Counter()
    ann_counter = Counter()
    unknown_sem = Counter()
    unknown_ann = Counter()
    intr_counter = Counter()
    prof_counter = Counter()
    cmd_counter = Counter()
    sem_by_name = Counter()
    vs_sem_counter = Counter()
    kind_counter = Counter()
    for r in recs:
        feats = r.get("features") or {}
        gc = feats.get("global_class")
        if gc and gc.get("kind"):
            kind_counter[gc["kind"]] += 1
        for p in feats.get("parameters", []):
            sem_counter[p["semantic"]] += 1
            if p["semantic"] not in KNOWN_SEMANTICS:
                unknown_sem[p["semantic"]] += 1
            for an in p.get("annotations", {}):
                ann_counter[an] += 1
                if an.lower() not in KNOWN_ANNOTATIONS:
                    unknown_ann[an] += 1
        for t in feats.get("textures", []):
            if t.get("semantic"):
                sem_by_name[t["semantic"]] += 1
            for an in t.get("annotations", {}):
                ann_counter[an] += 1
                if an.lower() not in KNOWN_ANNOTATIONS:
                    unknown_ann[an] += 1
        for tech in feats.get("techniques", []):
            for an in tech.get("annotations", {}):
                ann_counter[an] += 1
                if an.lower() not in KNOWN_ANNOTATIONS:
                    unknown_ann[an] += 1
            for p in tech.get("passes", []):
                for an in p.get("annotations", {}):
                    ann_counter[an] += 1
                    if an.lower() not in KNOWN_ANNOTATIONS:
                        unknown_ann[an] += 1
        for k, v in (feats.get("intrinsics") or {}).items():
            if v:
                intr_counter[k] += v
        for k, v in (feats.get("profiles") or {}).items():
            prof_counter[k] += v
        for k, v in (feats.get("script_commands") or {}).items():
            cmd_counter[k] += v
        for k, v in (feats.get("vertex_semantics") or {}).items():
            vs_sem_counter[k] += v

    def table(title, counter):
        lines.append("## %s" % title)
        lines.append("")
        if not counter:
            lines.append("(none)")
            lines.append("")
            return
        lines.append("| name | count |")
        lines.append("|---|---|")
        for k, v in counter.most_common():
            lines.append("| %s | %d |" % (k, v))
        lines.append("")

    table("Derived effect kinds", kind_counter)
    table("Global semantics", sem_counter)
    table("Texture / render-target semantics", sem_by_name)
    table("Annotations", ann_counter)
    table("Script commands", cmd_counter)
    table("Shader profiles", prof_counter)
    table("Intrinsics", intr_counter)
    table("Vertex input semantics", vs_sem_counter)
    table("Unknown semantics", unknown_sem)
    table("Unknown annotations", unknown_ann)

    # top error messages grouped
    err_counter = Counter()
    for r in recs:
        comp = r.get("compile") or {}
        fx2 = comp.get("fx2")
        if fx2 and not fx2.get("ok") and not fx2.get("timeout"):
            for e in fx2.get("errors", [])[:1]:
                err_counter[normalise_error(e)] += 1
        for p in (comp.get("sm5") or {}).get("passes", []):
            if p.get("ok") is False and p.get("error"):
                err_counter[normalise_error(p["error"])] += 1
    lines.append("## Top error messages")
    lines.append("")
    if not err_counter:
        lines.append("(none)")
    else:
        lines.append("| count | message |")
        lines.append("|---|---|")
        for k, v in err_counter.most_common(40):
            lines.append("| %d | %s |" % (v, k.replace("|", "\\|")))
    lines.append("")

    with open(out_path, "w", encoding="utf-8") as f:
        f.write("\n".join(lines))


def normalise_error(line):
    s = line.strip()
    # strip file path + line/col prefix
    s = re.sub(r"^.*?\((\d+)[,)]", "(\\1)", s)
    s = re.sub(r"^.*?:\s*(error|warning)", r"\1", s, flags=re.IGNORECASE)
    return s[:200]


# ---------------------------------------------------------------------------------------------
# Main
# ---------------------------------------------------------------------------------------------

def collect_effects(roots):
    effects = []
    for root in roots:
        rp = Path(root)
        if rp.is_file() and rp.suffix.lower() == EFFECT_EXT:
            effects.append((rp, rp.parent))
            continue
        if not rp.is_dir():
            continue
        for dirpath, dirnames, filenames in os.walk(rp):
            dirnames.sort()
            for fn in sorted(filenames):
                if Path(fn).suffix.lower() == EFFECT_EXT:
                    effects.append((Path(dirpath) / fn, rp))
    effects.sort(key=lambda e: (os.path.normcase(str(e[1])), os.path.normcase(str(e[0]))))
    return effects


def main(argv=None):
    parser = argparse.ArgumentParser(description="Scan MME .fx effects and compile-test them.")
    parser.add_argument("roots", nargs="+", help="folders (or .fx files) to scan")
    parser.add_argument("--out", required=True, help="output directory")
    parser.add_argument("--fxc", default=None, help="path to fxc.exe (default: newest Windows Kit)")
    parser.add_argument("--no-compile", action="store_true", help="skip fxc compile tests")
    parser.add_argument("--jobs", type=int, default=0, help="parallel fxc processes (default: cpu count)")
    parser.add_argument("--limit", type=int, default=0, help="only process the first N effect files")
    args = parser.parse_args(argv)

    try:
        sys.stdout.reconfigure(encoding="utf-8", errors="replace")
    except Exception:
        pass

    out_dir = Path(args.out)
    out_dir.mkdir(parents=True, exist_ok=True)
    tmp_dir = out_dir / "tmp"
    if tmp_dir.exists():
        shutil.rmtree(tmp_dir, ignore_errors=True)
    tmp_dir.mkdir(parents=True, exist_ok=True)

    fxc = args.fxc or find_fxc()
    if not args.no_compile and not fxc:
        print("[W] no fxc.exe found; continue with --no-compile behaviour", flush=True)
    jobs = args.jobs if args.jobs and args.jobs > 0 else (os.cpu_count() or 4)

    effects = collect_effects(args.roots)
    if args.limit and args.limit > 0:
        effects = effects[:args.limit]
    total = len(effects)
    print("scanning %d effect file(s)" % total, flush=True)

    recs = []
    lock = threading.Lock()
    done = [0]

    def work(item):
        index, path, root = item
        rec = gather_effect(path, root)
        if not args.no_compile and fxc:
            try:
                compile_effect(rec, fxc, str(out_dir), str(tmp_dir), index)
            except Exception as e:  # never crash on a bad file
                rec["compile"] = {"error": "compile: %s" % e}
                rec.setdefault("warnings", []).append("compile: %s" % e)
        rec.pop("raw_text", None)  # full source is not persisted into scan.json
        with lock:
            done[0] += 1
            if done[0] % 25 == 0 or done[0] == total:
                print("  %d/%d" % (done[0], total), flush=True)
        return rec

    indexed = [(i + 1, p, r) for i, (p, r) in enumerate(effects)]
    if jobs > 1 and total > 1:
        with futures.ThreadPoolExecutor(max_workers=jobs) as ex:
            recs = list(ex.map(work, indexed))
    else:
        recs = [work(e) for e in indexed]

    recs.sort(key=lambda r: (os.path.normcase(r.get("root", "")), os.path.normcase(r.get("path", ""))))

    write_scan_json(recs, out_dir / "scan.json")
    write_csv(recs, out_dir / "scan.csv")
    write_summary(recs, out_dir / "summary.md")

    fx2_ok = sum(1 for r in recs if (r.get("compile") or {}).get("fx2", {}).get("ok"))
    sm5_ok = sum(1 for r in recs
                 if any(p.get("ok") is True
                        for p in (r.get("compile") or {}).get("sm5", {}).get("passes", [])))
    print("done: %d effects, fx_2_0 ok=%d, sm5(any pass ok)=%d -> %s" %
          (len(recs), fx2_ok, sm5_ok, out_dir), flush=True)

    shutil.rmtree(tmp_dir, ignore_errors=True)
    return 0


if __name__ == "__main__":
    sys.exit(main())
