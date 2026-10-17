"""Reset Generation engine under CPU emulation (proof of concept for the Android port).

Loads ResetGeneration-Win32-Web.dll (x86) into Unicorn, provides a minimal Win32
(kernel32/winmm) layer and a fake JNIEnv, then drives the engine through the
same JNI entry points the Java wrapper uses. Frames are dumped as PNG and the
audio stream as WAV, and timing is reported, so we can judge whether "emulate
the original engine on ARM" is a viable Android strategy.

Usage: py -I rgemu.py <game ResetGeneration dir> <out dir> [frames] [width height]
       [--keys "frame:KEY,..."] [--files] [--rep-patch]
The game dir must hold the unpatched *.pak files, the DLL, settings.cfg and achievements.dat.
probes/ has the one-off instrumentation scripts used to find the boot/draw path.
"""
import os
import struct
import sys
import time
import wave

import pefile
from unicorn import Uc, UcError, UC_ARCH_X86, UC_MODE_32, UC_HOOK_CODE, UC_HOOK_INTR, UC_HOOK_MEM_UNMAPPED, UC_PROT_ALL
from unicorn.x86_const import (UC_X86_REG_EAX, UC_X86_REG_ECX, UC_X86_REG_EDX, UC_X86_REG_ESP,
                               UC_X86_REG_EIP, UC_X86_REG_EBP, UC_X86_REG_GDTR,
                               UC_X86_REG_SS, UC_X86_REG_DS, UC_X86_REG_ES, UC_X86_REG_FS)

PAGE = 0x1000
STACK_BASE, STACK_SIZE = 0x00100000, 0x00200000
TEB_ADDR = 0x00400000
STUB_BASE = 0x00500000          # one 16-byte slot per import / JNI function
EXIT_ADDR = 0x005FF000          # return address that ends a host->guest call
HEAP_BASE, HEAP_SIZE = 0x01000000, 0x0E000000
VIRT_BASE, VIRT_SIZE = 0x20000000, 0x10000000
JNI_BASE = 0x00600000           # JNIEnv struct + function table + handle data


def align(x, a=PAGE):
    return (x + a - 1) & ~(a - 1)


