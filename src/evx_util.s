# evx_util.s - shared primitives of the event engine (GNU as, Intel syntax).
#
#   evx_hash        64-bit hash of a byte string
#   evx_memeq       byte equality (internal convention: preserves every GPR
#                   except rax/r11; clobbers k1, k2, zmm16, zmm17)
#   evx_key16       ID -> 16-byte inline key (exact; pointer only for len >= 16)
#   evx_khash       index hash of a Key16
#   evx_keq         exact Key16 equality
#   evx_lookup      probe of a C-built LookupEntry table by Key16
#   evx_split_line  CSV dialect: split + unquote + validate one line
#   evx_parse_ts    F1 / F2 / F3 timestamp -> epoch ms
#
# Only zmm16-zmm31 / ymm16+ / xmm16+ are used, so no vzeroupper is needed:
# legacy-SSE code can never see a dirty upper state in these registers.

        .intel_syntax noprefix
        .text

# ---------------------------------------------------------------------------
# uint64_t evx_hash(const void *p /*rdi*/, size_t len /*rsi*/)
#   h = len * K; per 8-byte word: h = ((h ^ w) * K) ^ (.. >> 32);
#   tail (< 8 bytes) is a fault-suppressing masked load, so the function never
#   reads past the string; final murmur3 fmix64.
#   Clobbers rax, rcx, rdx, rdi, r8, k1, xmm16.
# ---------------------------------------------------------------------------
        .globl  evx_hash
        .type   evx_hash, @function
        .p2align 5
evx_hash:
        movabs  r8, 0x9E3779B97F4A7C15
        mov     rax, rsi
        imul    rax, r8
        mov     rcx, rsi
        cmp     rcx, 8
        jb      .Lh_tail
.Lh_loop:
        xor     rax, qword ptr [rdi]
        imul    rax, r8
        mov     rdx, rax
        shr     rdx, 32
        xor     rax, rdx
        add     rdi, 8
        sub     rcx, 8
        cmp     rcx, 8
        jae     .Lh_loop
.Lh_tail:
        test    rcx, rcx
        jz      .Lh_fin
        mov     rdx, -1
        bzhi    rdx, rdx, rcx
        kmovq   k1, rdx
        vmovdqu8 xmm16{k1}{z}, xmmword ptr [rdi]
        vmovq   rdx, xmm16
        xor     rax, rdx
        imul    rax, r8
        mov     rdx, rax
        shr     rdx, 32
        xor     rax, rdx
.Lh_fin:
        mov     rdx, rax
        shr     rdx, 33
        xor     rax, rdx
        movabs  rdx, 0xFF51AFD7ED558CCD
        imul    rax, rdx
        mov     rdx, rax
        shr     rdx, 33
        xor     rax, rdx
        movabs  rdx, 0xC4CEB9FE1A85EC53
        imul    rax, rdx
        mov     rdx, rax
        shr     rdx, 33
        xor     rax, rdx
        ret
        .size   evx_hash, .-evx_hash

# ---------------------------------------------------------------------------
# int evx_memeq(const void *a /*rdi*/, const void *b /*rsi*/, size_t len /*rdx*/)
#   <= 64 bytes: one masked load per side + one vpcmpb, no loop, no branch on
#   the data. Longer strings loop over 64-byte blocks.
# ---------------------------------------------------------------------------
        .globl  evx_memeq
        .type   evx_memeq, @function
        .p2align 5
evx_memeq:
        cmp     rdx, 64
        ja      .Lm_long
        mov     r11, -1
        bzhi    r11, r11, rdx
        kmovq   k1, r11
        vmovdqu8 zmm16{k1}{z}, zmmword ptr [rdi]
        vmovdqu8 zmm17{k1}{z}, zmmword ptr [rsi]
        vpcmpb  k2, zmm16, zmm17, 4
        xor     eax, eax
        kortestq k2, k2
        setz    al
        ret
.Lm_long:
        push    rdi
        push    rsi
        push    rdx
