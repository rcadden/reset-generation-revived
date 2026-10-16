import os, sys, time
sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), '..'))
import rgemu
from unicorn import UC_HOOK_BLOCK
st = {'n': 0, 'ins': 0, 'on': False}
orig_init = rgemu.Emu.__init__
def init(self, *a, **k):
    orig_init(self, *a, **k)
    sizes = {}
    def blk(uc, addr, size, user):
        if st['on']:
            st['bytes'] = st.get('bytes', 0) + size
            st['blocks'] = st.get('blocks', 0) + 1
    self.uc.hook_add(UC_HOOK_BLOCK, blk)
    orig_call = self.call
    def call(addr, *args, **kw):
        if addr == self.exports.get('_Java_rg_natives_NativeGameEngine_tick@8'):
            st['n'] += 1
            st['on'] = 200 < st['n'] <= 260
        return orig_call(addr, *args, **kw)
    self.call = call
rgemu.Emu.__init__ = init
sys.argv = ['rgemu.py'] + sys.argv[1:]
rgemu.main()
b = st.get('bytes', 0); print('frames 201-260: %.1f M code bytes executed, %.2f M blocks -> ~%.1f M instructions/frame (avg 3.5 B/insn)' % (b / 1e6, st.get('blocks', 0) / 1e6, b / 3.5 / 60 / 1e6))
