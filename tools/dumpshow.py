# 可读化显示字节 dump: python tools/dumpshow.py <file.bin>
import sys

special = {0x1B: "<ESC>", 0x0D: "<CR>", 0x0A: "<LF>\n", 0x07: "<BEL>", 0x09: "<TAB>"}

data = open(sys.argv[1], "rb").read()
out = []
for b in data:
    if b in special:
        out.append(special[b])
    elif 32 <= b < 127:
        out.append(chr(b))
    else:
        out.append("<%02X>" % b)
print("".join(out))
print("---- %d bytes, %s ----" % (len(data), sys.argv[1]))
