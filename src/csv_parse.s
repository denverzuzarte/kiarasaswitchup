    .text
    .globl csv_parse_line_asm
    .type csv_parse_line_asm, @function

/*
 * System V AMD64 ABI:
 *
 * rdi = pointer to record
 * rsi = record length
 * edx = expected field count
 * rcx = output Slice array
 *
 * Slice:
 *   +0  const char *ptr
 *   +8  uint32_t len
 *   +12 padding
 *
 * sizeof(Slice) = 16 bytes
 *
 * Returns:
 *   eax = 1 valid
 *   eax = 0 invalid
 *
 * No allocations.
 * No input modifications.
 */

csv_parse_line_asm:

    testq   %rdi, %rdi
    jz      .Lbad

    testq   %rcx, %rcx
    jz      .Lbad

    testl   %edx, %edx
    jz      .Lbad

    testq   %rsi, %rsi
    jz      .Lbad

    leaq    (%rdi,%rsi), %r8
    movq    %rdi, %r9
    movq    %rcx, %r10
    movl    %edx, %r11d


.Lfield:

    cmpq    %r8, %r9
    ja      .Lbad

    je      .Lempty_at_end

    cmpb    $'"', (%r9)
    jne     .Lunquoted


/*
 * Quoted field.
 */

    incq    %r9
    movq    %r9, %rax


.Lquoted_scan:

    cmpq    %r8, %r9
    jae     .Lbad

    cmpb    $'"', (%r9)
    je      .Lquoted_end

    cmpb    $',', (%r9)
    je      .Lbad

    incq    %r9
    jmp     .Lquoted_scan


.Lquoted_end:

    movq    %r9, %rsi
    subq    %rax, %rsi

    movl    $0xffffffff, %edx
    cmpq    %rdx, %rsi
    ja      .Lbad

    movq    %rax, 0(%r10)
    movl    %esi, 8(%r10)
    movl    $0, 12(%r10)

    incq    %r9
    jmp     .Lafter_field


/*
 * Unquoted field.
 */

.Lunquoted:

    movq    %r9, %rax


.Lunquoted_scan:

    cmpq    %r8, %r9
    jae     .Lunquoted_end

    cmpb    $'"', (%r9)
    je      .Lbad

    cmpb    $',', (%r9)
    je      .Lunquoted_end

    incq    %r9
    jmp     .Lunquoted_scan


.Lunquoted_end:

    movq    %r9, %rsi
    subq    %rax, %rsi

    movl    $0xffffffff, %edx
    cmpq    %rdx, %rsi
    ja      .Lbad

    movq    %rax, 0(%r10)
    movl    %esi, 8(%r10)
    movl    $0, 12(%r10)


/*
 * Field complete.
 */

.Lafter_field:

    addq    $16, %r10

    decl    %r11d
    jz      .Llast_field

    cmpq    %r8, %r9
    jae     .Lbad

    cmpb    $',', (%r9)
    jne     .Lbad

    incq    %r9

    jmp     .Lfield


/*
 * Final field: no remaining bytes allowed.
 */

.Llast_field:

    cmpq    %r8, %r9
    je      .Lok

    jmp     .Lbad


/*
 * Empty final field.
 */

.Lempty_at_end:

    movq    %r9, 0(%r10)
    movl    $0, 8(%r10)
    movl    $0, 12(%r10)

    jmp     .Lafter_field


.Lok:

    movl    $1, %eax
    ret


.Lbad:

    xorl    %eax, %eax
    ret


    .size csv_parse_line_asm, .-csv_parse_line_asm
    .section .note.GNU-stack,"",@progbits
