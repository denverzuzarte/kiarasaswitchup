# evx_parse.s - batch event-row parser (GNU as, Intel syntax).
#
# uint64_t evx_parse_events(const ParseCtx *ctx /*rdi*/, const char *begin /*rsi*/,
#                           const char *end /*rdx*/, Cand *out /*rcx*/)
#
# One call per newline-aligned byte range (no header). For every line:
#   1. 64-byte blocks: newline / comma / quote k-masks in one pass. The newline
#      bound truncates the comma and quote masks with bzhi; a virtual newline
#      at the range end handles a last line without '\n'. The final partial
#      block is a fault-suppressing masked load, so nothing past `end` is read.
#   2. Exactly 6 commas, quote count == 2 * wrapped fields, else malformed.
#   3. Empty event_id -> dropped (before dedup).
#   4. Every other row becomes a 56-byte Cand with event_id and user_id as
#      inline 16-byte keys (built while the line is in L1). All validation (user, ts, window,
#      type, product, price, late rule) is evaluated here, once, as a flag; the
#      row still competes in dedup whatever the flag says (rules: dedup first).
#   5. Hash lookups (links, products) run only for rows that are still valid,
#      and the product lookup only for purchases (only purchases need a
#      category). The person is NOT resolved here: only dedup winners need
#      it, so evx_collect does the link lookup (fewer lookups, smaller Cand).
#
# Stack frame (rsp-relative):
#    0 ..  55  comma pointers [7]
#   64 .. 175  field slices [7] (ptr, len) x 16 bytes
#  176         ts value
#  184         ingest value
#  192         line end
#  200         price

        .intel_syntax noprefix
        .text

        .set    F_EID,    64
        .set    F_USER,   80
        .set    F_TS,     96
        .set    F_ING,   112
        .set    F_TYPE,  128
        .set    F_PROD,  144
        .set    F_PRICE, 160
        .set    L_TS,    176
        .set    L_ING,   184
        .set    L_END,   192
        .set    L_PRICE, 200
        .set    L_PO,    208            # PartOut* (NULL = off)
        .set    L_ENT,   216            # next compact entry (u64)
        .set    L_PID,   224            # next partition id (u16)
        .set    FRAME,   232

        .globl  evx_parse_events
        .type   evx_parse_events, @function
        .p2align 5
evx_parse_events:
        push    rbx
        push    rbp
        push    r12
        push    r13
        push    r14
        push    r15
        sub     rsp, FRAME
        mov     qword ptr [rsp + L_PO], r8
        test    r8, r8
        jz      .Lpo_off
        mov     rax, qword ptr [r8 + 8]         # PartOut.ent
        mov     qword ptr [rsp + L_ENT], rax
        mov     rax, qword ptr [r8 + 16]        # PartOut.pid
        mov     qword ptr [rsp + L_PID], rax
.Lpo_off:
        mov     r12, rdi                # ctx
        mov     r13, rsi                # cursor (line start)
        mov     r14, rdx                # end
        mov     r15, rcx                # out cursor
        mov     rbx, rcx                # out base
        mov     eax, 0x0A
        vpbroadcastb zmm20, eax
        mov     eax, 0x2C
        vpbroadcastb zmm21, eax
        mov     eax, 0x22
        vpbroadcastb zmm22, eax

.Lline:
        cmp     r13, r14
        jae     .Ldone
        mov     rbp, r13                # line start
        mov     r8, r13                 # block pointer
        xor     r9d, r9d                # comma count
        xor     r10d, r10d              # quote count

.Lscan:
        mov     rcx, r14
        sub     rcx, r8                 # remaining bytes
        cmp     rcx, 64
        jb      .Lpartial
        vmovdqu8 zmm16, zmmword ptr [r8]
        vpcmpeqb k1, zmm16, zmm20
        vpcmpeqb k2, zmm16, zmm21
        vpcmpeqb k3, zmm16, zmm22
        kmovq   rax, k1
        kmovq   rdx, k2
        kmovq   r11, k3
        test    rax, rax
        jnz     .Lhas_nl
        popcnt  r11, r11
        add     r10, r11
.Lc_full:
        test    rdx, rdx
        jz      .Lc_full_done
        cmp     r9d, 7
        jae     .Lc_full_skip
        tzcnt   rax, rdx
        add     rax, r8
        mov     qword ptr [rsp + r9*8], rax
.Lc_full_skip:
        inc     r9d
        blsr    rdx, rdx
        jmp     .Lc_full
.Lc_full_done:
        add     r8, 64
        jmp     .Lscan

