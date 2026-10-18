"""Static recompiler: x86 engine DLL (as Ghidra P-code) -> portable C.

Input:  the P-code export written by tools/ghidra_scripts/ExportPcode.java.
Output: C sources for native/ (one C function per guest function entry, plus the
        address -> function dispatch table), compiled together with native/rt/.

Model (see docs/ANDROID_PLAN.md, "static recompilation"):
  * Guest memory is a flat 32-bit space at host pointer M (rt.h); guest addresses stay 32-bit.
  * General-purpose registers live in cpu.r[8]; each generated function copies the ones it
    uses into locals and spills them around calls/returns, so the C compiler can keep them in
    host registers. Flags (except DF) are function-local; MSVC code never keeps flags live
    across a call. x87, DF and other rare registers live in the cpu.rf register file.
  * A guest CALL pushes its return address like the original and calls the C function; RET
    pops it and returns. Indirect calls and jumps go through rt_call() (hash table lookup,
    or a host import/JNI stub).
  * A function's body is everything reachable from its entry without crossing a call, so code
    shared between functions (MSVC tail merging) is duplicated, and jumps into another known
    entry become tail calls.

Usage: py -I recomp.py <pcode.txt> <out dir> [--files N]
"""
import os
import re
import sys
from collections import defaultdict

GPR = ['eax', 'ecx', 'edx', 'ebx', 'esp', 'ebp', 'esi', 'edi']
DF_OFF = 0x20a
CT = {1: 'uint8_t', 2: 'uint16_t', 4: 'uint32_t', 8: 'uint64_t', 10: 'double'}
ST = {1: 'int8_t', 2: 'int16_t', 4: 'int32_t', 8: 'int64_t'}


class Instr:
    __slots__ = ('addr', 'length', 'text', 'ops', 'jumps')

    def __init__(self, addr, length, text):
        self.addr, self.length, self.text = addr, length, text
        self.ops = []
        self.jumps = []

    @property
    def mnem(self):
        return self.text.split(' ', 1)[0]

    @property
    def next(self):
        return self.addr + self.length


def parse_vn(s):
    if s == '-':
        return None
    if s.startswith('o:'):
        return ('o', s[2:], 0)
    sp, off, size = s.split(':')
    return (sp, int(off, 16), int(size))


def load(path):
    funcs = {}
    instrs = {}
    cur = None
    with open(path, encoding='utf-8', errors='replace') as f:
        for line in f:
            k = line[0]
            if k == 'P':
                parts = line.split()
                cur.ops.append((parts[1], parse_vn(parts[2]), [parse_vn(x) for x in parts[3:]]))
            elif k == 'I':
                _, a, n, _hex, text = line.rstrip('\n').split(' ', 4)
                cur = Instr(int(a, 16), int(n), text)
                instrs[cur.addr] = cur
            elif k == 'J':
                cur.jumps.append(int(line.split()[1], 16))
            elif k == 'F':
                _, a, name = line.rstrip('\n').split(' ', 2)
                funcs[int(a, 16)] = name
    for ins in instrs.values():
        if ins.mnem in ('BSR', 'BSF'):
            fix_bitscan(ins)
    apply_patches(instrs)
    return funcs, instrs


# Layout patches for the 640x360 native build (the engine is laid out for 240x320). The tutorial /
# story dialogue screen (FUN_100b8360 builds it, FUN_100b8570 draws it) places its speech box
# (centre x 0x78 = 120), its text (x 0x4b) and the portrait (x = slide offset) for a 240-wide screen,
# so on 640 they hug the left edge. Shifting x by (640 - 240) / 2 centres them on the screen.
DIALOG_DX = 200
PATCHES = {
    0x100b85fa: ('const', 0x78, 0x78 + DIALOG_DX),    # ADD EBP,0x78: speech box centre x
    0x100b875e: ('const', 0x4b, 0x4b + DIALOG_DX),    # ADD EBP,0x4b: text x
    0x100b8808: ('addreg', 8, DIALOG_DX),             # MOV EDX,[ESP+0x10]: portrait x (EDX += dx)
}


