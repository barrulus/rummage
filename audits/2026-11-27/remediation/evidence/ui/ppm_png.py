import pathlib,struct,zlib,sys
for filename in sys.argv[1:]:
 p=pathlib.Path(filename)
 with p.open('rb') as f:
  assert f.readline()==b'P6\n'
  w,h=map(int,f.readline().split());assert f.readline()==b'255\n';pixels=f.read()
 def chunk(t,b):return struct.pack('>I',len(b))+t+b+struct.pack('>I',zlib.crc32(t+b))
 rows=b''.join(b'\0'+pixels[y*w*3:(y+1)*w*3] for y in range(h))
 p.with_suffix('.png').write_bytes(b'\x89PNG\r\n\x1a\n'+chunk(b'IHDR',struct.pack('>IIBBBBB',w,h,8,2,0,0,0))+chunk(b'IDAT',zlib.compress(rows))+chunk(b'IEND',b''))
