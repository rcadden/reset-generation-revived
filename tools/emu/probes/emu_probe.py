import os, sys, runpy
sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), '..'))
import rgemu
from unicorn import UC_HOOK_CODE
from unicorn.x86_const import UC_X86_REG_ECX, UC_X86_REG_EAX
orig_init = rgemu.Emu.__init__
def init(self, *a, **k):
    orig_init(self, *a, **k)
    seen = {}
    def at_wait(uc, addr, size, user):
        ecx = uc.reg_read(UC_X86_REG_ECX)
        key = (self.r32(ecx + 0x2c), self.r32(ecx + 0x24))
        if key not in seen:
            seen[key] = 1
            mgr = key[1]; vt = self.r32(mgr); fn = self.r32(vt + 0xc)
            print('  wait-state update: package id', key[0], 'mgr', hex(mgr), 'isAvailable fn', hex(fn))
    def after_call(uc, addr, size, user):
        al = uc.reg_read(UC_X86_REG_EAX) & 0xff
        if not seen.get('r'):
            seen['r'] = 1
            print('  isAvailable returned', al)
    self.uc.hook_add(UC_HOOK_CODE, at_wait, begin=0x100441c0, end=0x100441c0)
    self.uc.hook_add(UC_HOOK_CODE, after_call, begin=0x100441d0, end=0x100441d0)
rgemu.Emu.__init__ = init
sys.argv = ['rgemu.py'] + sys.argv[1:]
rgemu.main()
