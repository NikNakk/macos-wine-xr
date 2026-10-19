#!/usr/bin/env python3
# Copyright 2026, Nick Kennedy
# SPDX-License-Identifier: LGPL-2.1-or-later
"""Describe the Steam interface methods the Wine Steam bridge cannot pass through as plain integers.

The bridge forwards every interface method by vtable index. Integer and pointer
arguments need no help: both calling conventions take them in order. This
script reads Steamworks SDK headers and lists, per interface version string,
the methods that do need help:

- float or double arguments (Microsoft and System V put them in different registers);
- an 8-byte class returned by value (CSteamID, CGameID): Microsoft returns it
  through a hidden pointer, System V in rax;
- other structures returned or passed by value, and game-side response objects
  (ISteamMatchmaking*Response) the client would call through their Windows
  vtables, which the bridge refuses;
- function pointer arguments (Windows code the Mac client would call);
- SteamNetworkingMessage_t traffic, whose release functions must be swapped.

Input: one or more directories of Steamworks headers. Proton's
lsteamclient/steamworks_sdk_* directories cover every interface version Steam
has shipped. Only numbers and names reach the output; no header text does.

    generate_steam_bridge_table.py --output src/steam_bridge/steam_methods.h \
        /path/to/Proton/lsteamclient/steamworks_sdk_*
"""
import argparse
import pathlib
import re
import sys

INT_TYPES = {
    "int", "unsigned", "unsigned int", "short", "unsigned short", "char", "unsigned char", "signed char",
    "long", "unsigned long", "long long", "unsigned long long", "bool", "uint8", "int8", "uint16", "int16",
    "uint32", "int32", "uint64", "int64", "uint64_t", "int64_t", "uint32_t", "int32_t", "uint16_t", "int16_t",
    "uint8_t", "int8_t", "intptr_t", "uintptr_t", "size_t", "lint64", "ulint64", "void",
    # 8-byte classes passed in a general register by both conventions.
    "CSteamID", "CGameID",
}
MAX_PARAMS = 10  # STEAM_MAX_PARAMS in src/steam_bridge/steam_methods_types.h
FLOAT_TYPES = {"float": "F32", "double": "F64"}
CLASS8_RETURNS = {"CSteamID", "CGameID"}
# Function pointer typedefs with a known shape.
FNPTR_KINDS = {
    "SteamAPIWarningMessageHook_t": "FN_INT_STRING",
    "FSteamNetworkingSocketsDebugOutput": "FN_INT_STRING",
    "SteamAPI_CheckCallbackRegistered_t": "FN_ALWAYS_ONE",
    "SteamAPI_PostAPIResultInProcess_t": "FN_NOTHING",
}
MESSAGE_METHODS = {
    "SendMessages": "MSG_SEND",
    "ReceiveMessagesOnConnection": "MSG_RECEIVE",
    "ReceiveMessagesOnPollGroup": "MSG_RECEIVE",
    "ReceiveMessagesOnChannel": "MSG_RECEIVE",
    "AllocateMessage": "MSG_ALLOCATE",
}


def strip_comments(text):
    text = re.sub(r"/\*.*?\*/", " ", text, flags=re.S)
    return re.sub(r"//[^\n]*", " ", text)


def drop_console_blocks(text):
    """Removes #if branches for consoles (_PS3), which a Windows or macOS build never compiles."""
    out, stack = [], []                     # stack of "this branch is live" flags
    for line in text.split("\n"):
        directive = line.strip()
        live = all(stack)
        if directive.startswith(("#if", "#ifdef", "#ifndef")):
            console = "PS3" in directive
            negated = directive.startswith("#ifndef") or "!defined" in directive.replace(" ", "")
            stack.append(not console or negated)
            continue
        if directive.startswith(("#elif", "#else")) and stack:
            stack[-1] = not stack[-1] if directive.startswith("#else") else stack[-1]
            continue
        if directive.startswith("#endif") and stack:
            stack.pop()
            continue
        if live:
            out.append(line)
    return "\n".join(out)


def unwrap_macros(text):
    # STEAM_PRIVATE_API( virtual ... ) keeps the declaration; annotation macros vanish.
    text = re.sub(r"STEAM_PRIVATE_API\s*\(", "(", text)
    text = re.sub(r"\b(STEAM_[A-Z_]+|METHOD_DESC|DEPRECATED)\s*\((?:[^()]|\([^()]*\))*\)", " ", text)
    text = re.sub(r"\bSTEAM_[A-Z_]+\b", " ", text)
    return text


