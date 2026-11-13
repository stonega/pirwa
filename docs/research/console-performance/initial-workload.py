
import hashlib,json,mmap,os,time
root='/mnt/pirwa-qa'
assert os.path.ismount(root) and os.path.isfile(root+'/.pirwa-qa-disposable')
block=1024*1024
count=1024
buf=mmap.mmap(-1,block)
buf[:]=hashlib.shake_256(b'Pirwa QA fixed seed').digest(block)
fd=os.open(root+'/workload.bin',os.O_CREAT|os.O_RDWR|os.O_DIRECT,0o600)
os.ftruncate(fd,count*block)
def write():
 os.lseek(fd,0,0)
 for _ in range(count):assert os.write(fd,buf)==block
 os.fsync(fd)
def read():
 os.lseek(fd,0,0)
 for _ in range(count):assert os.readv(fd,[buf])==block
write() # identical preconditioning for each sample, outside the timing span
t=time.monotonic();write();write_seconds=time.monotonic()-t
t=time.monotonic();read();read_seconds=time.monotonic()-t
t=time.monotonic();digest=hashlib.pbkdf2_hmac('sha256',b'Pirwa QA CPU',b'fixed salt',6000000).hex();cpu_seconds=time.monotonic()-t
os.close(fd);buf.close()
print(json.dumps(dict(cpu_seconds=cpu_seconds,cpu_digest=digest,write_mib_s=count/write_seconds,read_mib_s=count/read_seconds,bytes=count*block,guest_direct_io=True)))
