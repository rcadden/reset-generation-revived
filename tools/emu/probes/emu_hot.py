import os, sys, collections
sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), '..'))
import rgemu, capstone
from unicorn import UC_HOOK_BLOCK
st = {'n': 0, 'on': False}
hist = collections.Counter()
orig_init = rgemu.Emu.__init__
def init(self, *a, **k):
    orig_init(self, *a, **k)
    def blk(uc, addr, size, user):
        if st['on']:
            hist[(addr, size)] += 1
    self.uc.hook_add(UC_HOOK_BLOCK, blk)
    orig_call = self.call
    def call(addr, *args, **kw):
        if addr == self.exports.get('_Java_rg_natives_NativeGameEngine_tick@8'):
            st['n'] += 1
            st['on'] = 240 < st['n'] <= 250
        return orig_call(addr, *args, **kw)
    self.call = call
    st['emu'] = self
rgemu.Emu.__init__ = init
sys.argv = ['rgemu.py'] + sys.argv[1:]
rgemu.main()
emu = st['emu']
md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_32)
tot = sum(c * s for (a, s), c in hist.items())
fpu = 0; sse = 0; allins = 0
for (a, s), c in hist.items():
    for i in md.disasm(bytes(emu.uc.mem_read(a, s)), a):
        allins += c
        if i.mnemonic.startswith('f'): fpu += c
        if i.mnemonic.startswith(('movss','movsd','addss','mulss','cvt','movaps','movq','pmov','padd','xmm')) or 'xmm' in i.op_str or 'mm' in i.op_str.split(',')[0][:3]: sse += c
print('instructions sampled %.1fM, x87 %.1f%%, SSE/MMX %.1f%%' % (allins / 1e6, 100 * fpu / allins, 100 * sse / allins))
for (a, s), c in hist.most_common(8):
    print('--- block %#x (rva %#x) x%d' % (a, a - 0x10000000, c))
    for i in list(md.disasm(bytes(emu.uc.mem_read(a, s)), a))[:12]:
        print('   ', i.mnemonic, i.op_str)
