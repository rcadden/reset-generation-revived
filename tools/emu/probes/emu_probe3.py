import os, sys
sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), '..'))
import rgemu
from unicorn import UC_HOOK_CODE
from unicorn.x86_const import UC_X86_REG_ECX
orig_init = rgemu.Emu.__init__
def init(self, *a, **k):
    orig_init(self, *a, **k)
    n = {'c': 0}
    def upd(uc, addr, size, user):
        esi = uc.reg_read(UC_X86_REG_ECX)
        n['c'] += 1
        if n['c'] <= 12 or n['c'] % 50 == 0:
            print('  pkgmgr.update this=%#x idx=%d ready=%s avail=%s' % (esi, self.r32(esi + 8),
                  bytes(uc.mem_read(self.r32(esi + 0x10), 9)).hex(), bytes(uc.mem_read(self.r32(esi + 0x1c), 9)).hex()))
    self.uc.hook_add(UC_HOOK_CODE, upd, begin=0x100035d0, end=0x100035d0)
rgemu.Emu.__init__ = init
sys.argv = ['rgemu.py'] + sys.argv[1:]
rgemu.main()
