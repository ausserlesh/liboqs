#!/bin/bash

# sudo apt-get install qemu

set -e

CHOST=arm-linux-gnueabi

qemu-arm -cpu cortex-a8 -L /usr/arm-linux-gnueabi tests/example_kem

qemu-arm -cpu cortex-a8 -L /usr/arm-linux-gnueabi tests/test_kem BIKE1-L1
qemu-arm -cpu cortex-a8 -L /usr/arm-linux-gnueabi tests/test_kem BIKE1-L3
qemu-arm -cpu cortex-a8 -L /usr/arm-linux-gnueabi tests/test_kem BIKE1-L5
qemu-arm -cpu cortex-a8 -L /usr/arm-linux-gnueabi tests/test_kem BIKE2-L1
qemu-arm -cpu cortex-a8 -L /usr/arm-linux-gnueabi tests/test_kem BIKE2-L3
qemu-arm -cpu cortex-a8 -L /usr/arm-linux-gnueabi tests/test_kem BIKE2-L5
qemu-arm -cpu cortex-a8 -L /usr/arm-linux-gnueabi tests/test_kem BIKE3-L1
qemu-arm -cpu cortex-a8 -L /usr/arm-linux-gnueabi tests/test_kem BIKE3-L3
qemu-arm -cpu cortex-a8 -L /usr/arm-linux-gnueabi tests/test_kem BIKE3-L5
qemu-arm -cpu cortex-a8 -L /usr/arm-linux-gnueabi tests/test_kem Kyber512
qemu-arm -cpu cortex-a8 -L /usr/arm-linux-gnueabi tests/test_kem Kyber768
qemu-arm -cpu cortex-a8 -L /usr/arm-linux-gnueabi tests/test_kem Kyber1024
qemu-arm -cpu cortex-a8 -L /usr/arm-linux-gnueabi tests/test_kem NewHope-512-CCA
qemu-arm -cpu cortex-a8 -L /usr/arm-linux-gnueabi tests/test_kem NewHope-1024-CCA
qemu-arm -cpu cortex-a8 -L /usr/arm-linux-gnueabi tests/test_kem NTRU-HPS-2048-509
qemu-arm -cpu cortex-a8 -L /usr/arm-linux-gnueabi tests/test_kem NTRU-HPS-2048-677
qemu-arm -cpu cortex-a8 -L /usr/arm-linux-gnueabi tests/test_kem NTRU-HPS-4096-821
qemu-arm -cpu cortex-a8 -L /usr/arm-linux-gnueabi tests/test_kem NTRU-HRSS-701
qemu-arm -cpu cortex-a8 -L /usr/arm-linux-gnueabi tests/test_kem LightSaber-KEM
qemu-arm -cpu cortex-a8 -L /usr/arm-linux-gnueabi tests/test_kem Saber-KEM
qemu-arm -cpu cortex-a8 -L /usr/arm-linux-gnueabi tests/test_kem FireSaber-KEM
qemu-arm -cpu cortex-a8 -L /usr/arm-linux-gnueabi tests/test_kem FrodoKEM-640-AES
qemu-arm -cpu cortex-a8 -L /usr/arm-linux-gnueabi tests/test_kem FrodoKEM-640-SHAKE
qemu-arm -cpu cortex-a8 -L /usr/arm-linux-gnueabi tests/test_kem FrodoKEM-976-AES
qemu-arm -cpu cortex-a8 -L /usr/arm-linux-gnueabi tests/test_kem FrodoKEM-976-SHAKE
qemu-arm -cpu cortex-a8 -L /usr/arm-linux-gnueabi tests/test_kem FrodoKEM-1344-AES
qemu-arm -cpu cortex-a8 -L /usr/arm-linux-gnueabi tests/test_kem FrodoKEM-1344-SHAKE
qemu-arm -cpu cortex-a8 -L /usr/arm-linux-gnueabi tests/test_kem Sidh-p434
qemu-arm -cpu cortex-a8 -L /usr/arm-linux-gnueabi tests/test_kem Sidh-p503
qemu-arm -cpu cortex-a8 -L /usr/arm-linux-gnueabi tests/test_kem Sidh-p610
qemu-arm -cpu cortex-a8 -L /usr/arm-linux-gnueabi tests/test_kem Sidh-p751
qemu-arm -cpu cortex-a8 -L /usr/arm-linux-gnueabi tests/test_kem Sike-p434
qemu-arm -cpu cortex-a8 -L /usr/arm-linux-gnueabi tests/test_kem Sike-p503
qemu-arm -cpu cortex-a8 -L /usr/arm-linux-gnueabi tests/test_kem Sike-p610
qemu-arm -cpu cortex-a8 -L /usr/arm-linux-gnueabi tests/test_kem Sike-p751

# qemu-arm -cpu cortex-a8 -L /usr/arm-linux-gnueabi tests/example_sig
# qemu-arm -cpu cortex-a8 -L /usr/arm-linux-gnueabi tests/test_sig

qemu-arm -cpu cortex-a8 -L /usr/arm-linux-gnueabi tests/test_aes
qemu-arm -cpu cortex-a8 -L /usr/arm-linux-gnueabi tests/test_sha3
