#!/usr/bin/env python3
"""Salted SHA-256 password digests for /etc/shadow.

The construction is sha256(salt || password) with an 8-char salt -- the
same bytes kernel/sha256.c and user/sha256.c hash, which is exactly what
the shadow_roundtrip ktest proves. Teaching scope: no slow factor (real
systems use bcrypt/PBKDF2), recorded in DESIGN.md.

CLI: mkpasswd.py PASSWORD [SALT]   -> name-less "salt:hash" line
"""
import hashlib
import secrets
import sys

ALPHABET = "abcdefghjkmnpqrstuvwxyzABCDEFGHJKMNPQRSTUVWXYZ23456789"


def random_token(n=8):
    return ''.join(secrets.choice(ALPHABET) for _ in range(n))


def hash_password(salt, password):
    return hashlib.sha256((salt + password).encode()).hexdigest()


def shadow_line(name, password, salt=None):
    if salt is None:
        salt = random_token()
    return f"{name}:{salt}:{hash_password(salt, password)}"


if __name__ == '__main__':
    if len(sys.argv) not in (2, 3):
        sys.exit('usage: mkpasswd.py PASSWORD [SALT]')
    pw = sys.argv[1]
    salt = sys.argv[2] if len(sys.argv) == 3 else random_token()
    print(f"{salt}:{hash_password(salt, pw)}")
