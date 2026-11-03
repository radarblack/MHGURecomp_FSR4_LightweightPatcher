import argparse
import os
import re
import struct
import sys
import urllib.request

HERE = os.path.dirname(os.path.abspath(__file__))
VENDORED_HEADER = os.path.join(HERE, "..", "include", "ffx_vk_portable_abi.h")
UPSTREAM_HEADER_URL = "https://raw.githubusercontent.com/morerokk/MHGURecomp/master/third_party/fsr-vulkan/include/ffx_vk_portable.h"
HOOKED = ["ffxVkPortableUpscaleContextRecordDispatch", "ffxVkPortableUpscaleContextCreate"]
STRUCTS = [
    "FfxVkPortableUpscaleDispatchInfo",
    "FfxVkPortableUpscaleCreateInfo",
    "FfxVkPortableImage",
    "FfxVkPortableExtent2D",
    "FfxVkPortableFloat2",
]


class Pe:
    def __init__(self, data):
        self.data = data
        pe = struct.unpack_from("<I", data, 0x3C)[0]
        if data[:2] != b"MZ" or data[pe:pe + 4] != b"PE\0\0":
            raise ValueError("not a PE file")
        self.coff = pe + 4
        self.sections_count, = struct.unpack_from("<H", data, self.coff + 2)
        self.sym_ptr, self.sym_count = struct.unpack_from("<II", data, self.coff + 8)
        opt_size, = struct.unpack_from("<H", data, self.coff + 16)
        self.opt = self.coff + 20
        self.magic, = struct.unpack_from("<H", data, self.opt)
        if self.magic != 0x20B:
            raise ValueError("not a 64-bit PE file")
        self.image_base, = struct.unpack_from("<Q", data, self.opt + 24)
        self.sections = []
        table = self.opt + opt_size
        for i in range(self.sections_count):
            name, vsize, va, rsize, rptr = struct.unpack_from("<8sIIII", data, table + i * 40)
            self.sections.append((name.rstrip(b"\0").decode(errors="replace"), vsize, va, rsize, rptr))

    def rva_to_offset(self, rva):
        for _name, vsize, va, rsize, rptr in self.sections:
            if va <= rva < va + max(vsize, rsize):
                return rptr + rva - va
        return None

    def exports(self):
        dd = self.opt + 112
        rva, size = struct.unpack_from("<II", self.data, dd)
        off = self.rva_to_offset(rva) if rva else None
        result = {}
        if off is None:
            return result
        n_names, funcs, names, ords = struct.unpack_from("<IIII", self.data, off + 24)
        names_off, ords_off, funcs_off = self.rva_to_offset(names), self.rva_to_offset(ords), self.rva_to_offset(funcs)
        for i in range(n_names):
            name_rva, = struct.unpack_from("<I", self.data, names_off + i * 4)
            o = self.rva_to_offset(name_rva)
            end = self.data.index(b"\0", o)
            ordinal, = struct.unpack_from("<H", self.data, ords_off + i * 2)
            func_rva, = struct.unpack_from("<I", self.data, funcs_off + ordinal * 4)
            result[self.data[o:end].decode()] = func_rva
        return result

    def symbols(self, wanted):
        found = {}
        if not self.sym_ptr or not self.sym_count:
            return found
        strings = self.sym_ptr + self.sym_count * 18
        i = 0
        while i < self.sym_count:
            s = self.sym_ptr + i * 18
            raw = self.data[s:s + 8]
            if raw[:4] == b"\0\0\0\0":
                off, = struct.unpack_from("<I", raw, 4)
                end = self.data.index(b"\0", strings + off)
                name = self.data[strings + off:end].decode(errors="replace")
            else:
                name = raw.rstrip(b"\0").decode(errors="replace")
            if name in wanted:
                value, section = struct.unpack_from("<Ih", self.data, s + 8)
                if 0 < section <= len(self.sections):
                    found[name] = self.sections[section - 1][2] + value
            i += 1 + self.data[s + 17]
        return found

    def call_sites(self, target_rva):
        text = [s for s in self.sections if s[0] == ".text"]
        if not text:
            return []
        _n, vsize, va, rsize, rptr = text[0]
        blob = self.data[rptr:rptr + min(vsize, rsize)]
        sites = []
        for m in re.finditer(rb"[\xE8\xE9]", blob):
            i = m.start()
            if i + 5 > len(blob):
                continue
            rel, = struct.unpack_from("<i", blob, i + 1)
            if va + i + 5 + rel == target_rva:
                sites.append((va + i, "call" if blob[i] == 0xE8 else "jmp"))
        return sites


