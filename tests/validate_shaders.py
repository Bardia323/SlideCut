from pathlib import Path
import ctypes,re
root=Path(__file__).resolve().parents[1]
s=(root/'src/main.cpp').read_text(encoding='utf-8')
s=s[s.index('static const char* PROJ_HLSL'):s.index('// 0/1 ping-pong')]
s=s.replace('#include "../surveillance_shader.h"',(root/'surveillance_shader.h').read_text())
s=''.join(m.group(2) for m in re.finditer(r'R"(\w+)\((.*?)\)\1"',s,re.S)).encode()
dll=ctypes.WinDLL('d3dcompiler_47')
fn=dll.D3DCompile
fn.argtypes=[ctypes.c_void_p,ctypes.c_size_t,ctypes.c_char_p,ctypes.c_void_p,ctypes.c_void_p,ctypes.c_char_p,ctypes.c_char_p,ctypes.c_uint,ctypes.c_uint,ctypes.POINTER(ctypes.c_void_p),ctypes.POINTER(ctypes.c_void_p)]
fn.restype=ctypes.c_long
for entry,target in [('VSFull','vs_4_0'),('PSProjector','ps_4_0'),('PSBlend','ps_4_0')]:
 out=ctypes.c_void_p();err=ctypes.c_void_p()
 hr=fn(s,len(s),b'projector.hlsl',None,None,entry.encode(),target.encode(),0,0,ctypes.byref(out),ctypes.byref(err))
 if err:
  v=ctypes.cast(err,ctypes.POINTER(ctypes.POINTER(ctypes.c_void_p))).contents
  get=ctypes.WINFUNCTYPE(ctypes.c_void_p,ctypes.c_void_p)(v[3])
  print(ctypes.string_at(get(err)).decode())
 print(entry,hr)
 if hr < 0: raise SystemExit(1)
