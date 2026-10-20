"""imports_inc.py IMPORTS.TXT OUT.INC -- an exe's import list ("DLL FUNC" lines, from llvm-readobj) as a C
initializer for probe/jitprobe.c's census: DLL, its functions, "" -- so the console checks exactly what the
loader will ask it for."""
import sys
from collections import OrderedDict

by = OrderedDict()
for line in open(sys.argv[1]):
    parts = line.split()
    if len(parts) == 2:
        by.setdefault(parts[0], []).append(parts[1])
with open(sys.argv[2], "w") as f:
    for dll, fns in by.items():
        f.write('"%s", %s "",\n' % (dll, "".join('"%s", ' % fn for fn in fns)))
print("%d DLLs, %d functions" % (len(by), sum(len(v) for v in by.values())))
