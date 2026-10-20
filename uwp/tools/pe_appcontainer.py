"""pe_appcontainer.py EXE -- set IMAGE_DLLCHARACTERISTICS_APPCONTAINER (0x1000), as a packaged app's exe must have."""
import struct
import sys

p = sys.argv[1]
b = bytearray(open(p, "rb").read())
pe = struct.unpack_from("<I", b, 0x3C)[0]
assert b[pe:pe + 4] == b"PE\0\0", "not a PE image"
opt = pe + 24
assert struct.unpack_from("<H", b, opt)[0] == 0x20B, "not a PE32+ (x64) image"
dc = struct.unpack_from("<H", b, opt + 70)[0]
struct.pack_into("<H", b, opt + 70, dc | 0x1000)
open(p, "wb").write(b)
print("  exe: DllCharacteristics %04X -> %04X (AppContainer)" % (dc, dc | 0x1000))
