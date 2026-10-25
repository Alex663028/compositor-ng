"""Count PE export names in a DLL (no external deps). Usage: python export_count.py <dll>"""
import struct
import sys


def count_exports(path):
    data = open(path, "rb").read()
    e_lfanew = struct.unpack_from("<I", data, 0x3C)[0]
    if data[e_lfanew : e_lfanew + 4] != b"PE\0\0":
        raise SystemExit("not a PE file")
    coff = e_lfanew + 4
    num_sections, = struct.unpack_from("<H", data, coff + 2)
    opt_size, = struct.unpack_from("<H", data, coff + 16)
    opt = coff + 20
    magic, = struct.unpack_from("<H", data, opt)
    dd = opt + (112 if magic == 0x20B else 96)
    export_rva, export_size = struct.unpack_from("<II", data, dd)
    sections = []
    sec = opt + opt_size
    for i in range(num_sections):
        off = sec + i * 40
        vsize, vaddr, rawsize, rawptr = struct.unpack_from("<IIII", data, off + 8)
        sections.append((vaddr, max(vsize, rawsize), rawptr))

    def rva_to_off(rva):
        for vaddr, size, rawptr in sections:
            if vaddr <= rva < vaddr + size:
                return rawptr + (rva - vaddr)
        raise SystemExit("rva not mapped: %#x" % rva)

    names_rva, = struct.unpack_from("<I", data, rva_to_off(export_rva) + 32)
    count, = struct.unpack_from("<I", data, rva_to_off(export_rva) + 24)
    out = []
    base = rva_to_off(names_rva)
    for i in range(count):
        name_rva, = struct.unpack_from("<I", data, base + i * 4)
        off = rva_to_off(name_rva)
        end = data.index(b"\0", off)
        out.append(data[off:end].decode("ascii"))
    return out


if __name__ == "__main__":
    names = count_exports(sys.argv[1])
    print("exported symbols:", len(names))
    non = [n for n in names if not n.startswith("comp_")]
    print("non-comp_ names:", non)
