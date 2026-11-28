import sys,ctypes,json,os,time
lib=ctypes.CDLL('/tmp/rummage-remediation-build/librummage-photos.so')
lib.rummage_photos_request.argtypes=[ctypes.c_char_p,ctypes.c_char_p]
lib.rummage_photos_request.restype=ctypes.c_void_p
lib.rummage_photos_free.argtypes=[ctypes.c_void_p]
def call(req,check=True):
 t=time.monotonic(); p=lib.rummage_photos_request(os.fsencode(sys.argv[1]),json.dumps(req).encode())
 try: result=json.loads(ctypes.string_at(p))
 finally: lib.rummage_photos_free(p)
 if check and not result.get('ok'): raise RuntimeError(result)
 return result
if len(sys.argv)>2: print(json.dumps(call(json.loads(sys.argv[2]),False)))