def find_classes(text):
    """Yields (class name, body) for every class or struct with virtual methods."""
    for match in re.finditer(r"\b(?:class|struct)\s+(ISteam\w+)\s*(?::[^{;]*)?\{", text):
        depth, i = 1, match.end()
        while i < len(text) and depth:
            depth += {"{": 1, "}": -1}.get(text[i], 0)
            i += 1
        yield match.group(1), text[match.end():i - 1]


def split_params(params):
    out, depth, cur = [], 0, ""
    for ch in params:
        if ch == "," and depth == 0:
            out.append(cur)
            cur = ""
            continue
        depth += ch in "(<"
        depth -= ch in ")>"
        cur += ch
    if cur.strip():
        out.append(cur)
    return [p.strip() for p in out if p.strip() and p.strip() != "void"]


def param_type(param):
    param = re.sub(r"=.*$", "", param).strip()
    if "(*" in param:
        return "(*)"
    param = re.sub(r"\[.*?\]", "*", param)
    words = param.replace("*", " * ").replace("&", " & ").split()
    if len(words) > 1 and words[-1] not in ("*", "&") and re.match(r"^[A-Za-z_]\w*$", words[-1]):
        words = words[:-1]                     # drop the parameter name
    return " ".join(w for w in words if w not in ("const", "volatile", "struct", "enum", "class"))


class Types:
    def __init__(self):
        self.typedefs = {}
        self.enums = set()
        self.structs = set()
        self.fnptrs = set()

    def learn(self, text):
        for m in re.finditer(r"typedef\s+([^;(){}]+?)\s+(\w+)\s*;", text):
            self.typedefs[m.group(2)] = " ".join(w for w in m.group(1).split() if w not in ("const", "struct", "enum"))
        for m in re.finditer(r"typedef\s+[^;]*?\(\s*\w*\s*\*\s*(\w+)\s*\)\s*\(", text):
            self.fnptrs.add(m.group(1))
        for m in re.finditer(r"\benum\s+(\w+)", text):
            self.enums.add(m.group(1))
        for m in re.finditer(r"\b(?:struct|class)\s+(\w+)\s*(?::[^{;]*)?\{", text):
            self.structs.add(m.group(1))

    def kind(self, typ):
        if typ == "(*)":
            return "FNPTR", None
        base = typ.replace("*", "").replace("&", "").strip()
        if base.startswith("ISteam") and base.endswith("Response"):
            return "STRUCT", base               # a game object the client would call through its vtable
        if "*" in typ or "&" in typ:
            return "INT", None
        seen = set()
        while typ in self.typedefs and typ not in seen and typ not in INT_TYPES:
            seen.add(typ)
            target = self.typedefs[typ]
            if "*" in target:
                return "INT", None
            typ = target
        if typ in FLOAT_TYPES:
            return FLOAT_TYPES[typ], None
        if typ in INT_TYPES or typ in self.enums or typ.startswith("E") and typ[1:2].isupper():
            return "INT", None
        if typ in self.fnptrs:
            return "FNPTR", typ
        if typ.startswith("H") or typ.endswith("_t") and typ not in self.structs:
            return "INT", None                  # handles and ids: typedef'd integers in every SDK
        return "STRUCT", typ


def parse_methods(body):
    body = re.sub(r"\b(public|protected|private)\s*:", " ", body)
    methods = []
    for decl in re.finditer(r"\bvirtual\s+([^;{]*?)\s*(?:=\s*0)?\s*;", body, flags=re.S):
        sig = " ".join(decl.group(1).split())
        m = re.match(r"^(.*?)\b(\w+)\s*\((.*)\)\s*(?:const)?$", sig)
        if not m:
            raise ValueError(f"cannot read: virtual {sig}")
        ret, name, params = m.group(1).strip(), m.group(2), m.group(3)
        if name.startswith("~"):
            continue
        methods.append((name, param_type(ret + " x") if ret else "void", [param_type(p) for p in split_params(params)]))
    return methods


def version_macros(text):
    return {m.group(1): m.group(2) for m in re.finditer(r"#define\s+(\w+_INTERFACE_VERSION\w*)\s+\"([^\"]+)\"", text)}


