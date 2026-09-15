#!/usr/bin/env python3
"""Native I/P/B full-GOP oracle. No hardware access; reference buffers persist.
Pairs coded-order ET output with CPU display order by temporal_reference.
"""
import argparse, ctypes as C, pathlib, re, struct, subprocess
from compare import ROOT, Params, Bits, aligned, frame_params

def pictures(data):
    starts=[m.start() for m in re.finditer(b'\x00\x00\x01\x00',data)]
    prefix=data[:starts[0]]
    starts.append(len(data))
    gops=[m.start() for m in re.finditer(b'\x00\x00\x01\xb8',data)]
    group=0; base=0; last_display=-1
    for a,z in zip(starts,starts[1:]):
        newgroup=sum(g<a for g in gops)
        if newgroup!=group:
            base=last_display+1; group=newgroup
        b=Bits(data[a+4:z]); temporal=b.get(10)
        display=base+temporal; last_display=max(last_display,display)
        yield display,prefix+data[a:z]

def run_case(lib,stream,oracle,harts):
    allocations=[]; last=previous=0; count=0
    slots=[]; slot_bytes=0
    for temporal,packet in pictures(stream):
        p,payload=frame_params(packet)
        p.frame_id=count+1; p.active_harts=harts
        ri,p.input_addr=aligned(len(payload)); C.memmove(p.input_addr,bytes(payload),len(payload))
        if not slots:
            slot_bytes=p.frame_bytes+p.nb_slices*64
            for _ in range(3):
                ro,ptr=aligned(slot_bytes)
                C.memset(ptr,0xa5,slot_bytes)  # once, not per frame
                allocations.append(ro); slots.append(ptr)
        assert slot_bytes==p.frame_bytes+p.nb_slices*64
        # I/P keep the newest anchor alive for open-GOP backward pictures;
        # B uses the third slot, preserving both resident references.
        blocked=[previous,last] if p.pict_type==3 else [last]
        p.dst_addr=next(ptr for ptr in slots if ptr not in blocked)
        p.status_addr=p.dst_addr+p.frame_bytes
        if p.pict_type==2: p.ref_fwd_addr=last
        if p.pict_type==3: p.ref_fwd_addr=previous; p.ref_bwd_addr=last
        for hart in reversed(range(harts)):
            ret=lib.et_mpeg2_decode(C.byref(p),hart)
            if ret:
                statuses=[struct.unpack('<4I',C.string_at(p.status_addr+i*64,16)) for i in range(p.nb_slices)]
                raise AssertionError(f'coded={count} display={temporal} type={p.pict_type} ret={ret} hart={hart} statuses={statuses}')
        if p.pict_type!=3: previous,last=last,p.dst_addr
        visible=bytearray()
        for plane in range(3):
            width=(p.width+1)//2 if plane else p.width
            height=(p.height+1)//2 if plane else p.height
            stride=p.linesize_uv if plane else p.linesize_y
            for y in range(height): visible+=C.string_at(p.dst_addr+p.plane_offset[plane]+y*stride,width)
        reference=oracle[temporal*len(visible):(temporal+1)*len(visible)]
        if visible!=reference:
            assert len(visible)==len(reference)
            deltas=[abs(a-b) for a,b in zip(visible,reference)]
            first=next(i for i,d in enumerate(deltas) if d)
            raise AssertionError(f'coded={count} display={temporal} type={p.pict_type} harts={harts}: mismatches={sum(bool(d) for d in deltas)} max={max(deltas)} first={first} got={visible[first]} want={reference[first]}')
        for i in range(p.nb_slices):
            code,mbs,bits,frame=struct.unpack('<4I',C.string_at(p.status_addr+i*64,16))
            assert code==0 and mbs==p.mb_width and frame==p.frame_id
        count+=1
    return count

def main():
    ap=argparse.ArgumentParser()
    ap.add_argument('--ffmpeg',default=str(ROOT/'et-kernels/build-reference/ffmpeg'))
    ap.add_argument('--library',default=str(ROOT/'et-kernels/build-native/libet_mpeg2_native.so'))
    ap.add_argument('--out',type=pathlib.Path,default=ROOT/'et-kernels/build-native/fixtures')
    args=ap.parse_args();args.out.mkdir(parents=True,exist_ok=True)
    lib=C.CDLL(args.library);lib.et_mpeg2_decode.argtypes=[C.POINTER(Params),C.c_uint]
    cases=[('p','128x96',0,[]),('b','128x96',2,[]),('b-edge','318x242',2,[]),
           ('b-open','128x96',2,['-g','12','-frames:v','36']),
           ('b-dc11','128x96',2,['-dc','11']),
           ('b-matrix','128x96',2,['-intra_matrix',','.join(str(8 if i==0 else 1+i%31) for i in range(64)),
                                   '-inter_matrix',','.join(str(1+i%31) for i in range(64))]),
           ('b-vlc','128x96',2,['-intra_vlc','1']),
           ('b-qscale','128x96',2,['-non_linear_quant','1','-qmax','28']),
           ('b-hd','1920x1080',2,[])]
    for name,size,bframes,extra in cases:
        m2v=args.out/(name+'.m2v');yuv=args.out/(name+'.yuv')
        def ff(cmd):subprocess.run([args.ffmpeg,'-hide_banner','-loglevel','error','-y']+cmd,check=True)
        ff(['-f','lavfi','-i','testsrc2=size='+size+':rate=25','-frames:v','12','-c:v','mpeg2video',
            '-g','100','-bf',str(bframes),'-q:v','5']+extra+['-f','mpeg2video',str(m2v)])
        ff(['-threads','1','-idct','simple','-i',str(m2v),'-f','rawvideo','-pix_fmt','yuv420p',str(yuv)])
        for harts in [1,64]:
            count=run_case(lib,m2v.read_bytes(),yuv.read_bytes(),harts)
            print(f'{name}: {count} frames {size} {harts} harts bit-exact',flush=True)
if __name__=='__main__':main()
