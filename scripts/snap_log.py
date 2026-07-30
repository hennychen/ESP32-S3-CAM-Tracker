import serial, sys, time

port = sys.argv[1] if len(sys.argv) > 1 else 'COM9'
secs = int(sys.argv[2]) if len(sys.argv) > 2 else 15
do_reset = '--reset' in sys.argv

s = serial.Serial()
s.port = port
s.baudrate = 115200
s.dtr = False
s.rts = False
s.timeout = 0.2
s.open()
if do_reset:
    # 通过 RTS 触发一次硬复位（USB-Serial/JTAG 上等效于按 RESET）
    s.rts = True
    time.sleep(0.1)
    s.rts = False
    time.sleep(0.05)
s.reset_input_buffer()
print(f'--- reading {port} for {secs}s ---', flush=True)
end = time.time() + secs
while time.time() < end:
    d = s.read(4096)
    if d:
        sys.stdout.buffer.write(d)
        sys.stdout.buffer.flush()
s.close()
print('\n--- done ---')
