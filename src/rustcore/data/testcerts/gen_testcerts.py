#!/usr/bin/env python3
"""Regenerate the SEC22 tlsprobe fixture certificates.

Outputs DER files into this directory:

  ca.der / ca-key.der            scratch root CA (trust-anchor seam)
  valid.der / valid-key.der      CA-signed leaf, SAN=localhost, in-window
  expired.der / ...-key.der      CA-signed leaf, SAN=localhost, notAfter past
  notyet.der / ...-key.der       CA-signed leaf, SAN=localhost, notBefore future
  wronghost.der / ...-key.der    CA-signed leaf, SAN=wrong-host.invalid
  selfsigned.der / ...-key.der   self-signed leaf, SAN=localhost (untrusted)
  sha1.der / sha1-key.der        CA-signed leaf with a sha1 signature
                               (best effort — skipped if the local
                               cryptography build refuses SHA1)

Every key is an unencrypted PKCS#8 RSA-2048 blob.  These are throwaway
test fixtures — they protect nothing, and regenerating them is always
safe:  python3 gen_testcerts.py
"""

import datetime
import os

from cryptography import x509
from cryptography.hazmat.primitives import hashes, serialization
from cryptography.hazmat.primitives.asymmetric import rsa
from cryptography.x509.oid import NameOID

HERE = os.path.dirname(os.path.abspath(__file__))
NOW = datetime.datetime.now(datetime.timezone.utc)


def key():
    return rsa.generate_private_key(public_exponent=65537, key_size=2048)


def name(cn):
    return x509.Name([x509.NameAttribute(NameOID.COMMON_NAME, cn)])


def write(base, cert, priv):
    cert_path = os.path.join(HERE, base + ".der")
    with open(cert_path, "wb") as f:
        f.write(cert.public_bytes(serialization.Encoding.DER))
    # PEM copies feed the openssl s_server fixture the autotests run.
    with open(os.path.join(HERE, base + ".pem"), "wb") as f:
        f.write(cert.public_bytes(serialization.Encoding.PEM))
    if priv is not None:
        key_path = os.path.join(HERE, base + "-key.der")
        with open(key_path, "wb") as f:
            f.write(
                priv.private_bytes(
                    serialization.Encoding.DER,
                    serialization.PrivateFormat.PKCS8,
                    serialization.NoEncryption(),
                )
            )
        with open(os.path.join(HERE, base + "-key.pem"), "wb") as f:
            f.write(
                priv.private_bytes(
                    serialization.Encoding.PEM,
                    serialization.PrivateFormat.PKCS8,
                    serialization.NoEncryption(),
                )
            )


def leaf(subject_cn, sans, issuer_cert, issuer_key, not_before, not_after,
         subject_key=None, hash_alg=hashes.SHA256()):
    subject_key = subject_key or key()
    san = x509.SubjectAlternativeName(
        [x509.DNSName(s) for s in sans])
    builder = (
        x509.CertificateBuilder()
        .subject_name(name(subject_cn))
        .issuer_name(issuer_cert.subject)
        .public_key(subject_key.public_key())
        .serial_number(x509.random_serial_number())
        .not_valid_before(not_before)
        .not_valid_after(not_after)
        .add_extension(san, critical=False)
        .add_extension(
            x509.BasicConstraints(ca=False, path_length=None), critical=True)
    )
    return builder.sign(issuer_key, hash_alg), subject_key


def main():
    ca_key = key()
    ca = (
        x509.CertificateBuilder()
        .subject_name(name("Arora Test CA"))
        .issuer_name(name("Arora Test CA"))
        .public_key(ca_key.public_key())
        .serial_number(x509.random_serial_number())
        .not_valid_before(NOW - datetime.timedelta(days=3650))
        .not_valid_after(NOW + datetime.timedelta(days=3650))
        .add_extension(
            x509.BasicConstraints(ca=True, path_length=None), critical=True)
        .add_extension(
            x509.KeyUsage(
                digital_signature=False, content_commitment=False,
                key_encipherment=False, data_encipherment=False,
                key_agreement=False, key_cert_sign=True, crl_sign=True,
                encipher_only=False, decipher_only=False),
            critical=True)
        .sign(ca_key, hashes.SHA256())
    )
    write("ca", ca, ca_key)

    year = datetime.timedelta(days=365)
    day = datetime.timedelta(days=1)

    cert, k = leaf("localhost", ["localhost"], ca, ca_key,
                   NOW - year, NOW + year)
    write("valid", cert, k)

    cert, k = leaf("localhost", ["localhost"], ca, ca_key,
                   NOW - 2 * year, NOW - day)
    write("expired", cert, k)

    cert, k = leaf("localhost", ["localhost"], ca, ca_key,
                   NOW + day, NOW + year)
    write("notyet", cert, k)

    cert, k = leaf("wrong-host.invalid", ["wrong-host.invalid"], ca, ca_key,
                   NOW - year, NOW + year)
    write("wronghost", cert, k)

    # Self-signed leaf — an unknown issuer from the probe's viewpoint.
    ss_key = key()
    ss = (
        x509.CertificateBuilder()
        .subject_name(name("localhost"))
        .issuer_name(name("localhost"))
        .public_key(ss_key.public_key())
        .serial_number(x509.random_serial_number())
        .not_valid_before(NOW - year)
        .not_valid_after(NOW + year)
        .add_extension(
            x509.SubjectAlternativeName([x509.DNSName("localhost")]),
            critical=False)
        .add_extension(
            x509.BasicConstraints(ca=False, path_length=None), critical=True)
        .sign(ss_key, hashes.SHA256())
    )
    write("selfsigned", ss, ss_key)

    # sha1-signed leaf — webpki has no sha1 implementation, so the
    # signature cannot verify and the class is weak-signature.
    try:
        cert, k = leaf("localhost", ["localhost"], ca, ca_key,
                       NOW - year, NOW + year, hash_alg=hashes.SHA1())
        write("sha1", cert, k)
        print("sha1 fixture written")
    except Exception as e:  # newer cryptography may refuse SHA1 signing
        print("sha1 fixture skipped:", e)

    print("fixtures written to", HERE)


if __name__ == "__main__":
    main()