.Lm_loop:
        cmp     rdx, 64
        jb      .Lm_ltail
        vmovdqu8 zmm16, zmmword ptr [rdi]
        vpcmpb  k2, zmm16, zmmword ptr [rsi], 4
        kortestq k2, k2
        jnz     .Lm_ne
        add     rdi, 64
        add     rsi, 64
        sub     rdx, 64
        jmp     .Lm_loop
.Lm_ltail:
        mov     r11, -1
        bzhi    r11, r11, rdx
        kmovq   k1, r11
        vmovdqu8 zmm16{k1}{z}, zmmword ptr [rdi]
        vmovdqu8 zmm17{k1}{z}, zmmword ptr [rsi]
        vpcmpb  k2, zmm16, zmm17, 4
        kortestq k2, k2
        jnz     .Lm_ne
        mov     eax, 1
        jmp     .Lm_out
.Lm_ne:
        xor     eax, eax
.Lm_out:
        pop     rdx
        pop     rsi
        pop     rdi
        ret
        .size   evx_memeq, .-evx_memeq

# ---------------------------------------------------------------------------
# Key16 evx_key16(const void *p /*rdi*/, size_t len /*rsi*/) -> rax = k0, rdx = k1
#   len <= 15: one fault-suppressing masked 16-byte load (bytes stay exact),
#              length in the top byte of k1.
#   len >= 16: k0 = pointer, k1 = 0xFF << 56 | len24 << 32 | hash32.
#   Clobbers rcx, rsi, r8, k1, xmm16 (and rdi on the long path).
# ---------------------------------------------------------------------------
        .globl  evx_key16
        .type   evx_key16, @function
        .p2align 5
evx_key16:
        cmp     rsi, 15
        ja      .Lk16_long
        mov     rax, -1
        bzhi    rax, rax, rsi
        kmovq   k1, rax
        vmovdqu8 xmm16{k1}{z}, xmmword ptr [rdi]
        vmovq   rax, xmm16
        vpextrq rdx, xmm16, 1
        shl     rsi, 56
        or      rdx, rsi
        ret
.Lk16_long:
        push    rdi
        push    rsi
        call    evx_hash
        pop     rsi
        pop     rdi
        mov     edx, eax                # hash32
        mov     ecx, 0xFFFFFF
        cmp     rsi, rcx
        cmova   rsi, rcx                # len24
        shl     rsi, 32
        or      rdx, rsi
        movabs  rcx, 0xFF00000000000000
        or      rdx, rcx
        mov     rax, rdi
        ret
        .size   evx_key16, .-evx_key16

# ---------------------------------------------------------------------------
# uint64_t evx_khash(uint64_t k0 /*rdi*/, uint64_t k1 /*rsi*/)
#   Table index hash of a Key16: fmix64(k0 * K ^ k1); a long key hashes k1
#   only (k0 is a pointer, so equal long ids at different addresses must hash
#   alike). Clobbers rax and r11 only.
# ---------------------------------------------------------------------------
        .globl  evx_khash
        .type   evx_khash, @function
        .p2align 5
evx_khash:
        mov     rax, rsi
        shr     rax, 56
        cmp     eax, 0xFF
        mov     rax, rdi
        jne     .Lkh_mix
        xor     eax, eax
.Lkh_mix:
        movabs  r11, 0x9E3779B97F4A7C15
        imul    rax, r11
        xor     rax, rsi
        mov     r11, rax
        shr     r11, 33
        xor     rax, r11
        movabs  r11, 0xFF51AFD7ED558CCD
        imul    rax, r11
        mov     r11, rax
        shr     r11, 33
        xor     rax, r11
        movabs  r11, 0xC4CEB9FE1A85EC53
        imul    rax, r11
        mov     r11, rax
        shr     r11, 33
        xor     rax, r11
        ret
        .size   evx_khash, .-evx_khash