def parse_structs(header_text):
    result = {}
    for name in STRUCTS:
        m = re.search(r"typedef struct " + name + r"\s*\{(.*?)\}\s*" + name + r"\s*;", header_text, re.S)
        if not m:
            result[name] = None
            continue
        body = re.sub(r"/\*.*?\*/|//[^\n]*", "", m.group(1), flags=re.S)
        result[name] = [" ".join(f.split()) for f in body.split(";") if f.strip()]
    return result


def abi_version(header_text):
    m = re.search(r"#define\s+FFX_VK_PORTABLE_ABI_VERSION\s+(\d+)", header_text)
    return int(m.group(1)) if m else None


def check_exe(path):
    ok = True
    pe = Pe(open(path, "rb").read())
    exports = pe.exports()
    symbols = pe.symbols(set(HOOKED))
    for name in HOOKED:
        rva = exports.get(name)
        how = "export table"
        if rva is None:
            rva, how = symbols.get(name), "symbol table"
        if rva is None:
            print("MISSING  %s: not exported and not in the symbol table (the plugin cannot hook it)" % name)
            ok = ok and name != HOOKED[0]
            continue
        sites = pe.call_sites(rva)
        calls = [s for s in sites if s[1] == "call"]
        print("found    %s via %s at RVA 0x%x, %d direct call site(s)" % (name, how, rva, len(calls)))
        if not sites:
            print("WARNING  no direct call reaches the entry point of %s: it may have been inlined or cloned, so the hook may never run" % name)
            if name == HOOKED[0]:
                ok = False
    return ok


def check_header(upstream_text):
    vendored = open(VENDORED_HEADER, encoding="utf-8").read()
    ok = True
    va, ua = abi_version(vendored), abi_version(upstream_text)
    print("ABI version: plugin built for %s, upstream %s" % (va, ua))
    if va != ua:
        print("INCOMPATIBLE  ABI version changed: refresh include/ffx_vk_portable_abi.h, review the dispatch mapping in src/fsr4rt.c")
        ok = False
    vs, us = parse_structs(vendored), parse_structs(upstream_text)
    for name in STRUCTS:
        v, u = vs[name], us[name]
        if v is None or u is None:
            print("INCOMPATIBLE  %s not found in %s" % (name, "the vendored header" if v is None else "upstream"))
            ok = False
        elif u[:len(v)] == v:
            extra = len(u) - len(v)
            print("ok       %s%s" % (name, " (+%d field(s) appended upstream: harmless, plugin ignores them)" % extra if extra else ""))
        else:
            print("INCOMPATIBLE  %s changed in a way that moves existing fields" % name)
            ok = False
    return ok


def main():
    parser = argparse.ArgumentParser(description="Check whether a MHGURecomp build/header is still compatible with the FSR 4 layer plugin")
    parser.add_argument("--exe", help="path to mhgurecomp.exe (Windows build)")
    parser.add_argument("--header", help="path to upstream ffx_vk_portable.h (default: download from GitHub master)")
    args = parser.parse_args()
    ok = True
    if args.exe:
        ok = check_exe(args.exe) and ok
    if args.header:
        text = open(args.header, encoding="utf-8").read()
    else:
        text = urllib.request.urlopen(UPSTREAM_HEADER_URL, timeout=30).read().decode("utf-8")
    ok = check_header(text) and ok
    print("\nCOMPATIBLE: the plugin should keep working" if ok else "\nNOT COMPATIBLE: see the lines above")
    sys.exit(0 if ok else 1)


if __name__ == "__main__":
    main()
