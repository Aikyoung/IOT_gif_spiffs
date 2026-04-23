import struct, sys, os

def analyze_gif(path):
    with open(path, 'rb') as f:
        d = f.read()
    
    w = struct.unpack_from('<H', d, 6)[0]
    h = struct.unpack_from('<H', d, 8)[0]
    pkt = d[10]
    gct = (pkt >> 7) & 1
    gct_n = 2 ** ((pkt & 7) + 1)
    
    pos = 13 + (gct_n * 3 if gct else 0)
    frames = 0
    interlace_frames = []
    
    while pos < len(d):
        blk = d[pos]
        pos += 1
        if blk == 0x3B:
            break
        if blk == 0x21:
            lbl = d[pos]
            pos += 1
            if lbl == 0xF9:
                blen = d[pos]
                pos += 1 + blen + 1
            else:
                while pos < len(d):
                    s = d[pos]
                    pos += 1
                    if s == 0:
                        break
                    pos += s
            continue
        if blk == 0x2C:
            fx = struct.unpack_from('<H', d, pos)[0]
            fy = struct.unpack_from('<H', d, pos+2)[0]
            fw = struct.unpack_from('<H', d, pos+4)[0]
            fh = struct.unpack_from('<H', d, pos+6)[0]
            lf = d[pos+8]
            pos += 9
            interlaced = (lf >> 6) & 1
            lct_f = (lf >> 7) & 1
            lct_n2 = 2 ** ((lf & 7) + 1)
            if lct_f:
                pos += lct_n2 * 3
            pos += 1  # lzw min code size
            while pos < len(d):
                s = d[pos]
                pos += 1
                if s == 0:
                    break
                pos += s
            frames += 1
            interlace_frames.append(interlaced)
            if frames <= 3:
                print(f"  Frame {frames}: pos=({fx},{fy}) size={fw}x{fh} interlaced={interlaced} local_ct={lct_f}")
    
    name = os.path.basename(path)
    print(f"{name}: canvas={w}x{h}, GCT={gct} ({gct_n} colors), frames={frames}, any_interlaced={any(interlace_frames)}")
    mem_canvas = w * h * 2
    mem_idx = w * h
    print(f"  Streaming memory: canvas={mem_canvas} bytes, idxbuf={mem_idx} bytes, total={mem_canvas + mem_idx + 12300} bytes")
    return w, h

base = r'c:\Users\tanai\Desktop\30.201 WC & IOT\IOT_gif_rainmaker_v7\IOT_gif_spiffs\spiffs_data'
for name in ['gif_a.gif', 'gif_b.gif']:
    print()
    analyze_gif(os.path.join(base, name))
