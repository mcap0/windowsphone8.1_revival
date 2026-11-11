import hashlib
import hmac
import json
import struct
import sys

from cryptography.hazmat.primitives.ciphers import Cipher, algorithms, modes


AUTH_KEY = bytes((i * 7) % 256 for i in range(256))
SALT = 0x0123456789ABCDEF
SESSION_ID = 0xFFEEDDCCBBAA9988
MSG_ID = 0x1122334455667788
SEQ_NO = 7

SHORT_BODY = b"MTProto 2.0 encrypted-layer vector"
BODY_100 = bytes(range(100))

CLIENT_TO_SERVER = ("client_to_server", 0)
SERVER_TO_CLIENT = ("server_to_client", 8)


def xor16(left, right):
    return bytes(a ^ b for a, b in zip(left, right))


def aes_ige_encrypt(key, iv, plaintext):
    if len(key) != 32:
        raise ValueError("AES-IGE requires a 32-byte key")
    if len(iv) != 32:
        raise ValueError("AES-IGE requires a 32-byte IV")
    if len(plaintext) % 16:
        raise ValueError("AES-IGE plaintext length must be a multiple of 16")

    # MTProto's IV layout is y0 || x0: previous ciphertext, then plaintext.
    y_prev = iv[:16]
    x_prev = iv[16:]
    encryptor = Cipher(algorithms.AES(key), modes.ECB()).encryptor()
    ciphertext = bytearray()

    for offset in range(0, len(plaintext), 16):
        plain_block = plaintext[offset:offset + 16]
        encrypted = encryptor.update(xor16(plain_block, y_prev))
        cipher_block = xor16(encrypted, x_prev)
        ciphertext.extend(cipher_block)
        y_prev = cipher_block
        x_prev = plain_block

    if encryptor.finalize():
        raise ValueError("Unexpected AES-ECB finalization output")
    return bytes(ciphertext)


def aes_ige_decrypt(key, iv, ciphertext):
    if len(key) != 32:
        raise ValueError("AES-IGE requires a 32-byte key")
    if len(iv) != 32:
        raise ValueError("AES-IGE requires a 32-byte IV")
    if len(ciphertext) % 16:
        raise ValueError("AES-IGE ciphertext length must be a multiple of 16")

    # MTProto's IV layout is y0 || x0: previous ciphertext, then plaintext.
    y_prev = iv[:16]
    x_prev = iv[16:]
    decryptor = Cipher(algorithms.AES(key), modes.ECB()).decryptor()
    plaintext = bytearray()

    for offset in range(0, len(ciphertext), 16):
        cipher_block = ciphertext[offset:offset + 16]
        decrypted = decryptor.update(xor16(cipher_block, x_prev))
        plain_block = xor16(decrypted, y_prev)
        plaintext.extend(plain_block)
        y_prev = cipher_block
        x_prev = plain_block

    if decryptor.finalize():
        raise ValueError("Unexpected AES-ECB finalization output")
    return bytes(plaintext)


def message_key(auth_key, plaintext_padded, direction_offset):
    start = 88 + direction_offset
    digest = hashlib.sha256(
        auth_key[start:start + 32] + plaintext_padded
    ).digest()
    return digest[8:24]


def derive_aes_key_iv(auth_key, msg_key, direction_offset):
    sha256_a = hashlib.sha256(
        msg_key + auth_key[direction_offset:direction_offset + 36]
    ).digest()
    sha256_b = hashlib.sha256(
        auth_key[direction_offset + 40:direction_offset + 76] + msg_key
    ).digest()

    aes_key = sha256_a[:8] + sha256_b[8:24] + sha256_a[24:32]
    aes_iv = sha256_b[:8] + sha256_a[8:24] + sha256_b[24:32]
    return aes_key, aes_iv


