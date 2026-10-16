import sys, pefile
pe = pefile.PE(sys.argv[1])
print('machine', hex(pe.FILE_HEADER.Machine), 'imagebase', hex(pe.OPTIONAL_HEADER.ImageBase))
for s in pe.sections: print(s.Name.rstrip(b'\0'), hex(s.VirtualAddress), hex(s.Misc_VirtualSize), hex(s.SizeOfRawData))
if hasattr(pe, 'DIRECTORY_ENTRY_EXPORT'):
    for e in pe.DIRECTORY_ENTRY_EXPORT.symbols: print('EXP', hex(e.address), e.name)
for d in getattr(pe, 'DIRECTORY_ENTRY_IMPORT', []):
    print('IMP', d.dll.decode(), ' '.join((i.name or b'?').decode() for i in d.imports))
