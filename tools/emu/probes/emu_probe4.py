import os, sys
sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), '..'))
import rgemu
orig_main_call = None
orig_init = rgemu.Emu.__init__
def init(self, *a, **k):
    orig_init(self, *a, **k)
    orig_call = self.call
    def call(addr, *args, **kw):
        r = orig_call(addr, *args, **kw)
        if addr == self.exports.get('_Java_rg_natives_NativeGameEngine_init@44'):
            app = self.r32(0x101b827c); vt = self.r32(app)
            print('  app=%#x vtable=%#x' % (app, vt), ['%#x' % self.r32(vt + 4 * i) for i in range(10)])
        return r
    self.call = call
rgemu.Emu.__init__ = init
sys.argv = ['rgemu.py'] + sys.argv[1:]
rgemu.main()
