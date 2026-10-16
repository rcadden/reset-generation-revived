import os, sys, bisect, collections
sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), '..'))
import rgemu
from unicorn import UC_HOOK_BLOCK
funcs = sorted((int(a, 16), n) for a, n in (l.split() for l in open(os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', '..', '..', 'build', 'functions.txt'))))
addrs = [a for a, _ in funcs]
cov = collections.Counter()
state = {'on': False}
orig_init = rgemu.Emu.__init__
def init(self, *a, **k):
    orig_init(self, *a, **k)
    def blk(uc, addr, size, user):
        if state['on']:
            i = bisect.bisect_right(addrs, addr) - 1
            cov[funcs[i][1]] += 1
    self.uc.hook_add(UC_HOOK_BLOCK, blk, begin=0x10000000, end=0x10200000)
    orig_call = self.call
    def call(addr, *args, **kw):
        if addr == self.exports.get('_Java_rg_natives_NativeGameEngine_tick@8'):
            state['n'] = state.get('n', 0) + 1
            state['on'] = state['n'] > 50
        return orig_call(addr, *args, **kw)
    self.call = call
rgemu.Emu.__init__ = init
sys.argv = ['rgemu.py'] + sys.argv[1:]
rgemu.main()
for n, c in cov.most_common(45):
    print('%8d %s' % (c, n))