# ---------------------------------------------------------------------------
# int evx_keq(a0 /*rdi*/, a1 /*rsi*/, b0 /*rdx*/, b1 /*rcx*/)
#   Exact Key16 equality. Preserves every GPR except rax / r11.
# ---------------------------------------------------------------------------
        .globl  evx_keq
        .type   evx_keq, @function
        .p2align 5
evx_keq:
        xor     eax, eax
        cmp     rsi, rcx
        jne     .Lkq_out
        mov     r11, rsi
        shr     r11, 56
        cmp     r11d, 0xFF
        je      .Lkq_long
        cmp     rdi, rdx
        sete    al
.Lkq_out:
        ret
.Lkq_long:
        push    rsi
        push    rdx
        mov     r11, rsi
        shr     r11, 32
        and     r11d, 0xFFFFFF
        mov     rsi, rdx
        mov     rdx, r11
        call    evx_memeq               # (a ptr, b ptr, len24)
        pop     rdx
        pop     rsi
        ret
        .size   evx_keq, .-evx_keq

# ---------------------------------------------------------------------------
# const LookupEntry *evx_lookup(const LookupEntry *slots /*rdi*/, uint64_t mask /*rsi*/,
#                               uint64_t k0 /*rdx*/, uint64_t k1 /*rcx*/)
#   Linear probing from evx_khash & mask. A short key matches with two word
#   compares inside the 32-byte entry (no pointer chase); a long key falls back
#   to evx_keq. Returns NULL at an empty slot.
#   Preserves rdx, rcx; clobbers rax, rdi, rsi, r8 - r11.
# ---------------------------------------------------------------------------
        .globl  evx_lookup
        .type   evx_lookup, @function
        .p2align 5
evx_lookup:
        mov     r8, rdi                 # slots
        mov     r9, rsi                 # mask
        mov     rdi, rdx
        mov     rsi, rcx
        call    evx_khash
        mov     r10, rax
        and     r10, r9                 # idx
.Llk_loop:
        mov     rax, r10
        shl     rax, 5
        add     rax, r8
        mov     r11, qword ptr [rax + 8]
        test    r11, r11
        jz      .Llk_miss
        cmp     r11, rcx
        jne     .Llk_next
        shr     r11, 56
        cmp     r11d, 0xFF
        je      .Llk_longcmp
        cmp     qword ptr [rax], rdx
        je      .Llk_hit
        jmp     .Llk_next
.Llk_longcmp:
        push    rax
        mov     rdi, qword ptr [rax]
        mov     rsi, rcx
        call    evx_keq                 # (entry k0, k1, probe k0, k1)
        mov     r11d, eax
        pop     rax
        test    r11d, r11d
        jnz     .Llk_hit
.Llk_next:
        inc     r10
        and     r10, r9
        jmp     .Llk_loop
.Llk_miss:
        xor     eax, eax
.Llk_hit:
        ret
        .size   evx_lookup, .-evx_lookup

# ---------------------------------------------------------------------------
# int evx_split_line(const char *p /*rdi*/, size_t len /*rsi*/,
#                    uint32_t nfields /*edx*/, Slice *out /*rcx*/)
#   64-byte blocks -> comma and quote bit masks (vpcmpeqb into k-regs),
#   fields cut at each tzcnt'd comma. Quote rule without a per-byte state
#   machine: a field is "wrapped" iff len >= 2 and both ends are '"'; the line
#   is valid iff popcount(quotes) == 2 * wrapped fields.
# ---------------------------------------------------------------------------
        .globl  evx_split_line
        .type   evx_split_line, @function
        .p2align 5
evx_split_line:
        test    rsi, rsi
        jz      .Lsl_bad0
        push    rbx
        push    rbp
        push    r12
        push    r13
        push    r14
        push    r15
        mov     r8, rdi                 # block pointer
        lea     r9, [rdi + rsi]         # end
        mov     r10, rcx                # out
        mov     r11d, edx               # nfields
        mov     rbx, rdi                # current field start
        xor     ebp, ebp                # fields emitted
        xor     r12d, r12d              # quote count
        xor     r13d, r13d              # wrapped fields
        mov     eax, 0x2C
        vpbroadcastb zmm21, eax
        mov     eax, 0x22
        vpbroadcastb zmm22, eax