.Lpartial:
        mov     rax, -1
        bzhi    rax, rax, rcx
        kmovq   k4, rax
        vmovdqu8 zmm16{k4}{z}, zmmword ptr [r8]
        vpcmpeqb k1, zmm16, zmm20
        vpcmpeqb k2, zmm16, zmm21
        vpcmpeqb k3, zmm16, zmm22
        kmovq   rax, k1
        kmovq   rdx, k2
        kmovq   r11, k3
        bts     rax, rcx                # virtual newline at the range end
.Lhas_nl:
        tzcnt   rsi, rax                # newline position in block
        mov     rax, -1
        bzhi    rax, rax, rsi
        and     rdx, rax
        and     r11, rax
        popcnt  r11, r11
        add     r10, r11
.Lc_last:
        test    rdx, rdx
        jz      .Lc_last_done
        cmp     r9d, 7
        jae     .Lc_last_skip
        tzcnt   rax, rdx
        add     rax, r8
        mov     qword ptr [rsp + r9*8], rax
.Lc_last_skip:
        inc     r9d
        blsr    rdx, rdx
        jmp     .Lc_last
.Lc_last_done:
        lea     rax, [r8 + rsi]         # line end (the '\n' or `end`)
        mov     qword ptr [rsp + L_END], rax
        lea     r13, [rax + 1]          # next line

        cmp     r9d, 6
        jne     .Lline                  # wrong field count (also: empty line)

        # ---- cut the 7 fields, unwrapping quotes ---------------------------
        mov     rcx, rbp                # field start
        xor     r11d, r11d              # wrapped fields
        xor     r9d, r9d                # field index
.Lfield:
        cmp     r9d, 6
        je      .Lfield_lastend
        mov     rdx, qword ptr [rsp + r9*8]
        jmp     .Lfield_cut
.Lfield_lastend:
        mov     rdx, qword ptr [rsp + L_END]
.Lfield_cut:
        mov     rax, rdx
        sub     rax, rcx                # len
        mov     rsi, rcx                # value start
        cmp     rax, 2
        jb      .Lfield_store
        cmp     byte ptr [rcx], 0x22
        jne     .Lfield_store
        cmp     byte ptr [rdx - 1], 0x22
        jne     .Lfield_store
        inc     rsi
        sub     rax, 2
        inc     r11d
.Lfield_store:
        mov     r8d, r9d
        shl     r8d, 4
        mov     qword ptr [rsp + r8 + 64], rsi
        mov     qword ptr [rsp + r8 + 72], rax
        lea     rcx, [rdx + 1]
        inc     r9d
        cmp     r9d, 7
        jb      .Lfield
        add     r11, r11
        cmp     r10, r11
        jne     .Lline                  # stray quote -> malformed

        # ---- event_id ------------------------------------------------------
        mov     rsi, qword ptr [rsp + F_EID + 8]
        test    rsi, rsi
        jz      .Lline                  # empty event_id: dropped before dedup
        mov     rdi, qword ptr [rsp + F_EID]
        call    evx_key16                       # inline key from the hot line
        mov     qword ptr [r15], rax
        mov     qword ptr [r15 + 8], rdx
        mov     rdi, rax
        mov     rsi, rdx
        call    evx_khash
        mov     dword ptr [r15 + 52], eax       # eid_h32
        mov     rdx, qword ptr [rsp + L_PO]
        test    rdx, rdx
        jz      .Lnohist
        # compact dedup reference: (h32 & 0xFFFF) << 48 | Cand*, partition id
        movzx   ecx, ax
        shl     rcx, 48
        or      rcx, r15
        mov     r8, qword ptr [rsp + L_ENT]
        mov     qword ptr [r8], rcx
        add     qword ptr [rsp + L_ENT], 8
        mov     ecx, dword ptr [rdx + 24]       # PartOut.shift
        shr     eax, cl
        mov     r8, qword ptr [rsp + L_PID]
        mov     word ptr [r8], ax
        add     qword ptr [rsp + L_PID], 2
        mov     rdx, qword ptr [rdx]            # PartOut.hist
        inc     dword ptr [rdx + rax*4]         # rows per dedup partition
