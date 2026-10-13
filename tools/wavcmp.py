#!/usr/bin/env python3
"""Compare two WAV files: lengths, overall SNR and per-second SNR.
usage: wavcmp.py reference.wav test.wav"""
import struct, sys, math, array
def load(p):
    d=open(p,'rb').read(); i=12; fmt=None
    while i < len(d):
        cid=d[i:i+4]; sz=struct.unpack('<I',d[i+4:i+8])[0]; body=d[i+8:i+8+sz]
        if cid==b'fmt ': fmt=struct.unpack('<HHIIHH',body[:16])
        if cid==b'data': data=body
        i+=8+sz+(sz&1)
    tag,ch,rate,_,_,bits=fmt
    if bits==16: a=array.array('h'); a.frombytes(data); a=[x/32768 for x in a]
    elif bits==32 and tag==3: a=array.array('f'); a.frombytes(data); a=list(a)
    elif bits==32: a=array.array('i'); a.frombytes(data); a=[x/2**31 for x in a]
    elif bits==24: a=[int.from_bytes(data[j:j+3],'little',signed=True)/2**23 for j in range(0,len(data),3)]
    return ch,rate,bits,a
a=load(sys.argv[1]); b=load(sys.argv[2])
print('A ch=%d rate=%d bits=%d frames=%d'%(a[0],a[1],a[2],len(a[3])//a[0]))
print('B ch=%d rate=%d bits=%d frames=%d'%(b[0],b[1],b[2],len(b[3])//b[0]))
x=a[3]; y=b[3]; n=min(len(x),len(y))
sig=sum(v*v for v in x[:n]); err=sum((x[k]-y[k])**2 for k in range(n))
print('SNR %.1f dB over %d frames'%(10*math.log10(sig/max(err,1e-30)), n//2))
# per-second SNR to locate divergence
step=48000*2
for s in range(0,n,step):
    xs=x[s:s+step]; ys=y[s:s+step]
    sg=sum(v*v for v in xs); e=sum((p-q)**2 for p,q in zip(xs,ys))
    print('  %4.1fs %6.1f dB'%(s/2/48000, 10*math.log10(max(sg,1e-30)/max(e,1e-30))))
