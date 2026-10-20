"""msys2_fetch.py SYSROOT PKG... -- prebuilt MSYS2 clang64 packages (and their dependencies) unpacked into SYSROOT.

The clang64 environment is built with clang against UCRT and libc++, the same ABI as llvm-mingw, so its static
libraries link into an llvm-mingw build. Packages land under SYSROOT/clang64/{include,lib,...}; pkg-config .pc files
keep their /clang64 prefix, which tools/cross-pkg-config rewrites through PKG_CONFIG_SYSROOT_DIR.

  python3 tools/msys2_fetch.py ~/xemu-uwp/sysroot glib2 libepoxy libsamplerate openssl zlib libslirp pixman
"""
import io
import os
import re
import sys
import tarfile
import urllib.request

import zstandard

MIRROR = "https://mirror.msys2.org/mingw/clang64/"
PREFIX = "mingw-w64-clang-x86_64-"
# never pulled in: build tools and runtimes the cross compiler already provides
SKIP = {"crt-git", "headers-git", "libc++", "libunwind", "libwinpthread", "winpthreads", "winpthreads-git",
        "libwinpthread-git", "compiler-rt", "llvm-libs", "clang", "llvm", "python", "pkgconf", "cc-libs", "gcc-libs",
        "ca-certificates", "p11-kit", "libtasn1", "tzdata"}


def fetch(url):
    with urllib.request.urlopen(url) as r:
        return r.read()


def read_db():
    raw = fetch(MIRROR + "clang64.db")
    try:
        raw = zstandard.ZstdDecompressor().decompress(raw, max_output_size=1 << 30)
    except zstandard.ZstdError:
        pass                                    # gzip/plain: tarfile sniffs it
    db = {}
    provides = {}
    with tarfile.open(fileobj=io.BytesIO(raw)) as t:
        for m in t.getmembers():
            if not m.name.endswith("/desc"):
                continue
            text = t.extractfile(m).read().decode()
            fields = {}
            for block in text.strip().split("\n\n"):
                lines = block.strip().split("\n")
                fields[lines[0].strip("%")] = lines[1:]
            name = fields["NAME"][0]
            db[name] = fields
            for p in fields.get("PROVIDES", []):
                provides[re.split("[<>=]", p)[0]] = name
    return db, provides


def main():
    root = os.path.expanduser(sys.argv[1])
    want = sys.argv[2:]
    db, provides = read_db()
    todo = [PREFIX + w for w in want]
    seen = []
    while todo:
        n = todo.pop()
        n = provides.get(n, n) if n not in db else n
        if n in seen or n[len(PREFIX):] in SKIP:
            continue
        if n not in db:
            print("  (not in clang64: %s)" % n)
            continue
        seen.append(n)
        for d in db[n].get("DEPENDS", []):
            todo.append(re.split("[<>=]", d)[0])
    os.makedirs(root, exist_ok=True)
    for n in seen:
        fn = db[n]["FILENAME"][0]
        stamp = os.path.join(root, ".pkgs", fn)
        if os.path.exists(stamp):
            continue
        print("  %s" % fn)
        data = zstandard.ZstdDecompressor().decompress(fetch(MIRROR + fn), max_output_size=1 << 31) \
            if fn.endswith(".zst") else fetch(MIRROR + fn)
        with tarfile.open(fileobj=io.BytesIO(data)) as t:
            t.extractall(root, members=[m for m in t.getmembers() if not m.name.startswith(".")], filter="tar")
        os.makedirs(os.path.dirname(stamp), exist_ok=True)
        open(stamp, "w").close()
    print("%d packages in %s" % (len(seen), root))


main()