.Lnohist:

        # ---- ingest_ts (always F1 per rules; defensive) --------------------
        mov     rdi, qword ptr [rsp + F_ING]
        mov     rsi, qword ptr [rsp + F_ING + 8]
        lea     rdx, [rsp + L_ING]
        call    evx_parse_ts
        test    eax, eax
        jz      .Lbad_ingest
        mov     rax, qword ptr [rsp + L_ING]
        mov     qword ptr [r15 + 16], rax

        # ---- validation of this copy (applied only if it wins dedup) -------
        cmp     qword ptr [rsp + F_USER + 8], 0
        je      .Linvalid
        cmp     qword ptr [rsp + F_PROD + 8], 0
        je      .Linvalid
        mov     rdi, qword ptr [rsp + F_TS]
        mov     rsi, qword ptr [rsp + F_TS + 8]
        lea     rdx, [rsp + L_TS]
        call    evx_parse_ts
        test    eax, eax
        jz      .Linvalid
        mov     rax, qword ptr [rsp + L_TS]
        cmp     rax, qword ptr [r12]            # window_start <= ts
        jl      .Linvalid
        cmp     rax, qword ptr [r12 + 8]        # ts < window_end
        jge     .Linvalid
        mov     rdx, qword ptr [r15 + 16]
        sub     rdx, rax
        cmp     rdx, 86400000                   # late: ingest - ts > 24 h
        jg      .Linvalid
        sub     rax, qword ptr [r12]
        mov     qword ptr [r15 + 40], rax       # ts_off

        # event_type: compare the whole value as one 4- or 8-byte integer
        mov     rdi, qword ptr [rsp + F_TYPE]
        mov     rsi, qword ptr [rsp + F_TYPE + 8]
        cmp     rsi, 4
        jne     .Ltype8
        mov     eax, dword ptr [rdi]
        cmp     eax, 0x77656976                 # "view"
        je      .Lnonpurchase
        cmp     eax, 0x74726163                 # "cart"
        je      .Lnonpurchase
        jmp     .Linvalid
.Ltype8:
        cmp     rsi, 8
        jne     .Linvalid
        movabs  rdx, 0x6573616863727570         # "purchase"
        cmp     qword ptr [rdi], rdx
        jne     .Linvalid

        # purchase price: ASCII digits only, 1 ..= 10,000,000, leading zeros ok
        mov     rdi, qword ptr [rsp + F_PRICE]
        mov     rsi, qword ptr [rsp + F_PRICE + 8]
        test    rsi, rsi
        jz      .Linvalid
        add     rsi, rdi
        xor     eax, eax
.Lprice:
        movzx   ecx, byte ptr [rdi]
        sub     ecx, 0x30
        cmp     ecx, 9
        ja      .Linvalid
        imul    eax, eax, 10
        add     eax, ecx
        cmp     eax, 10000000
        ja      .Linvalid
        inc     rdi
        cmp     rdi, rsi
        jb      .Lprice
        test    eax, eax
        jz      .Linvalid
        mov     dword ptr [rsp + L_PRICE], eax

        # category (only purchases need one)
        mov     eax, dword ptr [r12 + 48]       # unknown_cat
        mov     dword ptr [r15 + 48], eax
        cmp     qword ptr [r12 + 32], 0
        je      .Lcat_done
        mov     rdi, qword ptr [rsp + F_PROD]
        mov     rsi, qword ptr [rsp + F_PROD + 8]
        call    evx_key16
        mov     rcx, rdx
        mov     rdx, rax
        mov     rdi, qword ptr [r12 + 32]
        mov     rsi, qword ptr [r12 + 40]
        call    evx_lookup
        test    rax, rax
        jz      .Lcat_done
        mov     eax, dword ptr [rax + 16]       # val.k0 = category id
        mov     dword ptr [r15 + 48], eax
.Lcat_done:
        mov     eax, dword ptr [r15 + 48]
        shl     eax, 24
        or      eax, dword ptr [rsp + L_PRICE]
        mov     dword ptr [r15 + 48], eax
        jmp     .Lvalid

.Lnonpurchase:
        mov     dword ptr [r15 + 48], 0

        # ---- valid: keep the raw user id; the person is resolved in collect,
        #      for winning copies only -----------------------------------------
.Lvalid:
        mov     rdi, qword ptr [rsp + F_USER]
        mov     rsi, qword ptr [rsp + F_USER + 8]
        call    evx_key16                       # never null for a non-empty id
        mov     qword ptr [r15 + 24], rax
        mov     qword ptr [r15 + 32], rdx
        add     r15, 56
        jmp     .Lline

.Lbad_ingest:
        movabs  rax, 0x7FFFFFFFFFFFFFFF         # never beats a real copy
        mov     qword ptr [r15 + 16], rax
.Linvalid:
        mov     qword ptr [r15 + 32], 0         # user.k1 == 0: invalid copy
        add     r15, 56
        jmp     .Lline

.Ldone:
        mov     rax, r15
        sub     rax, rbx
        xor     edx, edx
        mov     ecx, 56
        div     rcx                     # Cand count
        add     rsp, FRAME
        pop     r15
        pop     r14
        pop     r13
        pop     r12
        pop     rbp
        pop     rbx
        ret
        .size   evx_parse_events, .-evx_parse_events

        .section .note.GNU-stack,"",@progbits
