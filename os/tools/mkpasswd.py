#!/usr/bin/env python3
"""Password digests for /etc/shadow.

v1.1.3: PBKDF2-HMAC-SHA256 (RFC 8018), salt + iteration count per line --
the same construction kernel/sha256.c and user/sha256.c implement, which
is exactly what the shadow_roundtrip ktest proves byte for byte. The old
sha256(salt || password) construction stays available as LEGACY: those
lines still verify on old disks and login upgrades them on success
(the runtime leg of the contract is exercised by /bin/shadowtest).

Shadow line formats:
    new:     name:salt:iters:hexdigest
    legacy:  name:salt:hexdigest

CLI: mkpasswd.py PASSWORD [SALT] [ITERS]  -> "salt[:iters]:hash"
"""
import hashlib
import secrets
import sys

ALPHABET = "abcdefghjkmnpqrstuvwxyzABCDEFGHJKMNPQRSTUVWXYZ23456789"
ITERS = 4096
PW_MAX = 40          # etc.h ETC_PW_MAX -- set and verify clamp identically


def random_token(n=8):
    return ''.join(secrets.choice(ALPHABET) for _ in range(n))


def hash_password(salt, password, iters=ITERS):
    """PBKDF2-HMAC-SHA256, the v1.1.3 shadow digest."""
    return hashlib.pbkdf2_hmac('sha256', password.encode()[:PW_MAX],
                               salt.encode(), iters).hex()


def hash_password_legacy(salt, password):
    """sha256(salt || password), the pre-v1.1.3 digest (old disks)."""
    return hashlib.sha256((salt + password[:PW_MAX]).encode()).hexdigest()


def shadow_line(name, password, salt=None, iters=ITERS):
    if salt is None:
        salt = random_token()
    return f"{name}:{salt}:{iters}:{hash_password(salt, password, iters)}"


def shadow_line_legacy(name, password, salt=None):
    if salt is None:
        salt = random_token()
    return f"{name}:{salt}:{hash_password_legacy(salt, password)}"


if __name__ == '__main__':
    if len(sys.argv) not in (2, 3, 4):
        sys.exit('usage: mkpasswd.py PASSWORD [SALT] [ITERS]')
    pw = sys.argv[1]
    salt = sys.argv[2] if len(sys.argv) > 2 else random_token()
    iters = int(sys.argv[3]) if len(sys.argv) > 3 else ITERS
    print(f"{salt}:{iters}:{hash_password(salt, pw, iters)}")
