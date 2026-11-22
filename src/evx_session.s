# evx_session.s - per-person ordering, sessionization and user/day aggregation.
# (GNU as, Intel syntax)
#
# uint64_t evx_sessionize(SessCtx *ctx /*rdi*/, uint64_t pbegin /*rsi*/,
#                         uint64_t pend /*rdx*/, Row *out /*rcx*/)
#
# For each person slot p in [pbegin, pend) with events [off[p], off[p+1]):
#   sort the 16-byte Ev records by ts_off
#     n <= 32 : insertion sort (in-register key, 16-byte moves)
#     n >  32 : LSD radix, 8-bit digits, `radix_passes` digits; all digit
#               histograms come from ONE read of the data; a pass whose digit
#               is the same for every key is skipped.
#   scan: gap > 1,800,000 ms opens a session; a session whose start falls on a
#         new UTC day flushes the user/day row first. Equal timestamps are
#         adjacent after the sort, so their gap is 0 and they always share.
#   per-day category revenue: cat_sum[256] plus a 256-bit touched bitmap;
#         emission walks only touched categories (tzcnt / blsr) in ascending id
#         order, ids being byte-order ranks, so a strict '>' gives the
#         byte-wise smallest name on ties. The walk also clears the state.

        .intel_syntax noprefix
        .text

        .set    C_OFFS,    0
        .set    C_EVENTS,  8
        .set    C_SCRATCH, 16
        .set    C_DAY0,    24
        .set    C_BASE,    32
        .set    C_PASSES,  40
        .set    C_CATSUM,  48
        .set    C_TOUCHED, 2096

        .set    HIST,      8192         # 8 digits x 256 x u32
        .set    FRAME,     HIST + 8

        .globl  evx_sessionize
        .type   evx_sessionize, @function
        .p2align 5
evx_sessionize:
        push    rbx
        push    rbp
        push    r12
        push    r13
        push    r14
        push    r15
        sub     rsp, FRAME
        mov     rbx, rdi                # ctx
        mov     r12, rsi                # p
        mov     r13, rdx                # pend
        mov     r14, rcx                # out cursor
        mov     qword ptr [rsp + HIST], rcx   # out base
.Lse_person:
        cmp     r12, r13
        jae     .Lse_done
        mov     rax, qword ptr [rbx + C_OFFS]
        mov     rdi, qword ptr [rax + r12*8]
        mov     rsi, qword ptr [rax + r12*8 + 8]
        mov     r15, r12                # pid
        inc     r12
        sub     rsi, rdi                # n
        jz      .Lse_person
        shl     rdi, 4
        add     rdi, qword ptr [rbx + C_EVENTS]     # first Ev
        push    rdi
        push    rsi
        call    .Lsort
        pop     rsi
        pop     rdi
        shl     rsi, 4
        add     rsi, rdi                # end

        # first event opens the first session
        mov     r8, qword ptr [rdi]     # prev ts
        mov     rax, r8
        call    .Lday
        mov     r9, rax                 # current day
        mov     r10d, 1                 # sessions
        xor     r11d, r11d              # events
        xor     ebp, ebp                # revenue
.Lse_ev:
        mov     rax, qword ptr [rdi]
        mov     rdx, rax
        sub     rdx, r8
        cmp     rdx, 1800000
        jle     .Lse_same
        # new session
        mov     r8, rax
        call    .Lday
        cmp     rax, r9
        je      .Lse_sameday
        push    rax
        call    .Lemit
        pop     r9
        xor     r10d, r10d
        xor     r11d, r11d
        xor     ebp, ebp
.Lse_sameday:
        inc     r10d
.Lse_same:
        mov     r8, qword ptr [rdi]
        inc     r11
        mov     eax, dword ptr [rdi + 8]    # price | cat << 24
        mov     edx, eax
        and     edx, 0xFFFFFF
        jz      .Lse_nopurch
        add     rbp, rdx
        shr     eax, 24
        add     qword ptr [rbx + C_CATSUM + rax*8], rdx
        mov     ecx, eax
        shr     ecx, 6
        and     eax, 63
        bts     qword ptr [rbx + C_TOUCHED + rcx*8], rax