def apply_patches(instrs):
    for addr, p in PATCHES.items():
        ins = instrs.get(addr)
        if ins is None:
            raise SystemExit('patch site %x not found' % addr)
        if p[0] == 'const':
            n = 0
            for opc, o, a in ins.ops:
                for i, v in enumerate(a):
                    if v[0] == 'c' and v[1] == p[1]:
                        a[i] = ('c', p[2], v[2])
                        n += 1
            if not n:
                raise SystemExit('patch site %x: constant %#x not found' % (addr, p[1]))
        elif p[0] == 'addreg':
            ins.ops.append(('INT_ADD', ('r', p[1], 4), [('r', p[1], 4), ('c', p[2], 4)]))


def fix_bitscan(ins):
    """Ghidra's BSR/BSF model writes 31/0 to the destination for a zero source; hardware leaves
    it unchanged (compilers rely on it: MOV EAX,-1 / BSR EAX,ECX). Make the zero-source branch
    skip the final destination write."""
    last = len(ins.ops) - 1
    if last < 1 or ins.ops[last][0] != 'COPY':
        return
    for i, (opc, o, a) in enumerate(ins.ops):
        if opc == 'CBRANCH' and a[0][0] == 'c' and i + rel_const(a[0]) == last:
            a[0] = ('c', a[0][1] + 1, a[0][2])
            return


class Unsupported(Exception):
    pass


def rel_const(t):
    """Signed value of a constant varnode (relative P-code branch offsets)."""
    bits = 8 * t[2]
    v = t[1] & ((1 << bits) - 1)
    return v - (1 << bits) if v >> (bits - 1) else v


