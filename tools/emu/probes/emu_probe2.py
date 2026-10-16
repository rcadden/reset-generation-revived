import os, sys
sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), '..'))
import rgemu
from unicorn import UC_HOOK_CODE
from unicorn.x86_const import UC_X86_REG_ECX, UC_X86_REG_EAX, UC_X86_REG_ESP
orig_init = rgemu.Emu.__init__
def init(self, *a, **k):
    orig_init(self, *a, **k)
    def enter(uc, addr, size, user):
        ecx = uc.reg_read(UC_X86_REG_ECX)
        mgr = self.r32(ecx + 0x24)
        print('  WaitPackage.enter id=%d mgr=%#x isAvail=%#x' % (self.r32(ecx + 0x2c), mgr, self.r32(self.r32(mgr) + 0xc)))
    def leave(uc, addr, size, user):
        print('  WaitPackage.exit id=%d' % self.r32(uc.reg_read(UC_X86_REG_ECX) + 0x2c))
    def avail_ret(uc, addr, size, user):
        print('  enter: isAvailable ->', uc.reg_read(UC_X86_REG_EAX) & 0xff)
    self.uc.hook_add(UC_HOOK_CODE, enter, begin=0x100440d0, end=0x100440d0)
    self.uc.hook_add(UC_HOOK_CODE, leave, begin=0x10043f90, end=0x10043f90)
rgemu.Emu.__init__ = init
sys.argv = ['rgemu.py'] + sys.argv[1:]
rgemu.main()