.Lse_nopurch:
        add     rdi, 16
        cmp     rdi, rsi
        jb      .Lse_ev
        call    .Lemit
        jmp     .Lse_person
.Lse_done:
        mov     rax, r14
        sub     rax, qword ptr [rsp + HIST]
        shr     rax, 5
        add     rsp, FRAME
        pop     r15
        pop     r14
        pop     r13
        pop     r12
        pop     rbp
        pop     rbx
        ret

# local: rax = ts_off -> rax = UTC day number. Clobbers rcx, rdx.
.Lday:
        add     rax, qword ptr [rbx + C_BASE]
        xor     edx, edx
        mov     ecx, 86400000
        div     rcx
        add     rax, qword ptr [rbx + C_DAY0]
        ret

# local: write Row {r15 pid, r9 day, r10 sessions, r11 events, rbp revenue}.
# Clobbers rax, rcx, rdx; preserves rdi, rsi.
.Lemit:
        mov     dword ptr [r14], r15d
        mov     dword ptr [r14 + 4], r9d
        mov     dword ptr [r14 + 8], r10d
        mov     dword ptr [r14 + 12], -1
        mov     qword ptr [r14 + 16], r11
        mov     qword ptr [r14 + 24], rbp
        test    rbp, rbp
        jz      .Lem_out                # no purchase: top_category NULL
        push    rdi
        push    rsi
        mov     edi, -1                 # best cat
        xor     esi, esi                # best sum
        xor     ecx, ecx                # word index
.Lem_word:
        mov     rax, qword ptr [rbx + C_TOUCHED + rcx*8]
        mov     qword ptr [rbx + C_TOUCHED + rcx*8], 0
.Lem_bit:
        test    rax, rax
        jz      .Lem_wnext
        tzcnt   rdx, rax
        blsr    rax, rax
        push    rax
        mov     eax, ecx
        shl     eax, 6
        add     edx, eax                # category id
        mov     rax, qword ptr [rbx + C_CATSUM + rdx*8]
        mov     qword ptr [rbx + C_CATSUM + rdx*8], 0
        cmp     rax, rsi
        jbe     .Lem_notbest
        mov     rsi, rax
        mov     edi, edx
.Lem_notbest:
        pop     rax
        jmp     .Lem_bit
.Lem_wnext:
        inc     ecx
        cmp     ecx, 4
        jb      .Lem_word
        mov     dword ptr [r14 + 12], edi
        pop     rsi
        pop     rdi
.Lem_out:
        add     r14, 32
        ret

# local: sort Ev[n] at rdi (n in rsi) by ts_off.
# Uses ctx (rbx) scratch + radix_passes and the caller frame histogram,
# which sits at [rsp + 8 + 16 + 8] relative to this routine's entry rsp
# (return address, two saved args, then the evx_sessionize frame).
# Clobbers rax, rcx, rdx, rdi, rsi, r8-r11 (callers save what they need).
.Lsort:
        cmp     rsi, 32
        ja      .Lradix
        # ---- insertion sort ------------------------------------------------
        mov     r8d, 1                  # i
.Lis_outer:
        cmp     r8, rsi
        jae     .Lis_done
        mov     r9, r8
        shl     r9, 4
        add     r9, rdi                 # &a[i]
        mov     r10, qword ptr [r9]     # key
        mov     r11, qword ptr [r9 + 8]
.Lis_inner:
        cmp     r9, rdi
        jbe     .Lis_place
        cmp     qword ptr [r9 - 16], r10
        jle     .Lis_place
        mov     rax, qword ptr [r9 - 16]
        mov     rdx, qword ptr [r9 - 8]
        mov     qword ptr [r9], rax
        mov     qword ptr [r9 + 8], rdx
        sub     r9, 16
        jmp     .Lis_inner
