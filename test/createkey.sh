#!/bin/sh

dh=0
ecdh=0
rsa=0
dsa=0
clean=0

if [ $# -eq 0 ]; then
    dh=1
elif [ "$1" = "dh" ]; then
    dh=1
elif [ "$1" = "ecdh" ]; then
    ecdh=1
elif [ "$1" = "rsa" ]; then
    rsa=1
elif [ "$1" = "dsa" ]; then
    dsa=1
elif [ "$1" = "clean" ]; then
    clean=1
fi

echo "clean $clean"
echo "createkey dh $dh"
echo "createkey ecdh $ecdh"
echo "createkey rsa $rsa"
echo "createkey dsa $dsa"

mkdir -p key
cd key

if [ $clean -eq 1 ]; then
    rm -f *.key *.crt *.pem
    echo "clean done"
fi

if [ $dh -eq 1 ] || [ $ecdh -eq 1 ]; then
    # Delete previous keys and certificates
    rm -f *.key *.crt

    # --- CA ---
    if [ $dh -eq 1 ]; then
        # Generate CA private key (RSA 2048-bit)
        openssl genrsa -out ca.key 2048
    else
        # Generate CA private key (ECDH secp384r1 curve)
        openssl ecparam -genkey -name secp384r1 -out ca.key
    fi
    # Self-sign CA root certificate (CN=lygc.com, valid 3650 days / 10 years)
    openssl req -x509 -new -nodes -key ca.key -subj "/CN=lygc.com" -days 3650 -out ca.crt

    # --- Server ---
    if [ $dh -eq 1 ]; then
        # Generate server private key (RSA 2048-bit)
        openssl genrsa -out server.key 2048
    else
        # Generate server private key (ECDH secp384r1 curve)
        openssl ecparam -genkey -name secp384r1 -out server.key
    fi
    # Generate server certificate signing request (CN=fszm.com)
    openssl req -new -key server.key -subj "/CN=fszm.com" -out server.csr
    # Sign server certificate with CA (valid 3650 days / 10 years)
    openssl x509 -req -in server.csr -CA ca.crt -CAkey ca.key -CAcreateserial -days 3650 -out server.crt

    # --- Client ---
    if [ $dh -eq 1 ]; then
        # Generate client private key (RSA 2048-bit)
        openssl genrsa -out client.key 2048
    else
        # Generate client private key (ECDH secp384r1 curve)
        openssl ecparam -genkey -name secp384r1 -out client.key
    fi
    # Generate client certificate signing request (CN=localhost)
    openssl req -new -key client.key -subj "/CN=localhost" -out client.csr
    # Sign client certificate with CA (valid 3650 days / 10 years)
    openssl x509 -req -in client.csr -CA ca.crt -CAkey ca.key -CAcreateserial -days 3650 -out client.crt

    # Clean up temporary files (serial number file, CSR files)
    rm -f *.srl *.csr
fi

if [ $rsa -eq 1 ]; then
    # Generate RSA private key (4096-bit)
    openssl genpkey -algorithm RSA -out rsa_private_key.pem -pkeyopt rsa_keygen_bits:4096
    # Extract RSA public key from private key
    openssl rsa -pubout -in rsa_private_key.pem -out rsa_public_key.pem
fi

if [ $dsa -eq 1 ]; then
    # Generate DSA parameters (2048-bit)
    openssl dsaparam -out dsa_param.pem 2048
    # Generate DSA private key from parameters
    openssl genpkey -paramfile dsa_param.pem -out dsa_private_key.pem
    # Extract DSA public key from private key
    openssl pkey -in dsa_private_key.pem -pubout -out dsa_public_key.pem
    # Remove temporary DSA parameter file
    rm dsa_param.pem
fi

cd ..