class FuncGen:
    """Generates the C body of one function."""

    def __init__(self, rc, entry):
        self.rc = rc
        self.entry = entry
        self.body = []                  # instruction addresses, sorted
        self.temps = {}                 # name -> ctype
        self.flags = set()
        self.gprs = set()
        self.targets = set()            # instruction addresses that need labels
        self.lines = []

    # ------------------------------------------------------------------ CFG
    def discover(self):
        rc = self.rc
        seen = set()
        work = [self.entry]
        while work:
            a = work.pop()
            if a in seen:
                continue
            ins = rc.instrs.get(a)
            if ins is None:
                continue
            seen.add(a)
            falls = True
            for opc, out, ins_ in ins.ops:
                if opc in ('BRANCH', 'CBRANCH'):
                    t = ins_[0]
                    if t[0] == 'm':
                        if t[1] in rc.entries and t[1] != self.entry:
                            pass                                     # tail call
                        elif t[1] in rc.instrs:
                            work.append(t[1])
                            self.targets.add(t[1])
                    if opc == 'BRANCH' and t[0] == 'm':
                        falls = False
                elif opc == 'BRANCHIND':
                    for j in ins.jumps:
                        if j in rc.instrs and not (j in rc.entries and j != self.entry):
                            work.append(j)
                            self.targets.add(j)
                    falls = False
                elif opc == 'RETURN':
                    falls = False
                elif opc == 'CALL':
                    if ins_[0][1] in rc.noreturn:
                        falls = False
            # A relative BRANCH inside the instruction (rep prefixes, CPUID) still falls through.
            if falls:
                work.append(ins.next)
                if ins.next not in rc.instrs:
                    pass
        self.body = sorted(seen)
        self.targets.add(self.entry)

    # ------------------------------------------------------------------ varnodes
    def temp(self, off, size):
        if size not in CT:
            raise Unsupported('temp size %d' % size)
        name = 'u%x_%d' % (off, size)
        self.temps[name] = CT[size]
        return name

    def rd(self, v):
        sp, off, size = v
        if sp == 'c':
            if size == 10:
                raise Unsupported('float const')
            mask = (1 << (8 * size)) - 1
            val = off & mask
            if size == 8:
                return '0x%xull' % val
            if size == 4:
                return '0x%xu' % val
            return '((%s)0x%xu)' % (CT[size], val)
        if sp == 'u':
            return self.temp(off, size)
        if sp == 'm':
            return self.ld(size, '0x%xu' % off)
        if sp == 'r':
            if off < 0x20:
                g = GPR[off >> 2]
                self.gprs.add(g)
                sub = off & 3
                if sub == 0 and size == 4:
                    return g
                if sub == 0 and size == 2:
                    return '((uint16_t)%s)' % g
                if sub == 0 and size == 1:
                    return '((uint8_t)%s)' % g
                if sub == 1 and size == 1:
                    return '((uint8_t)(%s >> 8))' % g
                raise Unsupported('gpr sub %x:%d' % (off, size))
            if 0x200 <= off < 0x220 and size == 1 and off != DF_OFF:
                n = 'f%x' % off
                self.flags.add(n)
                return n
            return self.rf(off, size)
        raise Unsupported('space ' + sp)

    def rf(self, off, size):
        if size == 10:
            return 'RFD(0x%x)' % off
        if size not in CT:
            raise Unsupported('register size %d' % size)
        return 'RF%d(0x%x)' % (size * 8, off)

    def ld(self, size, addr):
        if size == 10:
            return 'LDX(%s)' % addr
        if size not in CT:
            raise Unsupported('load size %d' % size)
        return 'LD%d(%s)' % (size * 8, addr)

    def wr(self, v, expr):
        sp, off, size = v
        if sp == 'u':
            return '%s = %s;' % (self.temp(off, size), expr)
        if sp == 'm':
            return self.st(size, '0x%xu' % off, expr)
        if sp == 'r':
            if off < 0x20:
                g = GPR[off >> 2]
                self.gprs.add(g)
                sub = off & 3
                if sub == 0 and size == 4:
                    return '%s = %s;' % (g, expr)
                if sub == 0 and size == 2:
                    return '%s = (%s & 0xffff0000u) | (uint16_t)(%s);' % (g, g, expr)
                if sub == 0 and size == 1:
                    return '%s = (%s & 0xffffff00u) | (uint8_t)(%s);' % (g, g, expr)
                if sub == 1 and size == 1:
                    return '%s = (%s & 0xffff00ffu) | ((uint32_t)(uint8_t)(%s) << 8);' % (g, g, expr)
                raise Unsupported('gpr sub write %x:%d' % (off, size))
            if 0x200 <= off < 0x220 and size == 1 and off != DF_OFF:
                n = 'f%x' % off
                self.flags.add(n)
                return '%s = %s;' % (n, expr)
            if size == 10:
                return 'RFD(0x%x) = %s;' % (off, expr)
            if size not in CT:
                raise Unsupported('register write size %d' % size)
            return 'RF%d(0x%x) = %s;' % (size * 8, off, expr)
        raise Unsupported('write space ' + sp)

    def st(self, size, addr, expr):
        if size == 10:
            return 'STX(%s, %s);' % (addr, expr)
        if size not in CT:
            raise Unsupported('store size %d' % size)
        return 'ST%d(%s, %s);' % (size * 8, addr, expr)

    # floats: size-4/8 varnodes hold IEEE bit patterns; size-10 values are host doubles.
    def fval(self, v):
        e = self.rd(v)
        if v[2] == 4:
            return '((double)F32(%s))' % e
        if v[2] == 8:
            return 'F64(%s)' % e
        if v[2] == 10:
            return e
        raise Unsupported('float size %d' % v[2])

    def fout(self, size, dexpr):
        if size == 4:
            return 'B32((float)(%s))' % dexpr
        if size == 8:
            return 'B64(%s)' % dexpr
        if size == 10:
            return '(%s)' % dexpr
        raise Unsupported('float out size %d' % size)

    # ------------------------------------------------------------------ code
    def spill(self):
        return ' '.join('cpu.r[%d] = %s;' % (GPR.index(g), g) for g in GPR if g in self.gprs)

    def reload(self):
        return ' '.join('%s = cpu.r[%d];' % (g, GPR.index(g)) for g in GPR if g in self.gprs)

    def call_direct(self, target, ins):
        rc = self.rc
        if target in rc.entries:
            return '%s %s(); %s' % (self.SPILL, rc.fname(target), self.RELOAD)
        return '%s rt_call(0x%xu); %s' % (self.SPILL, target, self.RELOAD)

    def gen_op(self, ins, idx, opc, out, a):
        L = 'L_%x' % ins.addr
        if opc == 'COPY':
            return self.wr(out, self.rd(a[0]))
        if opc == 'LOAD':
            return self.wr(out, self.ld(out[2], self.rd(a[1])))
        if opc == 'STORE':
            return self.st(a[2][2], self.rd(a[1]), self.rd(a[2]))
        n = out[2] if out else 0
        if opc in ('INT_ADD', 'INT_SUB', 'INT_MULT', 'INT_AND', 'INT_OR', 'INT_XOR'):
            sym = {'INT_ADD': '+', 'INT_SUB': '-', 'INT_MULT': '*', 'INT_AND': '&', 'INT_OR': '|', 'INT_XOR': '^'}[opc]
            x, y = self.rd(a[0]), self.rd(a[1])
            if n >= 4:                       # uint32_t/uint64_t operands: no promotion issues
                return self.wr(out, '%s %s %s' % (x, sym, y))
            if opc == 'INT_MULT':
                return self.wr(out, '(%s)((uint32_t)%s * %s)' % (CT[n], x, y))
            return self.wr(out, '(%s)(%s %s %s)' % (CT[n], x, sym, y))
        if opc == 'INT_2COMP':
            w = 'uint64_t' if n == 8 else 'uint32_t'
            return self.wr(out, '(%s)(0u - (%s)%s)' % (CT[n], w, self.rd(a[0])))
        if opc == 'INT_NEGATE':
            w = 'uint64_t' if n == 8 else 'uint32_t'
            return self.wr(out, '(%s)(~(%s)%s)' % (CT[n], w, self.rd(a[0])))
        if opc in ('INT_LEFT', 'INT_RIGHT', 'INT_SRIGHT'):
            bits = 8 * n
            x, s = self.rd(a[0]), '(uint64_t)' + self.rd(a[1])
            w = 'uint64_t' if n == 8 else 'uint32_t'
            if opc == 'INT_LEFT':
                e = '(%s >= %d ? 0 : (%s)((%s)%s << %s))' % (s, bits, CT[n], w, x, s)
            elif opc == 'INT_RIGHT':
                e = '(%s >= %d ? 0 : (%s)(%s >> %s))' % (s, bits, CT[n], x, s)
            else:
                e = '(%s)((%s)%s >> (%s >= %d ? %d : %s))' % (CT[n], ST[n], x, s, bits, bits - 1, s)
            return self.wr(out, e)
        if opc in ('INT_EQUAL', 'INT_NOTEQUAL', 'INT_LESS', 'INT_LESSEQUAL'):
            sym = {'INT_EQUAL': '==', 'INT_NOTEQUAL': '!=', 'INT_LESS': '<', 'INT_LESSEQUAL': '<='}[opc]
            return self.wr(out, '(uint8_t)(%s %s %s)' % (self.rd(a[0]), sym, self.rd(a[1])))
        if opc in ('INT_SLESS', 'INT_SLESSEQUAL'):
            m = a[0][2]
            sym = '<' if opc == 'INT_SLESS' else '<='
            return self.wr(out, '(uint8_t)((%s)%s %s (%s)%s)' % (ST[m], self.rd(a[0]), sym, ST[m], self.rd(a[1])))
        if opc == 'INT_ZEXT':
            return self.wr(out, '(%s)%s' % (CT[n], self.rd(a[0])))
        if opc == 'INT_SEXT':
            return self.wr(out, '(%s)(%s)(%s)%s' % (CT[n], ST[n], ST[a[0][2]], self.rd(a[0])))
        if opc in ('INT_CARRY', 'INT_SCARRY', 'INT_SBORROW'):
            m = a[0][2]
            return self.wr(out, '%s%d(%s, %s)' % ({'INT_CARRY': 'CARRY', 'INT_SCARRY': 'SCARRY', 'INT_SBORROW': 'SBORROW'}[opc],
                                                 m * 8, self.rd(a[0]), self.rd(a[1])))
        if opc in ('INT_DIV', 'INT_REM'):
            sym = '/' if opc == 'INT_DIV' else '%'
            return self.wr(out, '(%s)(%s %s RT_NZ(%s))' % (CT[n], self.rd(a[0]), sym, self.rd(a[1])))
        if opc in ('INT_SDIV', 'INT_SREM'):
            sym = '/' if opc == 'INT_SDIV' else '%'
            m = a[0][2]
            return self.wr(out, '(%s)((%s)%s %s (%s)RT_NZ(%s))' % (CT[n], ST[m], self.rd(a[0]), sym, ST[m], self.rd(a[1])))
        if opc == 'BOOL_NEGATE':
            return self.wr(out, '(uint8_t)(!%s)' % self.rd(a[0]))
        if opc in ('BOOL_AND', 'BOOL_OR', 'BOOL_XOR'):
            sym = {'BOOL_AND': '&', 'BOOL_OR': '|', 'BOOL_XOR': '^'}[opc]
            return self.wr(out, '(uint8_t)(%s %s %s)' % (self.rd(a[0]), sym, self.rd(a[1])))
        if opc == 'POPCOUNT':
            return self.wr(out, '(%s)__builtin_popcountll((uint64_t)%s)' % (CT[n], self.rd(a[0])))
        if opc == 'LZCOUNT':
            m = a[0][2]
            return self.wr(out, '(%s)LZCNT%d(%s)' % (CT[n], m * 8, self.rd(a[0])))
        if opc == 'PIECE':
            lo_bits = 8 * a[1][2]
            return self.wr(out, '(%s)(((%s)%s << %d) | (%s)%s)' % (CT[n], CT[n], self.rd(a[0]), lo_bits, CT[n], self.rd(a[1])))
        if opc == 'SUBPIECE':
            if a[0][2] in (10, 16):
                raise Unsupported('subpiece of float/vector')
            sh = 8 * a[1][1]
            src = self.rd(a[0])
            return self.wr(out, '(%s)(%s >> %d)' % (CT[n], src, sh) if sh else '(%s)%s' % (CT[n], src))
        if opc.startswith('FLOAT') or opc in ('INT2FLOAT', 'TRUNC', 'ROUND', 'FLOOR', 'CEIL'):
            return self.gen_float(opc, out, a)
        if opc == 'BRANCH':
            t = a[0]
            if t[0] == 'c':
                return 'goto %s_%d;' % (L, idx + rel_const(t))
            return self.jump(t[1])
        if opc == 'CBRANCH':
            t = a[0]
            if t[0] == 'c':
                return 'if (%s) goto %s_%d;' % (self.rd(a[1]), L, idx + rel_const(t))
            return 'if (%s) { %s }' % (self.rd(a[1]), self.jump(t[1]))
        if opc == 'BRANCHIND':
            tgt = self.rd(a[0])
            local = sorted(set(j for j in ins.jumps if j in self.body_set))
            if local:
                cases = ' '.join('case 0x%xu: goto L_%x;' % (j, j) for j in local)
                return 'switch (%s) { %s default: %s rt_call(%s); return; }' % (tgt, cases, self.SPILL, tgt)
            return '%s rt_call(%s); return;' % (self.SPILL, tgt)
        if opc == 'CALL':
            t = a[0][1]
            if t == ins.next:
                return '/* call next: get-EIP idiom */'
            return self.call_direct(t, ins)
        if opc == 'CALLIND':
            return '%s rt_call(%s); %s' % (self.SPILL, self.rd(a[0]), self.RELOAD)
        if opc == 'RETURN':
            if self.entry in self.rc.trace:
                self.gprs.add('eax')
                return 'rt_trace_ret(0x%xu, eax); %s return;' % (self.entry, self.SPILL)
            return '%s return;' % self.SPILL
        if opc == 'CALLOTHER':
            name = a[0][1]
            if name in ('LOCK', 'UNLOCK'):
                return ''
            if name in ('fsin', 'fcos', 'fsqrt'):
                fn = {'fsin': 'sin', 'fcos': 'cos', 'fsqrt': 'sqrt'}[name]
                return self.wr(out, self.fout(out[2], '%s(%s)' % (fn, self.fval(a[1]))))
            raise Unsupported('callother ' + name)
        raise Unsupported(opc)

    def gen_float(self, opc, out, a):
        n = out[2]
        bin_ = {'FLOAT_ADD': '+', 'FLOAT_SUB': '-', 'FLOAT_MULT': '*', 'FLOAT_DIV': '/'}
        cmp_ = {'FLOAT_EQUAL': '==', 'FLOAT_NOTEQUAL': '!=', 'FLOAT_LESS': '<', 'FLOAT_LESSEQUAL': '<='}
        if opc in bin_:
            return self.wr(out, self.fout(n, '%s %s %s' % (self.fval(a[0]), bin_[opc], self.fval(a[1]))))
        if opc in cmp_:
            return self.wr(out, '(uint8_t)(%s %s %s)' % (self.fval(a[0]), cmp_[opc], self.fval(a[1])))
        if opc == 'FLOAT_NEG':
            return self.wr(out, self.fout(n, '-%s' % self.fval(a[0])))
        if opc == 'FLOAT_ABS':
            return self.wr(out, self.fout(n, 'fabs(%s)' % self.fval(a[0])))
        if opc == 'FLOAT_SQRT':
            return self.wr(out, self.fout(n, 'sqrt(%s)' % self.fval(a[0])))
        if opc == 'FLOAT_NAN':
            return self.wr(out, '(uint8_t)isnan(%s)' % self.fval(a[0]))
        if opc == 'INT2FLOAT':
            m = a[0][2]
            return self.wr(out, self.fout(n, '(double)(%s)%s' % (ST[m], self.rd(a[0]))))
        if opc == 'FLOAT2FLOAT':
            return self.wr(out, self.fout(n, self.fval(a[0])))
        if opc == 'TRUNC':
            return self.wr(out, '(%s)F2I%d(%s)' % (CT[n], n * 8, self.fval(a[0])))
        if opc in ('ROUND', 'FLOOR', 'CEIL'):
            fn = {'ROUND': 'rint', 'FLOOR': 'floor', 'CEIL': 'ceil'}[opc]
            return self.wr(out, self.fout(n, '%s(%s)' % (fn, self.fval(a[0]))))
        raise Unsupported(opc)

    def fallthrough(self, ins):
        """Address control reaches after this instruction, or None if it never falls through."""
        last = ins.ops[-1] if ins.ops else None
        if last is not None:
            if last[0] in ('RETURN', 'BRANCHIND') or (last[0] == 'BRANCH' and last[2][0][0] == 'm'):
                return None
            if last[0] == 'CALL' and last[2][0][1] in self.rc.noreturn:
                return None
        return ins.next

    def jump(self, target):
        if target in self.body_set and not (target in self.rc.entries and target != self.entry):
            return 'goto L_%x;' % target
        if target in self.rc.entries or target in self.rc.instrs:
            return '%s %s; return;' % (self.SPILL, '%s()' % self.rc.fname(target) if target in self.rc.entries else 'rt_call(0x%xu)' % target)
        return 'rt_bad_branch(0x%xu, 0x%xu);' % (self.entry, target)

    def special(self, ins):
        """Instructions emitted as a single runtime call instead of their P-code."""
        t = ins.text
        if t.startswith('MOVS') and '.REP ' in t:
            self.gprs.update(('ecx', 'esi', 'edi'))
            return 'RT_REP_MOVS(%d);' % {'B': 1, 'W': 2, 'D': 4}[t[4]]
        if t.startswith('STOS') and '.REP ' in t:
            self.gprs.update(('eax', 'ecx', 'edi'))
            return 'RT_REP_STOS(%d);' % {'B': 1, 'W': 2, 'D': 4}[t[4]]
        if ins.mnem == 'CPUID':
            self.gprs.update(('eax', 'ebx', 'ecx', 'edx'))
            return 'RT_CPUID();'
        return None

    def generate(self):
        rc = self.rc
        self.discover()
        self.body_set = set(self.body)
        self.SPILL = '@SPILL@'
        self.RELOAD = '@RELOAD@'
        # Pre-pass: fallthroughs that are not to the next emitted instruction need labels.
        for k, a in enumerate(self.body):
            nxt = self.fallthrough(rc.instrs[a])
            if nxt is not None and (k + 1 >= len(self.body) or self.body[k + 1] != nxt):
                self.targets.add(nxt)
        out = []
        prev_next = None
        for a in self.body:
            ins = rc.instrs[a]
            if prev_next is not None and prev_next != a:
                if prev_next in self.body_set:
                    out.append('    goto L_%x;' % prev_next)
                elif prev_next in rc.instrs or prev_next in rc.entries:
                    out.append('    %s' % self.jump(prev_next))
                else:
                    out.append('    rt_bad_branch(0x%xu, 0x%xu);' % (self.entry, prev_next))
            if a in self.targets:
                out.append('L_%x: /* %s */' % (a, ins.text.replace('*/', '* /')))
            else:
                out.append('    /* %x %s */' % (a, ins.text.replace('*/', '* /')))
            sp = self.special(ins)
            if sp:
                out.append('    ' + sp)
            else:
                try:
                    need = set()
                    for i, (opc, o, ins_) in enumerate(ins.ops):
                        if opc in ('BRANCH', 'CBRANCH') and ins_[0][0] == 'c':
                            need.add(i + rel_const(ins_[0]))
                    for i, (opc, o, ins_) in enumerate(ins.ops):
                        code = self.gen_op(ins, i, opc, o, ins_)
                        out.append(('L_%x_%d: %s' % (a, i, code)) if i in need else '    ' + code)
                    if len(ins.ops) in need:
                        out.append('L_%x_%d: ;' % (a, len(ins.ops)))
                except (Unsupported, KeyError) as e:
                    rc.unsupported[str(e)] += 1
                    out.append('    rt_unimpl(0x%xu, "%s");' % (a, str(e)))
            prev_next = self.fallthrough(ins)
        if prev_next is not None:
            if prev_next in rc.instrs or prev_next in rc.entries:
                out.append('    ' + self.jump(prev_next))
            else:
                out.append('    rt_bad_branch(0x%xu, 0x%xu);' % (self.entry, prev_next))
        spill, reload = self.spill(), self.reload()
        head = ['void %s(void) {' % rc.fname(self.entry)]
        for g in GPR:
            if g in self.gprs:
                head.append('    uint32_t %s = cpu.r[%d];' % (g, GPR.index(g)))
        for fl in sorted(self.flags):
            head.append('    uint8_t %s = 0;' % fl)
        for name, ty in sorted(self.temps.items()):
            head.append('    %s %s;' % (ty, name))
        head.append('    RT_ENTER(0x%xu);' % self.entry)
        if self.entry in rc.trace:
            head.append('    rt_trace(0x%xu);' % self.entry)
        if self.entry in rc.hooks:
            head.append('    rt_hook(0x%xu);' % self.entry)
        if self.body and self.body[0] != self.entry:
            head.append('    goto L_%x;' % self.entry)
        text = '\n'.join(head + out + ['}', ''])
        text = text.replace(' return;', ' RT_LEAVE(); return;')
        return text.replace('@SPILL@', spill).replace('@RELOAD@', reload)


