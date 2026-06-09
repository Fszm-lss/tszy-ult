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
    # delete last key
    rm -f *.key *.crt

    # CA
    if [ $dh -eq 1 ]; then
        openssl genrsa -out ca.key 2048
    else
        openssl ecparam -genkey -name secp384r1 -out ca.key
    fi
    openssl req -x509 -new -nodes -key ca.key -subj "/CN=lygc.com" -days 5000 -out ca.crt

    if [ $dh -eq 1 ]; then
        openssl genrsa -out server.key 2048
    else
        openssl ecparam -genkey -name secp384r1 -out server.key
    fi
    openssl req -new -key server.key -subj "/CN=fszm.com" -out server.csr
    openssl x509 -req -in server.csr -CA ca.crt -CAkey ca.key -CAcreateserial -days 5000 -out server.crt

    if [ $dh -eq 1 ]; then
        openssl genrsa -out client.key 2048
    else
        openssl ecparam -genkey -name secp384r1 -out client.key
    fi
    openssl req -new -key client.key -subj "/CN=localhost" -out client.csr
    openssl x509 -req -in client.csr -CA ca.crt -CAkey ca.key -CAcreateserial -days 5000 -out client.crt

    # clear temp
    rm -f *.srl *.csr
fi

if [ $rsa -eq 1 ]; then
    openssl genpkey -algorithm RSA -out rsa_private_key.pem -pkeyopt rsa_keygen_bits:4096
	openssl rsa -pubout -in rsa_private_key.pem -out rsa_public_key.pem
fi

if [ $dsa -eq 1 ]; then
    openssl dsaparam -out dsa_param.pem 2048
    openssl genpkey -paramfile dsa_param.pem -out dsa_private_key.pem
    openssl pkey -in dsa_private_key.pem -pubout -out dsa_public_key.pem
    rm dsa_param.pem
fi

cd ..