.Lis_place:
        mov     qword ptr [r9], r10
        mov     qword ptr [r9 + 8], r11
        inc     r8
        jmp     .Lis_outer
.Lis_done:
        ret

.Lradix:
        push    rbp
        push    r12
        push    r13
        push    r14
        push    r15
        # histogram base: entry rsp + 8 (ret) + 16 (args) + 8 (ret of
        # evx_sessionize's call is not on this path; frame starts right
        # above the two pushed args) ... computed explicitly:
        lea     r15, [rsp + 40 + 8 + 16]        # 5 pushes, ret, 2 args
        mov     r12, rdi                # src
        mov     r13, qword ptr [rbx + C_SCRATCH]   # dst
        mov     r14, rsi                # n
        mov     ebp, dword ptr [rbx + C_PASSES]
        # zero passes * 256 counters
        mov     rdi, r15
        mov     ecx, ebp
        shl     ecx, 8
        xor     eax, eax
        rep stosd
        # one read, all digit histograms
        mov     rdi, r12
        mov     rsi, r14
        shl     rsi, 4
        add     rsi, rdi
.Lrx_hist:
        mov     rax, qword ptr [rdi]
        xor     ecx, ecx
.Lrx_hd:
        movzx   edx, al
        mov     r8d, ecx
        shl     r8d, 8
        add     edx, r8d
        inc     dword ptr [r15 + rdx*4]
        shr     rax, 8
        inc     ecx
        cmp     ecx, ebp
        jb      .Lrx_hd
        add     rdi, 16
        cmp     rdi, rsi
        jb      .Lrx_hist
        # passes
        xor     ecx, ecx                # digit
        mov     r11, r12                # original array
.Lrx_pass:
        cmp     ecx, ebp
        jae     .Lrx_fin
        mov     r9d, ecx
        shl     r9d, 10
        add     r9, r15                 # this digit's 256 counters
        # skip when every key has the same digit
        mov     rax, qword ptr [r12]
        mov     edx, ecx
        shl     edx, 3
        push    rcx
        mov     ecx, edx
        shr     rax, cl
        pop     rcx
        movzx   eax, al
        cmp     dword ptr [r9 + rax*4], r14d
        je      .Lrx_skip
        # exclusive prefix sum
        xor     eax, eax
        xor     r8d, r8d
.Lrx_pref:
        mov     r10d, dword ptr [r9 + r8*4]
        mov     dword ptr [r9 + r8*4], eax
        add     eax, r10d
        inc     r8d
        cmp     r8d, 256
        jb      .Lrx_pref
        # scatter src -> dst
        mov     rdi, r12
        mov     rsi, r14
        shl     rsi, 4
        add     rsi, rdi
        push    rcx
        shl     ecx, 3                  # shift = digit * 8
.Lrx_sc:
        mov     rax, qword ptr [rdi]
        mov     rdx, rax
        shr     rdx, cl
        movzx   edx, dl
        mov     r8d, dword ptr [r9 + rdx*4]
        inc     dword ptr [r9 + rdx*4]
        shl     r8, 4
        mov     rdx, qword ptr [rdi + 8]
        mov     qword ptr [r13 + r8], rax
        mov     qword ptr [r13 + r8 + 8], rdx
        add     rdi, 16
        cmp     rdi, rsi
        jb      .Lrx_sc
        pop     rcx
        xchg    r12, r13
.Lrx_skip:
        inc     ecx
        jmp     .Lrx_pass
.Lrx_fin:
        cmp     r12, r11
        je      .Lrx_out
        # result is in scratch: copy back
        mov     rsi, r12
        mov     rdi, r11
        mov     rcx, r14
        shl     rcx, 1
        rep movsq
.Lrx_out:
        pop     r15
        pop     r14
        pop     r13
        pop     r12
        pop     rbp
        ret
        .size   evx_sessionize, .-evx_sessionize

        .section .note.GNU-stack,"",@progbits