# Function entries that call rt_hook() (native/rt/rt.c): the host can intercept them (game
# server mode: native/rt/server.c). Inert unless a handler is registered.
DEFAULT_HOOKS = {0x1008fa80, 0x1008f500,      # main menu flow: event handler, constructor
                 0x100d5350}                    # block placement tool activated (rt: hook_tile_tool)


class Recompiler:
    def __init__(self, funcs, instrs):
        self.funcs = funcs
        self.instrs = instrs
        self.entries = set(a for a in funcs if a in instrs)
        # direct call targets Ghidra did not turn into functions
        for ins in instrs.values():
            for opc, out, a in ins.ops:
                if opc == 'CALL' and a[0][0] == 'm' and a[0][1] in instrs and a[0][1] != ins.next:
                    self.entries.add(a[0][1])
        self.noreturn = set()
        self.unsupported = defaultdict(int)
        self.trace = set()
        self.hooks = set()

    def fname(self, a):
        return 'f_%08x' % a

    def add_code_immediates(self):
        """Code addresses used as immediates (PUSH offset f / MOV [x], offset f: callbacks,
        constructors stored at run time) become entries. Branch and call targets are not
        immediates in P-code (they are 'm' varnodes), so this does not pick up plain labels."""
        added = 0
        for ins in self.instrs.values():
            # only data moves: CALL pushes its return address as a constant, and x87
            # instructions record their own address (FPU instruction pointer)
            if ins.mnem not in ('PUSH', 'MOV'):
                continue
            for opc, out, ins_ in ins.ops:
                for v in ins_:
                    if v and v[0] == 'c' and v[2] == 4 and v[1] in self.instrs and v[1] not in self.entries                             and v[1] not in self.jump_targets:
                        self.entries.add(v[1])
                        added += 1
        return added

    def resolve_jump_tables(self, image):
        """Indirect jumps Ghidra left unresolved: read 'JMP [reg*4 + table]' tables from the image."""
        base, data, sections = image
        def rd32(va):
            for name, sva, raw in sections:
                if sva <= va < sva + len(raw) - 3:
                    return int.from_bytes(raw[va - sva:va - sva + 4], 'little')
            return None
        fixed = 0
        for ins in self.instrs.values():
            if not any(op[0] == 'BRANCHIND' for op in ins.ops):
                continue
            m = re.search(r'\[E[A-Z]{2}\*0x4 \+ 0x([0-9a-f]+)\]', ins.text)
            if not m:
                continue
            table = int(m.group(1), 16)
            for k in range(512):
                v = rd32(table + 4 * k)
                if v is None or v not in self.instrs or abs(v - ins.addr) > 0x10000:
                    break
                if v not in ins.jumps:
                    ins.jumps.append(v)
            fixed += bool(ins.jumps)
        print('jump tables resolved from the image:', fixed)

    def add_data_pointers(self, image):
        """Code addresses stored in data (vtables, callbacks) become entries too."""
        base, data, sections = image
        added = 0
        for name, va, raw in sections:
            if name in (b'.text', b'.reloc'):
                continue
            for off in range(0, len(raw) - 3, 4):
                v = int.from_bytes(raw[off:off + 4], 'little')
                if v in self.instrs and v not in self.entries and v not in self.jump_targets:
                    self.entries.add(v)
                    added += 1
        return added