.Lsl_blk:
        mov     rcx, r9
        sub     rcx, r8
        jbe     .Lsl_last
        cmp     rcx, 64
        jb      .Lsl_part
        vmovdqu8 zmm16, zmmword ptr [r8]
        jmp     .Lsl_cmp
.Lsl_part:
        mov     rax, -1
        bzhi    rax, rax, rcx
        kmovq   k1, rax
        vmovdqu8 zmm16{k1}{z}, zmmword ptr [r8]
.Lsl_cmp:
        vpcmpeqb k2, zmm16, zmm21
        vpcmpeqb k3, zmm16, zmm22
        kmovq   r14, k2
        kmovq   rax, k3
        popcnt  rax, rax
        add     r12, rax
.Lsl_comma:
        test    r14, r14
        jz      .Lsl_nextblk
        tzcnt   rax, r14
        lea     r15, [r8 + rax]         # field end = comma
        call    .Lsl_emit
        test    eax, eax
        jz      .Lsl_badpop
        lea     rbx, [r15 + 1]
        blsr    r14, r14
        jmp     .Lsl_comma
.Lsl_nextblk:
        add     r8, 64
        jmp     .Lsl_blk
.Lsl_last:
        mov     r15, r9
        call    .Lsl_emit
        test    eax, eax
        jz      .Lsl_badpop
        cmp     ebp, r11d
        jne     .Lsl_badpop
        lea     rax, [r13 + r13]
        cmp     rax, r12
        jne     .Lsl_badpop
        mov     eax, 1
        jmp     .Lsl_ret
.Lsl_badpop:
        xor     eax, eax
.Lsl_ret:
        pop     r15
        pop     r14
        pop     r13
        pop     r12
        pop     rbp
        pop     rbx
        ret
.Lsl_bad0:
        xor     eax, eax
        ret

# local: emit field [rbx, r15) into out[rbp]; eax = 0 when too many fields
.Lsl_emit:
        cmp     ebp, r11d
        jae     .Lsl_em_bad
        mov     rsi, rbx
        mov     rdx, r15
        sub     rdx, rsi                # len
        cmp     rdx, 2
        jb      .Lsl_em_store
        cmp     byte ptr [rsi], 0x22
        jne     .Lsl_em_store
        cmp     byte ptr [r15 - 1], 0x22
        jne     .Lsl_em_store
        inc     rsi
        sub     rdx, 2
        inc     r13d
.Lsl_em_store:
        mov     eax, ebp
        shl     rax, 4
        mov     qword ptr [r10 + rax], rsi
        mov     dword ptr [r10 + rax + 8], edx
        mov     dword ptr [r10 + rax + 12], 0
        inc     ebp
        mov     eax, 1
        ret
.Lsl_em_bad:
        xor     eax, eax
        ret
        .size   evx_split_line, .-evx_split_line

# ---------------------------------------------------------------------------
# int evx_parse_ts(const char *p /*rdi*/, size_t len /*rsi*/, int64_t *out /*rdx*/)
#   len 20 + 'Z' at 19           -> F1
#   len 25 + '+'/'-' at 19       -> F2
#   anything else                -> F3 (ASCII digits, overflow-checked)
#   F1/F2 date-time part: one masked 32-byte load, template compare for the
#   separators and an unsigned <= 9 compare for the digits (two k-masks),
#   then vpshufb gathers the 7 digit pairs and vpmaddubsw (x10, x1) turns
#   them into 7 two-digit words in one instruction.
#   Clobbers rax, rcx, rdx, rsi, rdi, r8-r11, k1-k3, ymm16-ymm19.
# ---------------------------------------------------------------------------
        .globl  evx_parse_ts
        .type   evx_parse_ts, @function
        .p2align 5
evx_parse_ts:
        mov     r11, rdx                # out
        xor     r10d, r10d              # offset in ms
        cmp     rsi, 20
        je      .Lts_f1
        cmp     rsi, 25
        je      .Lts_f2
        jmp     .Lts_f3