class Emu:
    def __init__(self, dll_path, game_dir):
        self.game_dir = game_dir
        self.uc = Uc(UC_ARCH_X86, UC_MODE_32)
        self.stubs = {}            # addr -> (name, nargs, fn)
        self.next_stub = STUB_BASE
        self.handles = {}          # file handle -> file object
        self.finds = {}            # find handle -> remaining paths
        self.next_handle = 0x100
        self.objects = {}          # JNI handle -> python object
        self.next_obj = 0x1000
        self.heap_top = HEAP_BASE
        self.heap_sizes = {}
        self.free_lists = {}
        self.virt_top = VIRT_BASE
        self.tls = {}
        self.next_tls = 1
        self.last_error = 0
        self.vtime = 5000000       # virtual milliseconds for timeGetTime/GetTickCount
        self.api_calls = 0
        self.api_hist = {}
        self.hook_time = 0.0
        self.missing = set()
        self.log_api = False
        self.api_trace = None
        self._map(STACK_BASE, STACK_SIZE)
        self._map(TEB_ADDR, PAGE * 4)
        self._map(STUB_BASE, 0x100000)
        self._map(JNI_BASE, 0x100000)
        self._map(HEAP_BASE, HEAP_SIZE)
        self._map(VIRT_BASE, VIRT_SIZE)
        # Each 16-byte stub slot is "int 2Eh; ret": the interrupt hook runs the shim. An
        # interrupt trap is much cheaper for Unicorn than a per-instruction code hook.
        self.uc.mem_write(STUB_BASE, (b'\xcd\x2e\xc3' + b'\x90' * 13) * (0x100000 // 16))
        self._setup_teb()
        self.pe = self._load_pe(dll_path)
        self._setup_jni()
        self.uc.hook_add(UC_HOOK_INTR, self._on_intr)
        self.uc.hook_add(UC_HOOK_MEM_UNMAPPED, self._on_bad_mem)

    # ------------------------------------------------------------------ memory
    def _map(self, addr, size):
        self.uc.mem_map(addr, align(size), UC_PROT_ALL)

    def r32(self, a):
        return struct.unpack('<I', self.uc.mem_read(a, 4))[0]

    def w32(self, a, v):
        self.uc.mem_write(a, struct.pack('<I', v & 0xffffffff))

    def cstr(self, a, maxlen=4096):
        if not a:
            return None
        out = bytearray()
        while len(out) < maxlen:
            chunk = self.uc.mem_read(a + len(out), 64)
            i = chunk.find(b'\0')
            if i >= 0:
                out += chunk[:i]
                break
            out += chunk
        return out.decode('latin1')

    def wstr(self, a, n=-1):
        out = []
        while n < 0 or len(out) < n:
            c = struct.unpack('<H', self.uc.mem_read(a + 2 * len(out), 2))[0]
            if c == 0 and n < 0:
                break
            out.append(c)
        return ''.join(map(chr, out))

    def alloc(self, n):
        n = max(16, (n + 15) & ~15)
        fl = self.free_lists.get(n)
        if fl:
            p = fl.pop()
        else:
            p = self.heap_top
            self.heap_top += n
            if self.heap_top > HEAP_BASE + HEAP_SIZE:
                raise MemoryError('guest heap exhausted')
        self.heap_sizes[p] = n
        return p

    def free(self, p):
        n = self.heap_sizes.pop(p, None)
        if n:
            self.free_lists.setdefault(n, []).append(p)

    def put_bytes(self, b):
        p = self.alloc(len(b) + 1)
        self.uc.mem_write(p, bytes(b) + b'\0')
        return p

    # ------------------------------------------------------------------ PE
    def _load_pe(self, path):
        pe = pefile.PE(path)
        base = pe.OPTIONAL_HEADER.ImageBase
        size = align(pe.OPTIONAL_HEADER.SizeOfImage)
        self._map(base, size)
        self.uc.mem_write(base, pe.get_memory_mapped_image()[:size])
        self.image_base = base
        self.exports = {}
        for e in pe.DIRECTORY_ENTRY_EXPORT.symbols:
            self.exports[e.name.decode()] = base + e.address
        for d in pe.DIRECTORY_ENTRY_IMPORT:
            for imp in d.imports:
                name = imp.name.decode()
                self.w32(imp.address, self._stub(name))
        return pe

    def _stub(self, name):
        addr = self.next_stub
        self.next_stub += 16
        impl = WIN32.get(name)
        if impl is None:
            self.stubs[addr] = (name, None, None)
        else:
            self.stubs[addr] = (name, impl[0], impl[1])
        return addr

    def _setup_teb(self):
        teb = TEB_ADDR
        self.w32(teb + 0x00, 0xffffffff)            # ExceptionList: end of chain
        self.w32(teb + 0x04, STACK_BASE + STACK_SIZE)
        self.w32(teb + 0x08, STACK_BASE)
        self.w32(teb + 0x18, teb)                   # self
        self.w32(teb + 0x24, 1)                     # thread id
        self.w32(teb + 0x30, teb + 0x1000)          # PEB
        # 32-bit Unicorn has no writable FS base: build a GDT whose FS entry points at the TEB.
        gdt = TEB_ADDR + 0x2000

        def desc(base, limit, access, flags=0xC):
            return struct.pack('<Q', (limit & 0xffff) | ((base & 0xffffff) << 16) | (access << 40)
                               | (((limit >> 16) & 0xf) << 48) | (flags << 52) | ((base >> 24) << 56))
        entries = [bytes(8),
                   desc(0, 0xfffff, 0x9b),        # 1: code
                   desc(0, 0xfffff, 0x93),        # 2: data
                   desc(teb, 0xfff, 0x93, 0x4),   # 3: fs -> TEB
                   desc(0, 0xfffff, 0x93)]        # 4: ss (ring 0)
        self.uc.mem_write(gdt, b''.join(entries))
        self.uc.reg_write(UC_X86_REG_GDTR, (0, gdt, len(entries) * 8 - 1, 0))
        self.uc.reg_write(UC_X86_REG_SS, 4 << 3)
        self.uc.reg_write(UC_X86_REG_DS, 2 << 3)
        self.uc.reg_write(UC_X86_REG_ES, 2 << 3)
        self.uc.reg_write(UC_X86_REG_FS, 3 << 3)

    # ------------------------------------------------------------------ JNI
    def _setup_jni(self):
        self.env = JNI_BASE
        table = JNI_BASE + 0x100
        self.w32(self.env, table)
        for i in range(240):
            addr = self.next_stub
            self.next_stub += 16
            name, nargs, fn = JNI.get(i, ('JNI_%d' % i, None, None))
            self.stubs[addr] = (name, nargs, fn)
            self.w32(table + 4 * i, addr)

    def new_obj(self, o):
        h = self.next_obj
        self.next_obj += 4
        self.objects[h] = o
        return h

    # ------------------------------------------------------------------ calls
    def patch_rep_strings(self):
        """Replace 'rep movs/stos' with 'int 2Fh' (both 2 bytes); the trap does the copy natively.
        QEMU-based emulators run REP string ops one iteration per dispatch, which dominated
        the profile (blits)."""
        import capstone
        md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_32)
        text = [s for s in self.pe.sections if s.Name.startswith(b'.text')][0]
        start = self.image_base + text.VirtualAddress
        code = bytes(self.uc.mem_read(start, text.Misc_VirtualSize))
        self.rep_sites = {}
        md.skipdata = True
        for i in md.disasm(code, start):
            if i.size == 2 and i.bytes[0] == 0xF3 and i.bytes[1] in (0xA4, 0xA5, 0xAA, 0xAB):
                self.rep_sites[i.address] = i.bytes[1]
        for a in self.rep_sites:
            self.uc.mem_write(a, bytes([0xCD, 0x2F]))
        return len(self.rep_sites)

    def _rep_string(self, uc, site):
        from unicorn.x86_const import UC_X86_REG_ESI, UC_X86_REG_EDI, UC_X86_REG_EFLAGS
        op = self.rep_sites[site]
        n = uc.reg_read(UC_X86_REG_ECX)
        if uc.reg_read(UC_X86_REG_EFLAGS) & 0x400:
            raise RuntimeError('rep string op with DF=1 at %#x' % site)
        width = 4 if op in (0xA5, 0xAB) else 1
        edi = uc.reg_read(UC_X86_REG_EDI)
        nbytes = n * width
        if op in (0xA4, 0xA5):
            esi = uc.reg_read(UC_X86_REG_ESI)
            if nbytes:
                uc.mem_write(edi, bytes(uc.mem_read(esi, nbytes)))
            uc.reg_write(UC_X86_REG_ESI, esi + nbytes)
        elif nbytes:
            v = uc.reg_read(UC_X86_REG_EAX)
            pat = struct.pack('<I', v) if width == 4 else bytes([v & 0xff])
            uc.mem_write(edi, pat * n)
        uc.reg_write(UC_X86_REG_EDI, edi + nbytes)
        uc.reg_write(UC_X86_REG_ECX, 0)

    def _on_intr(self, uc, intno, user):
        if intno == 0x2f:
            self._rep_string(uc, uc.reg_read(UC_X86_REG_EIP) - 2)
            return
        if intno != 0x2e:
            eip = uc.reg_read(UC_X86_REG_EIP)
            raise RuntimeError('guest interrupt %#x at %#x' % (intno, eip))
        self._on_stub(uc, uc.reg_read(UC_X86_REG_EIP) - 2, 0, user)

    def _on_stub(self, uc, addr, size, user):
        t0 = time.perf_counter()
        try:
            self._on_stub_inner(uc, addr, size, user)
        finally:
            self.hook_time += time.perf_counter() - t0

    def _on_stub_inner(self, uc, addr, size, user):
        if addr == EXIT_ADDR:
            uc.emu_stop()
            return
        st = self.stubs.get(addr)
        if st is None:
            return
        name, nargs, fn = st
        esp = uc.reg_read(UC_X86_REG_ESP)
        ret = self.r32(esp)
        args = [self.r32(esp + 4 + 4 * i) for i in range(nargs or 0)]
        self.api_calls += 1
        self.api_hist[name] = self.api_hist.get(name, 0) + 1
        if fn is None:
            if name not in self.missing:
                self.missing.add(name)
                print('  [unimplemented] %s' % name)
            result = 0
            nargs = nargs or 0
        else:
            result = fn(self, *args)
        if self.api_trace:
            self.api_trace.write('%s %s\n' % (name, '-' if result is None else '%x' % (result & 0xffffffff)))
        if self.log_api:
            print('  %s(%s) -> %s' % (name, ', '.join(hex(a) for a in args), result if result is None else hex(result & 0xffffffff)))
        if result is not None:
            uc.reg_write(UC_X86_REG_EAX, result & 0xffffffff)
        # stdcall: callee pops args; cdecl (nargs<0) leaves them.
        pop = 4 * nargs if nargs and nargs > 0 else 0
        uc.reg_write(UC_X86_REG_ESP, esp + 4 + pop)
        uc.reg_write(UC_X86_REG_EIP, ret)

    def _on_bad_mem(self, uc, access, addr, size, value, user):
        eip = uc.reg_read(UC_X86_REG_EIP)
        print('  !! unmapped memory access at %#x (eip=%#x, rva %#x)' % (addr, eip, eip - self.image_base))
        return False

    def call(self, addr, *args, ecx=None):
        esp = STACK_BASE + STACK_SIZE - 0x100 if not hasattr(self, '_depth') or self._depth == 0 else self.uc.reg_read(UC_X86_REG_ESP) - 0x40
        self._depth = getattr(self, '_depth', 0) + 1
        saved = self.uc.reg_read(UC_X86_REG_ESP)
        for a in reversed(args):
            esp -= 4
            self.w32(esp, a)
        esp -= 4
        self.w32(esp, EXIT_ADDR)
        self.uc.reg_write(UC_X86_REG_ESP, esp)
        if ecx is not None:
            self.uc.reg_write(UC_X86_REG_ECX, ecx)
        try:
            self.uc.emu_start(addr, EXIT_ADDR)
        except UcError as e:
            eip = self.uc.reg_read(UC_X86_REG_EIP)
            raise RuntimeError('emulation error %s at eip=%#x (rva %#x)' % (e, eip, eip - self.image_base))
        finally:
            self._depth -= 1
            if self._depth:
                self.uc.reg_write(UC_X86_REG_ESP, saved)
        return self.uc.reg_read(UC_X86_REG_EAX)

    def export(self, prefix):
        for k, v in self.exports.items():
            if prefix in k:
                return v
        raise KeyError(prefix)


# ====================================================================== Win32
WIN32 = {}


def api(name, nargs):
    def deco(fn):
        WIN32[name] = (nargs, fn)
        return fn
    return deco


def const(name, nargs, value):
    WIN32[name] = (nargs, lambda emu, *a: value)


for _n, _a, _v in [
        ('GetCurrentThreadId', 0, 1), ('GetCurrentProcessId', 0, 1234), ('GetCurrentProcess', 0, 0xffffffff),
        ('SetHandleCount', 1, 32), ('GetFileType', 1, 2), ('SetUnhandledExceptionFilter', 1, 0),
        ('UnhandledExceptionFilter', 1, 0), ('IsBadWritePtr', 2, 0), ('IsBadReadPtr', 2, 0), ('IsBadCodePtr', 1, 0),
        ('GetACP', 0, 1252), ('GetOEMCP', 0, 437), ('FlushFileBuffers', 1, 1), ('SetStdHandle', 2, 1),
        ('VirtualProtect', 4, 1), ('VirtualFree', 3, 1), ('HeapDestroy', 1, 1), ('HeapCreate', 3, 0x7000),
        ('GetModuleHandleA', 1, 0x10000000), ('LoadLibraryA', 1, 0), ('GetProcAddress', 2, 0),
        ('SetEnvironmentVariableA', 2, 1), ('FreeEnvironmentStringsA', 1, 1), ('FreeEnvironmentStringsW', 1, 1),
        ('GetTimeZoneInformation', 1, 0), ('CreateDirectoryA', 2, 1), ('RemoveDirectoryA', 1, 1),
        ('CreateMutexA', 3, 0x50), ('CreateSemaphoreA', 4, 0x51), ('ReleaseMutex', 1, 1),
        ('ReleaseSemaphore', 3, 1), ('WaitForSingleObject', 2, 0), ('InitializeCriticalSection', 1, None),
        ('DeleteCriticalSection', 1, None), ('EnterCriticalSection', 1, None), ('LeaveCriticalSection', 1, None),
        ('SetEndOfFile', 1, 1), ('DeleteFileA', 1, 1), ('RtlUnwind', 4, 0)]:
    const(_n, _a, _v)


@api('GetLastError', 0)
def _gle(emu):
    return emu.last_error


@api('SetLastError', 1)
def _sle(emu, v):
    emu.last_error = v


@api('InterlockedExchange', 2)
def _ixchg(emu, p, v):
    old = emu.r32(p)
    emu.w32(p, v)
    return old


@api('GetVersionExA', 1)
def _gvx(emu, p):
    emu.uc.mem_write(p + 4, struct.pack('<4I', 5, 1, 2600, 2))   # Windows XP
    return 1


@api('GetCommandLineA', 0)
def _gcl(emu):
    if not hasattr(emu, '_cmdline'):
        emu._cmdline = emu.put_bytes(b'java.exe')
    return emu._cmdline


@api('GetEnvironmentStrings', 0)
def _ges(emu):
    return emu.put_bytes(b'A=B\0')


@api('GetEnvironmentStringsW', 0)
def _gesw(emu):
    return emu.put_bytes('A=B\0\0'.encode('utf-16le'))


@api('GetStartupInfoA', 1)
def _gsi(emu, p):
    emu.uc.mem_write(p, struct.pack('<I', 68) + b'\0' * 64)


@api('GetStdHandle', 1)
def _gsh(emu, n):
    return {0xfffffff6: 1, 0xfffffff5: 2, 0xfffffff4: 3}.get(n, 0xffffffff)


@api('GetModuleFileNameA', 3)
def _gmfn(emu, mod, buf, n):
    s = (emu.game_dir.replace('/', '\\') + '\\ResetGeneration-Win32-Web.dll').encode()[:n - 1]
    emu.uc.mem_write(buf, s + b'\0')
    return len(s)


@api('GetSystemInfo', 1)
def _gsysi(emu, p):
    emu.uc.mem_write(p, struct.pack('<HHIIIIIIHH', 0, 0, PAGE, 0x10000, 0x7ffeffff, 1, 1, 586, 0x10000, 0))


@api('GetCPInfo', 2)
def _gcpi(emu, cp, p):
    emu.uc.mem_write(p, struct.pack('<I', 1) + b'?\0' + b'\0' * 12)
    return 1


@api('GetLocaleInfoA', 4)
def _gli(emu, lcid, typ, buf, n):
    return 0


@api('HeapAlloc', 3)
def _halloc(emu, h, flags, n):
    p = emu.alloc(n)
    if flags & 8:
        emu.uc.mem_write(p, b'\0' * n)
    return p


@api('HeapFree', 3)
def _hfree(emu, h, flags, p):
    emu.free(p)
    return 1


@api('HeapReAlloc', 4)
def _hrealloc(emu, h, flags, p, n):
    old = emu.heap_sizes.get(p, 0)
    q = emu.alloc(n)
    if p and old:
        emu.uc.mem_write(q, bytes(emu.uc.mem_read(p, min(old, n))))
        emu.free(p)
    if flags & 8 and n > old:
        emu.uc.mem_write(q + old, b'\0' * (n - old))
    return q


@api('HeapSize', 3)
def _hsize(emu, h, flags, p):
    return emu.heap_sizes.get(p, 0xffffffff)


@api('VirtualAlloc', 4)
def _valloc(emu, addr, size, typ, prot):
    if addr and VIRT_BASE <= addr < emu.virt_top:
        return addr                       # commit inside an earlier reservation
    p = emu.virt_top
    emu.virt_top += align(size, 0x10000)
    return p


@api('VirtualQuery', 3)
def _vquery(emu, addr, buf, n):
    emu.uc.mem_write(buf, struct.pack('<7I', addr & ~0xfff, addr & ~0xffff, 4, PAGE, 0x1000, 4, 0x20000))
    return 28


@api('TlsAlloc', 0)
def _tlsalloc(emu):
    i = emu.next_tls
    emu.next_tls += 1
    return i


@api('TlsFree', 1)
def _tlsfree(emu, i):
    return 1


@api('TlsSetValue', 2)
def _tlsset(emu, i, v):
    emu.tls[i] = v
    return 1


@api('TlsGetValue', 1)
def _tlsget(emu, i):
    emu.last_error = 0
    return emu.tls.get(i, 0)


@api('timeGetTime', 0)
def _tgt(emu):
    return emu.vtime


@api('GetTickCount', 0)
def _gtc(emu):
    return emu.vtime


@api('QueryPerformanceCounter', 1)
def _qpc(emu, p):
    emu.uc.mem_write(p, struct.pack('<Q', emu.vtime * 1000))
    return 1


@api('GetSystemTimeAsFileTime', 1)
def _gstaft(emu, p):
    emu.uc.mem_write(p, struct.pack('<Q', 128000000000000000 + emu.vtime * 10000))


@api('FileTimeToLocalFileTime', 2)
def _ftlft(emu, a, b):
    emu.uc.mem_write(b, bytes(emu.uc.mem_read(a, 8)))
    return 1


@api('FileTimeToSystemTime', 2)
def _ftst(emu, a, b):
    emu.uc.mem_write(b, struct.pack('<8H', 2009, 5, 6, 30, 12, 0, 0, 0))
    return 1


@api('Sleep', 1)
def _sleep(emu, ms):
    emu.vtime += ms


@api('ExitProcess', 1)
def _exitp(emu, code):
    raise SystemExit('guest ExitProcess(%d)' % code)


@api('TerminateProcess', 2)
def _termp(emu, h, code):
    raise SystemExit('guest TerminateProcess(%d)' % code)


@api('RaiseException', 4)
def _raise(emu, code, flags, n, args):
    eip = emu.r32(emu.uc.reg_read(UC_X86_REG_ESP))
    raise RuntimeError('guest RaiseException %#x from %#x (C++ exception; SEH dispatch not implemented)' % (code, eip))


def _host_path(emu, p):
    p = p.replace('\\', '/')
    if not os.path.isabs(p) and not (len(p) > 1 and p[1] == ':'):
        p = os.path.join(emu.game_dir, p)
    return p


@api('CreateFileA', 7)
def _createfile(emu, name, access, share, sa, disp, flags, tmpl):
    path = _host_path(emu, emu.cstr(name))
    write = bool(access & 0x40000000)
    try:
        if write:
            mode = 'r+b' if os.path.exists(path) and disp in (3, 4) else 'w+b'
            if disp == 2:
                mode = 'w+b'
            f = open(path, mode)
        else:
            f = open(path, 'rb')
    except OSError:
        emu.last_error = 2
        if emu.log_files:
            print('  CreateFileA %s -> not found' % path)
        return 0xffffffff
    if emu.log_files:
        print('  CreateFileA %s (%s)' % (path, 'w' if write else 'r'))
    h = emu.next_handle
    emu.next_handle += 4
    emu.handles[h] = f
    return h


@api('ReadFile', 5)
def _readfile(emu, h, buf, n, pread, ovl):
    f = emu.handles.get(h)
    data = f.read(n) if f else b''
    emu.uc.mem_write(buf, data)
    if pread:
        emu.w32(pread, len(data))
    return 1


@api('WriteFile', 5)
def _writefile(emu, h, buf, n, pwritten, ovl):
    data = bytes(emu.uc.mem_read(buf, n))
    if h in (2, 3):
        sys.stdout.write(data.decode('latin1'))
    elif h in emu.handles:
        emu.handles[h].write(data)
    if pwritten:
        emu.w32(pwritten, n)
    return 1


@api('SetFilePointer', 4)
def _sfp(emu, h, dist, phigh, method):
    f = emu.handles.get(h)
    if not f:
        return 0xffffffff
    if dist & 0x80000000:
        dist -= 1 << 32
    f.seek(dist, method)
    if phigh:
        emu.w32(phigh, 0)
    return f.tell()


@api('CloseHandle', 1)
def _close(emu, h):
    f = emu.handles.pop(h, None)
    if f:
        f.close()
    return 1


def _fill_find(emu, data, path):
    st = os.stat(path)
    attr = 0x10 if os.path.isdir(path) else 0x80
    name = os.path.basename(path).encode('latin1')[:259]
    emu.uc.mem_write(data, struct.pack('<I', attr) + bytes(24) + struct.pack('<II', st.st_size >> 32, st.st_size & 0xffffffff)
                     + bytes(8) + name + bytes(260 - len(name)) + bytes(14))


@api('FindFirstFileA', 2)
def _fff(emu, pat, data):
    import fnmatch
    pattern = _host_path(emu, emu.cstr(pat))
    d, mask = os.path.split(pattern)
    try:
        names = [n for n in sorted(os.listdir(d)) if fnmatch.fnmatch(n.lower(), mask.lower())]
    except OSError:
        names = []
    if emu.log_files:
        print('  FindFirstFileA %s -> %d matches' % (pattern, len(names)))
    if not names:
        emu.last_error = 2
        return 0xffffffff
    h = emu.next_handle
    emu.next_handle += 4
    emu.finds[h] = [os.path.join(d, n) for n in names]
    _fill_find(emu, data, emu.finds[h].pop(0))
    return h


@api('FindNextFileA', 2)
def _fnf(emu, h, data):
    lst = emu.finds.get(h)
    if not lst:
        emu.last_error = 18
        return 0
    _fill_find(emu, data, lst.pop(0))
    return 1


@api('FindClose', 1)
def _fclose(emu, h):
    emu.finds.pop(h, None)
    return 1


@api('MultiByteToWideChar', 6)
def _mb2wc(emu, cp, flags, src, n, dst, dn):
    s = bytes(emu.uc.mem_read(src, n)) if n >= 0 and n != 0xffffffff else (emu.cstr(src) + '\0').encode('latin1')
    w = s.decode('latin1')
    if dn == 0:
        return len(w)
    emu.uc.mem_write(dst, w[:dn].encode('utf-16le'))
    return min(len(w), dn)


@api('WideCharToMultiByte', 8)
def _wc2mb(emu, cp, flags, src, n, dst, dn, defc, used):
    w = emu.wstr(src, -1) + '\0' if n == 0xffffffff else emu.wstr(src, n)
    b = w.encode('latin1', 'replace')
    if dn == 0:
        return len(b)
    emu.uc.mem_write(dst, b[:dn])
    return min(len(b), dn)


@api('LCMapStringA', 6)
def _lcmapa(emu, lcid, flags, src, n, dst, dn):
    s = emu.cstr(src) if n == 0xffffffff else bytes(emu.uc.mem_read(src, n)).decode('latin1')
    s = s.lower() if flags & 0x100 else s.upper() if flags & 0x200 else s
    if dn == 0:
        return len(s)
    emu.uc.mem_write(dst, s.encode('latin1')[:dn])
    return min(len(s), dn)


@api('LCMapStringW', 6)
def _lcmapw(emu, lcid, flags, src, n, dst, dn):
    s = emu.wstr(src, -1 if n == 0xffffffff else n)
    s = s.lower() if flags & 0x100 else s.upper() if flags & 0x200 else s
    if dn == 0:
        return len(s)
    emu.uc.mem_write(dst, s.encode('utf-16le')[:2 * dn])
    return min(len(s), dn)


@api('GetStringTypeA', 5)
def _gsta(emu, lcid, typ, src, n, out):
    s = bytes(emu.uc.mem_read(src, n))
    emu.uc.mem_write(out, b''.join(struct.pack('<H', _ctype(chr(c))) for c in s))
    return 1


@api('GetStringTypeW', 4)
def _gstw(emu, typ, src, n, out):
    s = emu.wstr(src, n)
    emu.uc.mem_write(out, b''.join(struct.pack('<H', _ctype(c)) for c in s))
    return 1


def _ctype(c):
    v = 0
    if c.isupper(): v |= 1
    if c.islower(): v |= 2
    if c.isdigit(): v |= 4
    if c.isspace(): v |= 8
    if c in '!"#$%&\'()*+,-./:;<=>?@[\\]^_`{|}~': v |= 0x10
    if ord(c) < 32 or ord(c) == 127: v |= 0x20
    if c == ' ' or c == '\t': v |= 0x40
    if c.isalpha(): v |= 0x100
    return v


@api('CompareStringA', 6)
def _cmpa(emu, lcid, flags, a, na, b, nb):
    sa = emu.cstr(a) if na == 0xffffffff else bytes(emu.uc.mem_read(a, na)).decode('latin1')
    sb = emu.cstr(b) if nb == 0xffffffff else bytes(emu.uc.mem_read(b, nb)).decode('latin1')
    if flags & 1:
        sa, sb = sa.lower(), sb.lower()
    return 1 if sa < sb else 3 if sa > sb else 2


@api('CompareStringW', 6)
def _cmpw(emu, lcid, flags, a, na, b, nb):
    sa = emu.wstr(a, -1 if na == 0xffffffff else na)
    sb = emu.wstr(b, -1 if nb == 0xffffffff else nb)
    if flags & 1:
        sa, sb = sa.lower(), sb.lower()
    return 1 if sa < sb else 3 if sa > sb else 2


# ====================================================================== JNI
JNI = {}


def jni(index, name, nargs):
    def deco(fn):
        JNI[index] = (name, nargs, fn)
        return fn
    return deco


@jni(21, 'NewGlobalRef', 2)
def _ngr(emu, env, o):
    return o


@jni(23, 'DeleteLocalRef', 2)
def _dlr(emu, env, o):
    return None


@jni(22, 'DeleteGlobalRef', 2)
def _dgr(emu, env, o):
    return None


@jni(31, 'GetObjectClass', 2)
def _goc(emu, env, o):
    return emu.new_obj(('class-of', o))


@jni(6, 'FindClass', 2)
def _fc(emu, env, name):
    return emu.new_obj(('class', emu.cstr(name)))


@jni(33, 'GetMethodID', 4)
def _gmid(emu, env, cls, name, sig):
    m = emu.new_obj(('method', emu.cstr(name), emu.cstr(sig)))
    return m


@jni(113, 'GetStaticMethodID', 4)
def _gsmid(emu, env, cls, name, sig):
    return emu.new_obj(('smethod', emu.cstr(name), emu.cstr(sig)))


@jni(62, 'CallVoidMethodV', 4)
def _cvmv(emu, env, obj, mid, va):
    m = emu.objects.get(mid)
    emu.java_calls.append(m[1] if m else mid)
    return None


@jni(61, 'CallVoidMethod', -3)
def _cvm(emu, env, obj, mid):
    m = emu.objects.get(mid)
    emu.java_calls.append(m[1] if m else mid)
    return None


@jni(167, 'NewStringUTF', 2)
def _nsu(emu, env, s):
    return emu.new_obj(emu.cstr(s))


@jni(169, 'GetStringUTFChars', 3)
def _gsuc(emu, env, h, iscopy):
    s = emu.objects.get(h)
    if not isinstance(s, str):
        return 0
    return emu.put_bytes(s.encode('utf-8'))


@jni(170, 'ReleaseStringUTFChars', 3)
def _rsuc(emu, env, h, p):
    emu.free(p)
    return None


@jni(168, 'GetStringUTFLength', 2)
def _gsul(emu, env, h):
    return len(emu.objects.get(h, '').encode('utf-8'))


@jni(171, 'GetArrayLength', 2)
def _gal(emu, env, h):
    a = emu.objects.get(h)
    return len(a) if a is not None else 0


@jni(230, 'GetDirectBufferAddress', 2)
def _gdba(emu, env, h):
    return emu.objects.get(h)[1]


@jni(17, 'ExceptionClear', 1)
def _exc(emu, env):
    return None


@jni(228, 'ExceptionCheck', 1)
def _exchk(emu, env):
    return 0


# ====================================================================== driver
def jstring(emu, s):
    return emu.new_obj(s)


def direct_buffer(emu, nbytes):
    p = emu.alloc(nbytes)
    emu.uc.mem_write(p, b'\0' * nbytes)
    return emu.new_obj(('dbuf', p, nbytes)), p


def save_frame(emu, fb, w, h, path):
    from PIL import Image
    raw = bytes(emu.uc.mem_read(fb, w * h * 4))
    Image.frombuffer('RGBX', (w, h), raw, 'raw', 'BGRX', 0, 1).convert('RGB').save(path)


VK = {'RETURN': 10, 'ESC': 27, 'UP': 38, 'DOWN': 40, 'LEFT': 37, 'RIGHT': 39, 'CTRL': 17, 'SPACE': 32}


def main():
    game_dir = os.path.abspath(sys.argv[1])
    out = os.path.abspath(sys.argv[2])
    frames = int(sys.argv[3]) if len(sys.argv) > 3 else 300
    w, h = (int(sys.argv[4]), int(sys.argv[5])) if len(sys.argv) > 5 else (640, 360)
    keys = {}
    if '--keys' in sys.argv:
        # "frame:KEY,frame:KEY..." -> press at frame, release 6 frames later
        for item in sys.argv[sys.argv.index('--keys') + 1].split(','):
            f, k = item.split(':')
            keys.setdefault(int(f), []).append(('down', k))
            keys.setdefault(int(f) + 6, []).append(('up', k))
    shots = set(range(0, frames, max(1, frames // 12))) | {frames - 1}
    os.makedirs(out, exist_ok=True)
    emu = Emu(os.path.join(game_dir, 'ResetGeneration-Win32-Web.dll'), game_dir)
    emu.log_files = '--files' in sys.argv
    if '--api-trace' in sys.argv:
        emu.api_trace = open(sys.argv[sys.argv.index('--api-trace') + 1], 'w')
    emu.java_calls = []
    if '--rep-patch' in sys.argv:
        print('patched %d rep movs/stos sites' % emu.patch_rep_strings())
    t0 = time.time()
    print('DllMain ->', emu.call(emu.image_base + emu.pe.OPTIONAL_HEADER.AddressOfEntryPoint, emu.image_base, 1, 0))
    env, this = emu.env, emu.new_obj('engine')
    emu.call(emu.export('NativeGameEngine_setHomeDirectory'), env, this, jstring(emu, game_dir.replace('\\', '/')))
    fbh, fb = direct_buffer(emu, w * (h + 1) * 4)
    print('init ...')
    emu.call(emu.export('NativeGameEngine_init@'), env, this, fbh, jstring(emu, 'en'), 0, w, h,
             jstring(emu, ''), 0, 0, 0)
    # SNAPMobileClient.initNative(): shared 2 KB network buffer (online play; unused offline).
    neth, _ = direct_buffer(emu, 2048)
    emu.call(emu.export('NativeSNAPMobileInterface_initNative'), env, emu.new_obj('snap'), neth, 2048, 876)
    for pak in sorted(f for f in os.listdir(game_dir) if f.endswith('.pak')):
        emu.call(emu.export('NativeGameEngine_notifyDataPackageReady'), env, this, jstring(emu, pak))
    print('boot took %.1fs, %d API calls' % (time.time() - t0, emu.api_calls))
    pkg = emu.r32(0x101b8278)
    print('package flags:', bytes(emu.uc.mem_read(emu.r32(pkg + 0x10), 9)).hex())
    sndh, snd = direct_buffer(emu, 1024)
    on_sound = emu.export('NativeSoundOutputDevice_onSoundBufferEmpty')
    audio = bytearray()
    listener = emu.new_obj('input')
    t_emu = 0.0
    emu.hook_time = 0.0
    for i in range(frames):
        for kind, k in keys.get(i, []):
            fn = 'onKeyPressed@' if kind == 'down' else 'onKeyReleased@'
            emu.call(emu.export('NativeInputEventListener_' + fn), env, listener, emu.vtime, VK[k], 0, 1)
        t = time.time()
        emu.vtime += 1                          # engine Sleep()s the rest of its 32 fps frame
        alive = emu.call(emu.export('NativeGameEngine_tick'), env, this)
        # 8 kHz mono 16-bit: 33 ms of audio is ~528 bytes; pull one 1 KB buffer every other frame.
        if i % 2 == 0:
            emu.call(on_sound, env, emu.new_obj('snd'), sndh, 1024)
            audio += bytes(emu.uc.mem_read(snd, 1024))
        t_emu += time.time() - t
        if i % 50 == 0:
            ws = emu.call(emu.export('NativeGameEngine_isInWaitScreen'), env, this) & 0xff
            nz = sum(1 for b in emu.uc.mem_read(fb, w * h * 4) if b)
            print('  frame %d: waitscreen=%d nonzero fb bytes=%d' % (i, ws, nz))
        if i in shots:
            save_frame(emu, fb, w, h, os.path.join(out, 'frame_%04d.png' % i))
        if not alive & 0xff:
            print('engine requested exit at frame', i)
            break
    with wave.open(os.path.join(out, 'audio.wav'), 'wb') as wf:
        wf.setnchannels(1)
        wf.setsampwidth(2)
        wf.setframerate(8000)
        wf.writeframes(bytes(audio))
    n = i + 1
    print('%d frames in %.1fs emulated (%.1f ms/frame, %.1f fps); %d API calls; heap %.1f MB'
          % (n, t_emu, 1000 * t_emu / n, n / t_emu, emu.api_calls, (emu.heap_top - HEAP_BASE) / 1e6))
    print('of which Python Win32/JNI shim: %.1fs -> pure emulated CPU %.1f ms/frame'
          % (emu.hook_time, 1000 * (t_emu - emu.hook_time) / n))
    print('java callbacks seen:', sorted(set(map(str, emu.java_calls)))[:20])
    print('unimplemented APIs:', sorted(emu.missing))
    print('hottest APIs:', sorted(emu.api_hist.items(), key=lambda kv: -kv[1])[:12])


if __name__ == '__main__':
    main()
