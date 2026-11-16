/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Linux x86_64 ELF entry for the freestanding disposable recovery guest. */
.text
.global _start
_start:
    xor %rbp, %rbp
    and $-16, %rsp
    call guest_main
    ud2
.section .note.GNU-stack,"",@progbits
