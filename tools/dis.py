"""Disassemble x86 DLL. Usage: dis.py dll RVA_start RVA_end"""
import sys, pefile, capstone
pe = pefile.PE(sys.argv[1]); base = pe.OPTIONAL_HEADER.ImageBase
a, b = int(sys.argv[2], 16), int(sys.argv[3], 16)
data = pe.get_memory_mapped_image()[a:b]
md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_32)
for i in md.disasm(data, base + a):
    print(f"{i.address - base:06x}  {i.mnemonic:7s} {i.op_str}")
