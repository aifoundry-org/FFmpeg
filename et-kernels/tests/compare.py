#!/usr/bin/env python3
"""Generate single MPEG2 I frames and compare native kernel with -idct simple.
No hardware access. Accepts --ffmpeg and --library to use other CPU builds.
Also exports params/input/dst/status blobs for separate, coordinated sysemu use.
"""
import argparse, ctypes as C, pathlib, struct, subprocess, re
ROOT = pathlib.Path(__file__).resolve().parents[2]
class Params(C.Structure):
    _fields_ = [(n,C.c_uint64) for n in ['input_addr','dst_addr','ref_fwd_addr','ref_bwd_addr','status_addr']] + [
        (n,C.c_uint32) for n in ['abi_version','nb_slices','mb_width','mb_height','width','height','linesize_y','linesize_uv','slice_table_offset','bitstream_offset','quant_offset','frame_bytes']] + [
        ('plane_offset',C.c_uint32*3),('pict_type',C.c_uint16),('picture_structure',C.c_uint16)] + [
        (n,C.c_uint8) for n in ['q_scale_type','intra_dc_precision','intra_vlc_format','alternate_scan','concealment_mv','frame_pred_frame_dct','top_field_first','progressive_frame']] + [
        ('f_code',(C.c_uint8*2)*2),('active_harts',C.c_uint16),('reserved16',C.c_uint16),('input_bytes',C.c_uint32),('frame_id',C.c_uint32)]