.Lts_f1:
        cmp     byte ptr [rdi + 19], 0x5A   # 'Z'
        jne     .Lts_f3
        jmp     .Lts_dt
.Lts_f2:
        movzx   eax, byte ptr [rdi + 19]
        cmp     al, 0x2B                # '+'
        je      .Lts_off
        cmp     al, 0x2D                # '-'
        jne     .Lts_f3
.Lts_off:
        cmp     byte ptr [rdi + 22], 0x3A   # ':'
        jne     .Lts_bad
        movzx   ecx, byte ptr [rdi + 20]
        sub     ecx, 0x30
        cmp     ecx, 9
        ja      .Lts_bad
        movzx   edx, byte ptr [rdi + 21]
        sub     edx, 0x30
        cmp     edx, 9
        ja      .Lts_bad
        lea     ecx, [rcx + rcx*4]
        lea     ecx, [rdx + rcx*2]      # offset hours
        cmp     ecx, 23
        ja      .Lts_bad
        movzx   edx, byte ptr [rdi + 23]
        sub     edx, 0x30
        cmp     edx, 9
        ja      .Lts_bad
        movzx   r8d, byte ptr [rdi + 24]
        sub     r8d, 0x30
        cmp     r8d, 9
        ja      .Lts_bad
        lea     edx, [rdx + rdx*4]
        lea     edx, [r8 + rdx*2]       # offset minutes
        cmp     edx, 59
        ja      .Lts_bad
        imul    ecx, ecx, 60
        add     ecx, edx                # total minutes
        imul    r10, rcx, 60000
        cmp     al, 0x2D
        jne     .Lts_dt
        neg     r10
.Lts_dt:
        mov     eax, 0x7FFFF            # 19 bytes of date-time
        kmovd   k1, eax
        vmovdqu8 ymm16{k1}{z}, ymmword ptr [rdi]
        vpcmpeqb k2, ymm16, ymmword ptr [rip + .Lts_tmpl]
        vpsubb  ymm17, ymm16, ymmword ptr [rip + .Lts_zero]
        vpcmpub k3, ymm17, ymmword ptr [rip + .Lts_nine], 2
        kmovd   eax, k2
        kmovd   ecx, k3
        and     eax, 0x12490            # separator positions 4,7,10,13,16
        and     ecx, 0x6DB6F            # digit positions
        or      eax, ecx
        cmp     eax, 0x7FFFF
        jne     .Lts_bad
        vpshufb ymm18, ymm17, ymmword ptr [rip + .Lts_shuf]
        vpmaddubsw ymm19, ymm18, ymmword ptr [rip + .Lts_w101]
        vmovdqu64 ymmword ptr [rsp - 40], ymm19       # red zone
        movzx   eax, word ptr [rsp - 40]                # YY (hi)
        movzx   ecx, word ptr [rsp - 38]                # YY (lo)
        movzx   r8d, word ptr [rsp - 36]                # month
        movzx   r9d, word ptr [rsp - 34]                # day
        # month 1..12
        lea     edx, [r8 - 1]
        cmp     edx, 11
        ja      .Lts_bad
        # day 1..days_in_month
        test    r9d, r9d
        jz      .Lts_bad
        lea     rsi, [rip + .Lts_dim]
        movzx   edx, byte ptr [rsi + r8 - 1]
        cmp     r8d, 2
        jne     .Lts_dimok
        # leap: lo % 4 == 0 && (lo != 0 || hi % 4 == 0)
        test    ecx, 3
        jnz     .Lts_dimok
        test    ecx, ecx
        jnz     .Lts_leap
        test    eax, 3
        jnz     .Lts_dimok
.Lts_leap:
        inc     edx
