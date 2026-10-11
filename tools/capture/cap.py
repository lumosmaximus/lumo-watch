import serial, time, glob, re, sys
dur=float(sys.argv[1])
def op():
    for i in range(40):
        p=glob.glob('/dev/cu.usbmodem*')
        if p:
            try: return serial.Serial(p[0], 115200, timeout=0.3)
            except Exception: pass
        time.sleep(0.3)
s=op(); s.dtr=False; s.rts=True; time.sleep(0.1); s.rts=False
t=time.time()
while time.time()-t<dur:
    try: d=s.read(4096)
    except Exception:
        s.close(); time.sleep(0.5); s=op(); continue
    if d: sys.stdout.write(re.sub(r'\x1b\[[0-9;]*[A-Za-z]','',d.decode(errors='replace'))); sys.stdout.flush()
