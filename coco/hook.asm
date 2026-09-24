* hook.asm: feed a command line to BASIC through RVEC4 (CONSOLE IN).
*
* void hook_install(void) arms the hook with the NUL-terminated text in
* hook_text; the caller copies the text in and ends it with CR (13) before
* the NUL. When the C program returns to BASIC, the command-line input
* routine calls CONSOLE IN once per character, and each call is answered
* from hook_text instead of the keyboard. The hook puts the original
* RVEC4 back before it hands over the CR, so BASIC then executes the line
* (e.g. RUN"X" or LOADM"X":EXEC) and the keyboard works normally again.
*
* Stack at RVEC4 entry, verified with xroar -trace on CoCo 2 (Color BASIC
* 1.3 + Ext 1.1 + HDB-DOS 1.5 becker) and CoCo 3 (BASIC 2.1 + HDB-DOS 1.5
* becker CoCo 3); both share this Color BASIC code:
*   $A171 CONSOLE IN:  BSR $A176      $A173: ANDA #$7F / RTS
*   $A176:             JSR RVEC4      $A179: CLR CINBFL / keyboard ...
*   0,S = $A179   return into $A176 (keyboard path we skip)
*   2,S = $A173   return into CONSOLE IN's tail (ANDA #$7F / RTS)
*   4,S = caller  $A39D when it is the command-line input routine
* So "LEAS 2,S / RTS" lands on $A173 with the character in A, exactly what
* HDB-DOS's own RVEC4 handler does ($DE50 CoCo 3, $DE47 CoCo 2: PULS B,X /
* LEAS 2,S / RTS). CINBFL is cleared as $A179 would have. B, X, U are kept.
*
* The hook and hook_text live in the program image, which BASIC does not
* touch at the OK prompt; they only need to survive until the CR is read.
RVEC4   EQU     $016A
CINBFL  EQU     $0070           console-in EOF flag

        SECTION bss
        EXPORT  _hook_text
_hook_text RMB  64
hook_ptr   RMB  2
hook_save  RMB  3
        ENDSECTION

        SECTION code
        EXPORT  _hook_install
_hook_install
        ldx     #_hook_text
        stx     hook_ptr
        ldd     RVEC4
        std     hook_save
        lda     RVEC4+2
        sta     hook_save+2
        lda     #$7E            JMP extended
        sta     RVEC4
        ldx     #hook
        stx     RVEC4+1
        rts

hook    pshs    b,x
        clr     CINBFL
        ldx     hook_ptr
        lda     ,x+
        stx     hook_ptr
        cmpa    #13
        bne     hookout
        ldx     hook_save       last char: put RVEC4 back
        stx     RVEC4
        ldb     hook_save+2
        stb     RVEC4+2
hookout puls    b,x
        leas    2,s             drop the return into $A176...
        rts                     ...and return to $A173 with A = char
        ENDSECTION
