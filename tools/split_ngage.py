"""Split an N-Gage 2.0 .n-gage install file (MIME multipart) into its parts."""
import sys, os, re
src, outdir = sys.argv[1], sys.argv[2]
data = open(src, 'rb').read()
os.makedirs(outdir, exist_ok=True)
parts = data.split(b'--KBoundary')
for i, p in enumerate(parts[1:]):
    if p.startswith(b'--'): break
    hdr, _, body = p.partition(b'\r\n\r\n')
    if body.endswith(b'\r\n'): body = body[:-2]
    m = re.search(rb'Content-ID: <([^>]+)>', hdr)
    name = (m.group(1).decode() if m else f'part{i}')
    enc = re.search(rb'Content-Transfer-Encoding: (\S+)', hdr)
    print(i, name, len(body), hdr.decode('latin1').strip().replace('\r\n',' | '), enc)
    ext = '.xml' if b'text/xml' in hdr else '.sis'
    open(os.path.join(outdir, name + ext), 'wb').write(body)