assert C.sizeof(Params)==128
class Bits:
    def __init__(self,data): self.data=data; self.pos=0
    def get(self,n):
        val=0
        for _ in range(n):
            val=(val<<1)|((self.data[self.pos//8]>>(7-self.pos%8))&1); self.pos+=1
        return val

def aligned(n):
    raw=C.create_string_buffer(n+64); ptr=(C.addressof(raw)+63)&~63
    return raw,ptr

def frame_params(data):
    p=Params(); p.abi_version=1; p.frame_id=1; p.active_harts=1
    from importlib.machinery import SourceFileLoader
    gen=SourceFileLoader('generate',str(ROOT/'et-kernels/scripts/generate.py')).load_module()
    matrix=gen.array('libavcodec/mpeg12data.c','ff_mpeg1_default_intra_matrix')[1]
    zig=gen.array('libavcodec/mathtables.c','ff_zigzag_direct')[1]
    matrices=[list(matrix),[16]*64,list(matrix),[16]*64]
    marks=[m.start() for m in re.finditer(b'\x00\x00\x01',data)] + [len(data)]
    slices=[]
    for i in range(len(marks)-1):
        part=data[marks[i]:marks[i+1]]; code=part[3]; b=Bits(part[4:])
        if code==0xb3:
            p.width=b.get(12); p.height=b.get(12)
            b.get(4+4+18+1+10+1)
            for m in range(2):
                if b.get(1):
                    for k in range(64): matrices[m][zig[k]]=b.get(8)
                    matrices[m+2]=list(matrices[m])
        elif code==0:
            b.get(10); p.pict_type=b.get(3)
        elif code==0xb5:
            typ=b.get(4)
            if typ==1:
                b.get(8); progressive=b.get(1); chroma=b.get(2)
                assert chroma==1
            elif typ==8:
                for direction in range(2):
                    for axis in range(2): p.f_code[direction][axis]=b.get(4)
                p.intra_dc_precision=b.get(2); p.picture_structure=b.get(2)
                for field in ['top_field_first','frame_pred_frame_dct','concealment_mv','q_scale_type','intra_vlc_format','alternate_scan']:
                    setattr(p,field,b.get(1))
                b.get(1+1); p.progressive_frame=b.get(1)
        elif 1<=code<=0xaf:
            slices.append((code-1,part))
    p.mb_width=(p.width+15)//16; p.mb_height=(p.height+15)//16
    p.linesize_y=(p.mb_width*16+63)&~63; p.linesize_uv=(p.mb_width*8+63)&~63
    p.plane_offset[1]=p.mb_height*16*p.linesize_y
    p.plane_offset[2]=p.plane_offset[1]+p.mb_height*8*p.linesize_uv
    p.frame_bytes=p.plane_offset[2]+p.mb_height*8*p.linesize_uv
    p.nb_slices=len(slices); p.quant_offset=0; p.slice_table_offset=512
    p.bitstream_offset=(512+16*len(slices)+63)&~63
    stream=bytearray(); desc=bytearray()
    for row,part in slices:
        desc+=struct.pack('<IIHHI',len(stream),len(part),row,0,0); stream+=part
    payload=bytearray(p.bitstream_offset)
    payload[:512]=struct.pack('<256H',*[v for mat in matrices for v in mat])
    payload[512:512+len(desc)]=desc; payload+=stream; p.input_bytes=len(payload)
    return p,payload

def decode(lib,data,harts):
    p,payload=frame_params(data)
    raw_i,p.input_addr=aligned(len(payload)); C.memmove(p.input_addr,bytes(payload),len(payload))
    raw_o,p.dst_addr=aligned(p.frame_bytes); C.memset(p.dst_addr,0xa5,p.frame_bytes)
    raw_s,p.status_addr=aligned(p.nb_slices*64); C.memset(p.status_addr,0xcc,p.nb_slices*64)
    p.active_harts=harts
    for h in reversed(range(harts)):
        ret=lib.et_mpeg2_decode(C.byref(p),h)
        if ret:
            codes=[struct.unpack('<4I',C.string_at(p.status_addr+i*64,16)) for i in range(p.nb_slices)]
            raise AssertionError(f'decode ret={ret} hart={h} statuses={codes}')
    decoded=bytearray()
    for plane in range(3):
        w=(p.width+1)//2 if plane else p.width
        height=(p.height+1)//2 if plane else p.height
        stride=p.linesize_uv if plane else p.linesize_y
        for y in range(height):
            decoded+=C.string_at(p.dst_addr+p.plane_offset[plane]+y*stride,w)
    return p,payload,bytes(decoded)

def main():
    ap=argparse.ArgumentParser()
    ap.add_argument('--ffmpeg',default=str(ROOT/'et-kernels/build-reference/ffmpeg'))
    ap.add_argument('--library',default=str(ROOT/'et-kernels/build-native/libet_mpeg2_native.so'))
    ap.add_argument('--out',type=pathlib.Path,default=ROOT/'et-kernels/build-native/fixtures')
    args=ap.parse_args(); args.out.mkdir(parents=True,exist_ok=True)
    lib=C.CDLL(args.library); lib.et_mpeg2_decode.argtypes=[C.POINTER(Params),C.c_uint]
    tests=[('basic','128x96',[]),('edge','318x242',[]),('vlc','128x96',['-intra_vlc','1']),
           ('scan','128x96',['-alternate_scan','1']),
           ('nonlinear','128x96',['-non_linear_quant','1','-qmax','28']),
           ('dc9','128x96',['-dc','9']),
           ('dc10','128x96',['-dc','10']),
           ('fielddct','128x96',['-flags','+ildct','-top','1']),
           ('matrix','128x96',['-intra_matrix',','.join(str(8 if i==0 else 1+i%31) for i in range(64))]),
           ('hd','1920x1080',[]),
           ('combined','128x96',['-intra_vlc','1','-alternate_scan','1','-dc','11'])]
    for name,size,extra in tests:
        m2v=args.out/(name+'.m2v'); yuv=args.out/(name+'.yuv')
        def ff(cmd): subprocess.run([args.ffmpeg,'-hide_banner','-loglevel','error','-y']+cmd,check=True)
        ff(['-f','lavfi','-i','testsrc2=size='+size+':rate=25','-frames:v','1',
            '-c:v','mpeg2video','-g','1','-q:v','5']+extra+['-f','mpeg2video',str(m2v)])
        ff(['-threads','1','-idct','simple','-i',str(m2v),'-frames:v','1','-f','rawvideo','-pix_fmt','yuv420p',str(yuv)])
        for harts in [1,64]:
            p,payload,result=decode(lib,m2v.read_bytes(),harts)
            oracle=yuv.read_bytes(); assert len(result)==len(oracle)
            if result != oracle:
                deltas=[abs(x-y) for x,y in zip(result,oracle)]
                raise AssertionError(f'{name} harts={harts}: {sum(d!=0 for d in deltas)} mismatches, max={max(deltas)}')
            print(f'{name}: {p.width}x{p.height}, {harts} harts, {len(result)} bytes bit-exact')
        if name=='basic':
            # Relative placeholders: runtime/sysemu harness must relocate these.
            p.input_addr=p.dst_addr=p.status_addr=0; p.active_harts=1
            (args.out/'basic.params').write_bytes(bytes(p))
            (args.out/'basic.input').write_bytes(payload)
if __name__=='__main__': main()
