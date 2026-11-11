import hashlib, hmac, subprocess, sys
from cryptography.hazmat.primitives.asymmetric.x25519 import X25519PrivateKey, X25519PublicKey
from cryptography.hazmat.primitives.ciphers.aead import ChaCha20Poly1305
import nacl.public
data = bytes((i * 7 + 1) & 255 for i in range(300))
k1 = nacl.public.PrivateKey(data[0:32]); k2 = nacl.public.PrivateKey(data[64:96])
sealed = nacl.public.Box(k2, k1.public_key).encrypt(data[20:60])   # from key2 to key1
out = subprocess.run([sys.argv[1], sealed.hex()], capture_output=True, text=True).stdout.split('\n')
got = {}
for l in out:
    if l: p = l.split(' ', 1); got.setdefault(p[0], []).append(p[1])
fail = 0
def chk(name, i, want):
    global fail
    if got[name][i] != want: fail += 1; print('FAIL', name, i, got[name][i][:40], want[:40])
for i, l in enumerate([0, 1, 63, 64, 65, 128, 300]): chk('b2s', i, f'{l} ' + hashlib.blake2s(data[:l]).hexdigest())
chk('b2k', 0, hashlib.blake2s(data[:116], key=data[:32], digest_size=16).hexdigest())
chk('hmac', 0, hmac.new(data[:32], data[40:140], hashlib.blake2s).hexdigest())
prk = hmac.new(data[:32], data[50:82], hashlib.blake2s).digest()
t1 = hmac.new(prk, b'\x01', hashlib.blake2s).digest(); t2 = hmac.new(prk, t1 + b'\x02', hashlib.blake2s).digest(); t3 = hmac.new(prk, t2 + b'\x03', hashlib.blake2s).digest()
chk('kdf', 0, ' '.join(x.hex() for x in (t1, t2, t3)))
chk('xpub', 0, X25519PrivateKey.from_private_bytes(data[:32]).public_key().public_bytes_raw().hex())
chk('xsh', 0, X25519PrivateKey.from_private_bytes(data[:32]).exchange(X25519PublicKey.from_public_bytes(data[100:132])).hex())
chk('aead', 0, ChaCha20Poly1305(data[:32]).encrypt(data[32:44], data[50:150], data[200:213]).hex())
chk('aeadopen', 0, '1 1')
b = bytes.fromhex(got['box'][0])
if nacl.public.Box(k2, k1.public_key).decrypt(b[24:], b[:24]) != data[10:87]: fail += 1; print('FAIL box')
chk('boxopen', 0, '1 ' + data[20:60].hex())
print('fail', fail)
