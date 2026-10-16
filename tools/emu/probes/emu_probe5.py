import os, sys
sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), '..'))
import rgemu
from unicorn import UC_HOOK_CODE
from unicorn.x86_const import UC_X86_REG_ECX, UC_X86_REG_ESP
orig_init = rgemu.Emu.__init__
cnt = {}
def init(self, *a, **k):
    orig_init(self, *a, **k)
    def h(name):
        def f(uc, addr, size, user):
            cnt[name] = cnt.get(name, 0) + 1
            if cnt[name] <= 2:
                ecx = uc.reg_read(UC_X86_REG_ECX)
                print('  hit %s ecx=%#x ret=%#x' % (name, ecx, self.r32(uc.reg_read(UC_X86_REG_ESP))))
        return f
    for a, n in [(0x100448c0, 'stack.draw'), (0x100441c0, 'wait.draw'), (0x10008940, 'app.update'), (0x10044c90, 'stack.update')]:
        self.uc.hook_add(UC_HOOK_CODE, h(n), begin=a, end=a)
    orig_call = self.call
    def call(addr, *args, **kw):
        r = orig_call(addr, *args, **kw)
        if addr == self.exports.get('_Java_rg_natives_NativeGameEngine_init@44'):
            g = self.r32(0x101b828c)
            for off in (0x28, 0x60):
                o = self.r32(g + off); print('  global+%#x = %#x vt=%#x' % (off, o, self.r32(o) if o else 0), ['%#x' % self.r32(self.r32(o) + 4 * i) for i in range(8)] if o else '')
        return r
    self.call = call
rgemu.Emu.__init__ = init
sys.argv = ['rgemu.py'] + sys.argv[1:]
rgemu.main()
print(cnt)
