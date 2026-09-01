#!/usr/bin/env python3
"""Insert a file into a host-formatted (mkfs.exfat) image so AuraLite can
read a Linux-created file (external-lane read test).  Builds a correct FILE
entry set (0x85+0xC0+0xC1), appends it after the root EOD, allocates one
cluster, sets FAT, and marks the bitmap. Matches the driver's on-disk format.
"""
import struct, sys

def csum_dentry(d, chk, primary):
    d=bytes(d)
    chk=(((chk<<15)|(chk>>1))&0xFFFF)+d[0]
    chk=(((chk<<15)|(chk>>1))&0xFFFF)+d[1]
    start=4 if primary else 2
    for i in range(start,32):
        chk=(((chk<<15)|(chk>>1))&0xFFFF)+d[i]
    return chk&0xFFFF

def set_csum(entries):
    c=0
    c=csum_dentry(entries[0],c,True)
    for e in entries[1:]:
        c=csum_dentry(e,c,False)
    return c&0xFFFF

def name_hash(name):
    h=0
    for ch in name.encode('ascii'):
        if 0x61 <= ch <= 0x7A: ch-=32   # host upcase table upcases ASCII
        h=(((h<<15)|(h>>1))&0xFFFF)+(ch&0xFF)
        h=(((h<<15)|(h>>1))&0xFFFF)+(ch>>8)
    return h&0xFFFF

def main(img_path, filename, content):
    img=open(img_path,'r+b')
    data=img.read()
    def r32(o): return struct.unpack_from('<I',data,o)[0]
    def r64(o): return struct.unpack_from('<Q',data,o)[0]
    bps=1<<data[108]; spc=1<<data[109]
    heap=r32(88); root=r32(96); clu_count=r32(92)
    def clu_sec(c): return heap+(c-2)*spc
    # read root dir cluster
    rbase=clu_sec(root)*bps
    rcl=bytearray(data[rbase:rbase+spc*bps])
    # find bitmap entry (0x81) and upcase/root layout
    bmclu=None; bmsize=None
    nent=len(rcl)//32
    eod_idx=None
    for i in range(nent):
        t=rcl[i*32]
        if t==0x81:
            bmclu=struct.unpack_from('<I',rcl,i*32+20)[0]
            bmsize=struct.unpack_from('<Q',rcl,i*32+24)[0]
        if t==0x00 and eod_idx is None:
            eod_idx=i
    assert bmclu is not None, "no bitmap entry"
    # read bitmap cluster(s)
    bmb=clu_sec(bmclu)*bps
    bm=bytearray(data[bmb:bmb+bmsize])
    # find first free cluster (bit cluster-2)
    free=None
    for c in range(2, clu_count):
        cc=c-2
        if not (bm[cc>>3]>>(cc&7))&1:
            free=c; break
    assert free, "no free cluster"
    # mark free cluster in bitmap
    cc=free-2; bm[cc>>3]|=1<<(cc&7)
    # write bitmap back
    img.seek(bmb); img.write(bytes(bm))
    # write file content to the free cluster
    img.seek(clu_sec(free)*bps); img.write(content)
    # set FAT[free]=EOF
    fat_off=r32(80)
    img.seek(fat_off*bps + free*4); img.write(struct.pack('<I',0xFFFFFFFF))
    # build entry set
    name=filename.encode('ascii')
    nlen=len(name)
    nname=(nlen+14)//15
    f=bytearray(32)
    f[0]=0x85; f[1]=1+nname
    f[4:6]=struct.pack('<H',0x20)  # archive
    ts=0x8d1029c8
    f[8:12]=struct.pack('<I',ts); f[12:16]=struct.pack('<I',ts); f[16:20]=struct.pack('<I',ts)
    s=bytearray(32)
    s[0]=0xC0; s[3]=nlen
    s[4:6]=struct.pack('<H',name_hash(filename))
    s[8:16]=struct.pack('<Q',len(content)); s[20:24]=struct.pack('<I',free); s[24:32]=struct.pack('<Q',len(content))
    entries=[f,s]
    for k in range(nname):
        n=bytearray(32); n[0]=0xC1
        for j in range(15):
            idx=k*15+j
            if idx<nlen:
                n[2+j*2]=name[idx]
        entries.append(n)
    # set checksum
    entries[0][2:4]=struct.pack('<H',set_csum(entries))
    setbytes=b''.join(bytes(e) for e in entries)
    # append at EOD position in root
    assert eod_idx is not None, "no EOD in root"
    neweod=bytearray(32); neweod[0]=0x00
    for off in range(len(setbytes)):
        rcl[eod_idx*32+off]=setbytes[off]
    # write new EOD after the set
    neweod_pos=eod_idx+len(entries)
    rcl[neweod_pos*32]=0x00
    # also ensure trailing beyond is zero
    img.seek(rbase); img.write(bytes(rcl))
    img.close()
    print(f"seeded {filename} (cluster {free}, {len(content)} bytes) into {img_path}")

if __name__=='__main__':
    main(sys.argv[1], sys.argv[2], sys.argv[3].encode('ascii'))