def build_plaintext(body):
    header = struct.pack(
        "<QQQII",
        SALT,
        SESSION_ID,
        MSG_ID,
        SEQ_NO,
        len(body),
    )
    unpadded = header + body
    padding_length = (-len(unpadded)) % 16
    if padding_length < 12:
        padding_length += 16
    padding = bytes((0xA5 + index * 17) & 0xFF
                    for index in range(padding_length))
    return unpadded + padding


def encrypt_packet(auth_key, plaintext_padded, direction_offset):
    msg_key = message_key(auth_key, plaintext_padded, direction_offset)
    aes_key, aes_iv = derive_aes_key_iv(
        auth_key, msg_key, direction_offset
    )
    ciphertext = aes_ige_encrypt(aes_key, aes_iv, plaintext_padded)
    auth_key_id = hashlib.sha1(auth_key).digest()[-8:]
    return auth_key_id + msg_key + ciphertext


def decrypt_packet(auth_key, packet, direction_offset):
    if len(packet) < 24:
        raise ValueError("Encrypted packet is too short")

    auth_key_id = packet[:8]
    expected_auth_key_id = hashlib.sha1(auth_key).digest()[-8:]
    if not hmac.compare_digest(auth_key_id, expected_auth_key_id):
        raise ValueError("auth_key_id does not match")

    msg_key = packet[8:24]
    ciphertext = packet[24:]
    if not ciphertext or len(ciphertext) % 16:
        raise ValueError("Encrypted packet has an invalid ciphertext length")

    aes_key, aes_iv = derive_aes_key_iv(
        auth_key, msg_key, direction_offset
    )
    plaintext_padded = aes_ige_decrypt(aes_key, aes_iv, ciphertext)
    computed_msg_key = message_key(
        auth_key, plaintext_padded, direction_offset
    )
    if not hmac.compare_digest(msg_key, computed_msg_key):
        raise ValueError("msg_key verification failed")

    if len(plaintext_padded) < 32 + 12:
        raise ValueError("Decrypted message is too short")
    body_length = struct.unpack_from("<I", plaintext_padded, 28)[0]
    padding_length = len(plaintext_padded) - 32 - body_length
    if not 12 <= padding_length <= 1024:
        raise ValueError("Decrypted message has invalid padding length")
    if 32 + body_length + padding_length != len(plaintext_padded):
        raise ValueError("Decrypted message has an invalid body length")

    expected_header = struct.pack(
        "<QQQII",
        SALT,
        SESSION_ID,
        MSG_ID,
        SEQ_NO,
        body_length,
    )
    if plaintext_padded[:32] != expected_header:
        raise ValueError("Decrypted message header does not match")

    return plaintext_padded


def build_vector(body):
    plaintext_padded = build_plaintext(body)
    packets = {}

    for direction_name, direction_offset in (
        CLIENT_TO_SERVER,
        SERVER_TO_CLIENT,
    ):
        packet = encrypt_packet(
            AUTH_KEY, plaintext_padded, direction_offset
        )
        decrypted = decrypt_packet(AUTH_KEY, packet, direction_offset)
        if decrypted != plaintext_padded:
            raise AssertionError("MTProto decrypt self-check failed")
        packets[direction_name + "_packet_hex"] = packet.hex()

    return {
        "body_hex": body.hex(),
        "body_length": len(body),
        **packets,
    }


def main():
    output = {
        "auth_key_id_hex": hashlib.sha1(AUTH_KEY).digest()[-8:].hex(),
        "fixed_fields": {
            "salt": SALT,
            "session_id": SESSION_ID,
            "msg_id": MSG_ID,
            "seq_no": SEQ_NO,
        },
        "vectors": {
            "short_body": build_vector(SHORT_BODY),
            "body_100_bytes": build_vector(BODY_100),
        },
        "self_check": True,
    }
    json.dump(output, sys.stdout, indent=2, sort_keys=True)
    sys.stdout.write("\n")


if __name__ == "__main__":
    main()