def macro_for(classname, macros):
    stem = classname[1:].upper()                 # ISteamUser -> STEAMUSER
    for name in (stem + "_INTERFACE_VERSION", stem + "_VERSION"):
        if name in macros:
            return macros[name]
    return None


def scan(dirs):
    interfaces = {}
    problems = []
    for directory in dirs:
        types = Types()
        headers = sorted(pathlib.Path(directory).glob("*.h"))
        texts = {h: unwrap_macros(drop_console_blocks(strip_comments(h.read_text(errors="replace")))) for h in headers}
        for text in texts.values():
            types.learn(text)
        macros = {}
        for text in texts.values():
            macros.update(version_macros(text))
        for header, text in texts.items():
            for classname, body in find_classes(text):
                version = macro_for(classname, version_macros(text)) or macro_for(classname, macros)
                if not version:
                    continue
                try:
                    methods = parse_methods(body)
                except ValueError as error:
                    problems.append(f"{header}: {classname}: {error}")
                    continue
                described = []
                for index, (name, ret, params) in enumerate(methods):
                    rkind = "CLASS8" if ret in CLASS8_RETURNS else types.kind(ret)[0]
                    args, special, fnarg = [], None, None
                    if len(params) > MAX_PARAMS:
                        raise SystemExit(f"{version} {name}: {len(params)} parameters; raise STEAM_MAX_PARAMS")
                    for position, p in enumerate(params):
                        kind, typ = types.kind(p)
                        if kind == "FNPTR":
                            if special is None:
                                special, fnarg = FNPTR_KINDS.get(typ or "", "FN_DEFERRED"), position
                            kind = "INT"
                        args.append(kind)
                    if "PostAPIResultInProcess" in name:
                        special = "FN_NOTHING"          # retired; steam_api still sets it
                    special = special or MESSAGE_METHODS.get(name)
                    if rkind in ("INT",) and all(a == "INT" for a in args) and not special:
                        continue
                    described.append((index, name, rkind, args, special, fnarg))
                previous = interfaces.get(version)
                record = (classname, len(methods), described, header.parent.name)
                if previous:
                    # Valve appends methods to a published version and renames retired ones; what the
                    # bridge needs is that every slot both SDKs have is described the same way.
                    common = min(previous[1], len(methods))
                    def shape(entries):
                        return {e[0]: e[2:] for e in entries if e[0] < common}
                    if shape(previous[2]) != shape(described):
                        problems.append(f"{version}: {previous[3]} and {header.parent.name} describe its methods differently")
                        continue
                    if len(methods) <= previous[1]:
                        continue
                interfaces[version] = record
    return interfaces, problems


def emit(interfaces, output):
    lines = [
        "/* Generated by tools/generate_steam_bridge_table.py from Steamworks SDK headers. Do not edit. */",
        "/* SPDX-License-Identifier: LGPL-2.1-or-later */",
        "#include \"steam_methods_types.h\"",
        "",
    ]
    table = []
    for n, version in enumerate(sorted(interfaces)):
        classname, count, described, _ = interfaces[version]
        if described:
            lines.append(f"static const struct steam_method methods_{n}[] = {{")
            for index, name, rkind, args, special, fnarg in described:
                arg_list = ", ".join(f"ARG_{a}" for a in args) or "0"
                fn = 0xFF if fnarg is None else fnarg
                lines.append(f"    {{ {index}, RET_{rkind}, {special or 'SPECIAL_NONE'}, {fn:#x}, {len(args)}, {{ {arg_list} }}, \"{name}\" }},")
            lines.append("};")
        table.append(f"    {{ \"{version}\", \"{classname}\", {count}, {len(described)}, {'methods_%d' % n if described else 'NULL'} }},")
    lines.append("static const struct steam_interface steam_interfaces[] = {")
    lines += table
    lines.append("};")
    pathlib.Path(output).write_text("\n".join(lines) + "\n")


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--output", required=True)
    parser.add_argument("dirs", nargs="+")
    args = parser.parse_args()
    interfaces, problems = scan(args.dirs)
    for problem in problems:
        print("warning:", problem, file=sys.stderr)
    emit(interfaces, args.output)
    described = sum(len(v[2]) for v in interfaces.values())
    print(f"{len(interfaces)} interface versions, {described} methods that need more than integers", file=sys.stderr)


if __name__ == "__main__":
    main()