def read_pe(path):
    import struct
    d = open(path, 'rb').read()
    pe = struct.unpack_from('<I', d, 0x3c)[0]
    nsec = struct.unpack_from('<H', d, pe + 6)[0]
    optsz = struct.unpack_from('<H', d, pe + 20)[0]
    base = struct.unpack_from('<I', d, pe + 24 + 28)[0]
    secs = []
    for i in range(nsec):
        o = pe + 24 + optsz + 40 * i
        name = d[o:o + 8].rstrip(b'\0')
        vsize, va, rsize, rptr = struct.unpack_from('<IIII', d, o + 8)
        secs.append((name, base + va, d[rptr:rptr + min(vsize, rsize)]))
    return base, d, secs


def write_if_changed(path, text):
    """Keeps the timestamp of unchanged outputs so builds only recompile what changed."""
    try:
        with open(path, encoding='utf-8', newline='') as f:
            if f.read() == text:
                return
    except OSError:
        pass
    with open(path, 'w', encoding='utf-8', newline='\n') as f:
        f.write(text)


def main():
    src, outdir = sys.argv[1], sys.argv[2]
    nfiles = int(sys.argv[sys.argv.index('--files') + 1]) if '--files' in sys.argv else 48
    dll = sys.argv[sys.argv.index('--dll') + 1] if '--dll' in sys.argv else None
    funcs, instrs = load(src)
    rc = Recompiler(funcs, instrs)
    if dll:
        rc.resolve_jump_tables(read_pe(dll))
    rc.jump_targets = set(j for i in instrs.values() for j in i.jumps)
    if '--trace' in sys.argv:
        rc.trace = set(int(x, 16) for x in sys.argv[sys.argv.index('--trace') + 1].split(',') if x)
    rc.hooks = set(DEFAULT_HOOKS)
    if '--hook' in sys.argv:
        rc.hooks |= set(int(x, 16) for x in sys.argv[sys.argv.index('--hook') + 1].split(',') if x)
    if dll:
        print('data-pointer entries added:', rc.add_data_pointers(read_pe(dll)))
    print('code-immediate entries added:', rc.add_code_immediates())
    entries = sorted(rc.entries)
    print('%d instructions, %d Ghidra functions, %d entries' % (len(instrs), len(funcs), len(entries)))
    os.makedirs(outdir, exist_ok=True)
    chunks = [entries[i::nfiles] for i in range(nfiles)]
    total = 0
    for k, chunk in enumerate(chunks):
        parts = ['/* generated by tools/recomp/recomp.py - do not edit */', '#include "rt.h"', '#include "rg_gen.h"', '']
        for e in chunk:
            g = FuncGen(rc, e)
            code = g.generate()
            total += len(g.body)
            parts.append(code)
        write_if_changed(os.path.join(outdir, 'rg_gen_%02d.c' % k), '\n'.join(parts))
    write_if_changed(os.path.join(outdir, 'rg_gen.h'), '/* generated - do not edit */\n#pragma once\n'
                     + ''.join('void %s(void);\n' % rc.fname(e) for e in entries))
    write_if_changed(os.path.join(outdir, 'rg_gen_table.c'),
                     '/* generated - do not edit */\n#include "rt.h"\n#include "rg_gen.h"\n'
                     'const rt_func_entry rt_func_table[] = {\n'
                     + ''.join('    {0x%xu, %s},\n' % (e, rc.fname(e)) for e in entries)
                     + '};\nconst unsigned rt_func_count = %d;\n' % len(entries))
    print('emitted %d instructions (%.2fx duplication) into %d files' % (total, total / max(1, len(instrs)), nfiles))
    if rc.unsupported:
        print('unsupported:', dict(rc.unsupported))


if __name__ == '__main__':
    main()