.Lts_dimok:
        cmp     r9d, edx
        ja      .Lts_bad
        imul    eax, eax, 100
        add     eax, ecx                # year
        # Hinnant days_from_civil with y shifted by +400 (one era) so every
        # division is of a non-negative value; divisions are reciprocal mults.
        mov     ecx, r8d                # m
        cmp     ecx, 2
        ja      .Lts_m3
        dec     eax                     # y -= 1
        add     ecx, 9                  # mp = m + 9
        jmp     .Lts_mdone
.Lts_m3:
        sub     ecx, 3                  # mp = m - 3
.Lts_mdone:
        add     eax, 400                # y >= 0
        mov     edx, eax
        imul    rdx, rdx, 5243
        shr     rdx, 21                 # era = y / 400
        imul    esi, edx, 400
        sub     eax, esi                # yoe = y - era * 400
        imul    rdx, rdx, 146097        # era * 146097
        imul    ecx, ecx, 153
        add     ecx, 2
        imul    rcx, rcx, 52429
        shr     rcx, 18                 # (153 * mp + 2) / 5
        add     ecx, r9d
        dec     ecx                     # doy
        imul    esi, eax, 365
        add     ecx, esi
        mov     esi, eax
        shr     esi, 2
        add     ecx, esi                # + yoe / 4
        mov     esi, eax
        imul    rsi, rsi, 5243
        shr     rsi, 19                 # yoe / 100
        sub     ecx, esi                # doe
        add     rdx, rcx
        sub     rdx, 865565             # 719468 + 146097
        # time of day
        movzx   eax, word ptr [rsp - 32]                # hour
        movzx   ecx, word ptr [rsp - 30]                # minute
        movzx   esi, word ptr [rsp - 24]                # second
        cmp     eax, 23
        ja      .Lts_bad
        cmp     ecx, 59
        ja      .Lts_bad
        cmp     esi, 59
        ja      .Lts_bad
        imul    rdx, rdx, 86400
        imul    eax, eax, 3600
        add     rdx, rax
        imul    ecx, ecx, 60
        add     rdx, rcx
        add     rdx, rsi
        imul    rdx, rdx, 1000
        sub     rdx, r10                # UTC = local - offset
        mov     qword ptr [r11], rdx
        mov     eax, 1
        ret
.Lts_f3:
        test    rsi, rsi
        jz      .Lts_bad
        add     rsi, rdi                # end
        xor     eax, eax
.Lts_f3loop:
        movzx   ecx, byte ptr [rdi]
        sub     ecx, 0x30
        cmp     ecx, 9
        ja      .Lts_bad
        imul    rax, rax, 10            # OF = value no longer fits int64
        jo      .Lts_bad
        add     rax, rcx
        jo      .Lts_bad
        inc     rdi
        cmp     rdi, rsi
        jb      .Lts_f3loop
        mov     qword ptr [r11], rax
        mov     eax, 1
        ret
.Lts_bad:
        xor     eax, eax
        ret
        .size   evx_parse_ts, .-evx_parse_ts

        .section .rodata
        .p2align 5
# "YYYY-MM-DDTHH:MM:SS": separators at 4,7 '-', 10 'T', 13,16 ':'
.Lts_tmpl:
        .byte 0,0,0,0,0x2D,0,0,0x2D,0,0,0x54,0,0,0x3A,0,0
        .byte 0x3A,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0
.Lts_zero:
        .fill 32,1,0x30
.Lts_nine:
        .fill 32,1,9
# lane 0: pairs (0,1)(2,3)(5,6)(8,9)(11,12)(14,15) -> words 0..5
# lane 1: pair (17,18) = lane bytes (1,2)            -> word 8
.Lts_shuf:
        .byte 0,1,2,3,5,6,8,9,11,12,14,15,0x80,0x80,0x80,0x80
        .byte 1,2,0x80,0x80,0x80,0x80,0x80,0x80,0x80,0x80,0x80,0x80,0x80,0x80,0x80,0x80
.Lts_w101:
        .rept 16
        .byte 10,1
        .endr
.Lts_dim:
        .byte 31,28,31,30,31,30,31,31,30,31,30,31

        .section .note.GNU-stack,"",@progbits
