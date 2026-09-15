# Project-local PIC crt0

`crt0-efi-x86_64.o` here is the GNU-EFI x86_64 startup object, rebuilt from
source with `-fPIC`.

## Why

Alpine's prebuilt `/usr/lib/crt0-efi-x86_64.o` (from `gnu-efi-dev`) is compiled
**without** `-fPIC`. GNU-EFI links the final image with `-shared`, and the
crt0's `R_X86_64_PC32` relocations against `ImageBase` and `_DYNAMIC` cannot
be resolved in a `-shared` link unless the object is PIC. The prebuilt object
fails with:

```
ld: /usr/lib/crt0-efi-x86_64.o: relocation R_X86_64_PC32 against undefined
    symbol `ImageBase' can not be used when making a shared object
```

## How it was built

From the GNU-EFI source tree (master, matching 4.0.2):

```sh
gcc -fPIC -fno-stack-protector -fno-stack-check -fno-stack-clash-protection \
    -mno-red-zone -maccumulate-outgoing-args -fshort-wchar -ffreestanding \
    -fno-builtin -Iinc -Iinc/x86_64 \
    -c gnuefi/crt0-efi-x86_64.S -o crt0-efi-x86_64.o
```

The Makefile references this object via `CRT0 := gnuefi/crt0-efi-$(ARCH).o`.
