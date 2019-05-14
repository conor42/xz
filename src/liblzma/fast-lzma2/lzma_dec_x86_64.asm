; lzma_dec_x86_64.asm -- ASM version of LZMA_decodeReal_3() function
; 2018-02-06: Igor Pavlov : Public domain
; Modified for FL2 by Conor McCarthy
;
; 3 - is the code compatibility version of LZMA_decodeReal_*()
; function for check at link time.
; That code is tightly coupled with LZMA_tryDummy()
; and with another functions in lzma2_dec.c file.
; CLzmaDec structure, (probs) array layout, input and output of
; LZMA_decodeReal_*() must be equal in both versions (C / ASM).

x0 equ EAX
x1 equ ECX
x2 equ EDX
x3 equ EBX
x4 equ ESP
x5 equ EBP
x6 equ ESI
x7 equ EDI

x0_W equ AX
x1_W equ CX
x2_W equ DX
x3_W equ BX

x5_W equ BP
x6_W equ SI
x7_W equ DI

x0_L equ AL
x1_L equ CL
x2_L equ DL
x3_L equ BL

x0_H equ AH
x1_H equ CH
x2_H equ DH
x3_H equ BH

x5_L equ BPL
x6_L equ SIL
x7_L equ DIL

r0 equ RAX
r1 equ RCX
r2 equ RDX
r3 equ RBX
r4 equ RSP
r5 equ RBP
r6 equ RSI
r7 equ RDI
x8 equ r8d
x9 equ r9d
x10 equ r10d
x11 equ r11d
x12 equ r12d
x13 equ r13d
x14 equ r14d
x15 equ r15d

MY_PUSH_4_REGS macro
    push    r3
    push    r5
    push    r6
    push    r7
endm

MY_POP_4_REGS macro
    pop     r7
    pop     r6
    pop     r5
    pop     r3
endm


; for WIN64-x64 ABI:

REG_PARAM_0 equ r1
REG_PARAM_1 equ r2
REG_PARAM_2 equ r8
REG_PARAM_3 equ r9

MY_PUSH_PRESERVED_REGS macro
    MY_PUSH_4_REGS
    push    r12
    push    r13
    push    r14
    push    r15
endm


MY_POP_PRESERVED_REGS macro
    pop     r15
    pop     r14
    pop     r13
    pop     r12
    MY_POP_4_REGS
endm

MY_ALIGN macro num:req
        align  num
endm

MY_ALIGN_16 macro
        MY_ALIGN 16
endm

MY_ALIGN_64 macro
        MY_ALIGN 64
endm


; _LZMA_SIZE_OPT  equ 1

_LZMA_PROB16 equ 1

ifdef _LZMA_PROB16
        PSHIFT  equ 1
        PLOAD macro dest, mem
                movzx   dest, word ptr [mem]
        endm
        PSTORE macro src, mem
                mov     word ptr [mem], @CatStr(src, _W)
        endm
else
        PSHIFT  equ 2
        PLOAD macro dest, mem
                mov     dest, dword ptr [mem]
        endm
        PSTORE  macro src, mem
                mov     dword ptr [mem], src
        endm
endif

PMULT           equ (1 SHL PSHIFT)
PMULT_HALF      equ (1 SHL (PSHIFT - 1))
PMULT_2         equ (1 SHL (PSHIFT + 1))


;       x0      range
;       x1      pbPos / (prob) TREE
;       x2      probBranch / prm (MATCHED) / pbPos / cnt
;       x3      sym
;====== r4 ===  RSP
;       x5      cod
;       x6      t1 NORM_CALC / probs_state / dist
;       x7      t0 NORM_CALC / prob2 IF_BIT_1
;       x8      state
;       x9      match (MATCHED) / sym2 / dist2 / lpMask_reg
;       x10     kBitModelTotal_reg
;       r11     probs
;       x12     offs (MATCHED) / dic / len_temp
;       x13     processedPos
;       x14     bit (MATCHED) / dicPos
;       r15     buf


cod     equ x5
cod_L   equ x5_L
range   equ x0
state   equ x8
state_R equ r8
buf     equ r15
processedPos equ x13
kBitModelTotal_reg equ x10

probBranch   equ x2
probBranch_R equ r2
probBranch_W equ x2_W

pbPos   equ x1
pbPos_R equ r1

cnt     equ x2
cnt_R   equ r2

lpMask_reg equ x9
dicPos  equ r14

sym     equ x3
sym_R   equ r3
sym_L   equ x3_L

probs   equ r11
dic     equ r12

t0      equ x7
t0_W    equ x7_W
t0_R    equ r7

prob2   equ t0
prob2_W equ t0_W

t1      equ x6
t1_R    equ r6

probs_state     equ t1
probs_state_R   equ t1_R

prm     equ r2
match   equ x9
match_R equ r9
offs    equ x12
offs_R  equ r12
bit     equ x14
bit_R   equ r14

sym2    equ x9
sym2_R  equ r9

len_temp equ x12

dist    equ sym
dist2   equ x9



kNumBitModelTotalBits   equ 11
kBitModelTotal          equ (1 SHL kNumBitModelTotalBits)
kNumMoveBits            equ 5
kBitModelOffset         equ ((1 SHL kNumMoveBits) - 1)
kTopValue               equ (1 SHL 24)

NORM_2 macro
        shl     cod, 8
        mov     cod_L, BYTE PTR [buf]
        shl     range, 8
        inc     buf
endm


NORM macro
        cmp     range, kTopValue
        jae     SHORT @F
        NORM_2
@@:
endm


; ---------- Branch MACROS ----------

UPDATE_0 macro probsArray:req, probOffset:req, probDisp:req
        mov     prob2, kBitModelTotal_reg
        sub     prob2, probBranch
        shr     prob2, kNumMoveBits
        add     probBranch, prob2
        PSTORE  probBranch, probOffset * 1 + probsArray + probDisp * PMULT
endm


UPDATE_1 macro probsArray:req, probOffset:req, probDisp:req
        sub     prob2, range
        sub     cod, range
        mov     range, prob2
        mov     prob2, probBranch
        shr     probBranch, kNumMoveBits
        sub     prob2, probBranch
        PSTORE  prob2, probOffset * 1 + probsArray + probDisp * PMULT
endm


CMP_COD macro probsArray:req, probOffset:req, probDisp:req
        PLOAD   probBranch, probOffset * 1 + probsArray + probDisp * PMULT
        NORM
        mov     prob2, range
        shr     range, kNumBitModelTotalBits
        imul    range, probBranch
        cmp     cod, range
endm


IF_BIT_1_NOUP macro probsArray:req, probOffset:req, probDisp:req, toLabel:req
        CMP_COD probsArray, probOffset, probDisp
        jae     toLabel
endm


IF_BIT_1 macro probsArray:req, probOffset:req, probDisp:req, toLabel:req
        IF_BIT_1_NOUP probsArray, probOffset, probDisp, toLabel
        UPDATE_0 probsArray, probOffset, probDisp
endm


IF_BIT_0_NOUP macro probsArray:req, probOffset:req, probDisp:req, toLabel:req
        CMP_COD probsArray, probOffset, probDisp
        jb      toLabel
endm


; ---------- CMOV MACROS ----------

NORM_CALC macro prob:req
        NORM
        mov     t0, range
        shr     range, kNumBitModelTotalBits
        imul    range, prob
        sub     t0, range
        mov     t1, cod
        sub     cod, range
endm


PUP macro prob:req, probPtr:req
        sub     t0, prob
       ; only sar works for both 16/32 bit prob modes
        sar     t0, kNumMoveBits
        add     t0, prob
        PSTORE  t0, probPtr
endm


PUP_SUB macro prob:req, probPtr:req, symSub:req
        sbb     sym, symSub
        PUP prob, probPtr
endm


PUP_COD macro prob:req, probPtr:req, symSub:req
        mov     t0, kBitModelOffset
        cmovb   cod, t1
        mov     t1, sym
        cmovb   t0, kBitModelTotal_reg
        PUP_SUB prob, probPtr, symSub
endm


BIT_0 macro prob:req, probNext:req
        PLOAD   prob, probs + 1 * PMULT
        PLOAD   probNext, probs + 1 * PMULT_2

        NORM_CALC prob
        
        cmovae  range, t0
        PLOAD   t0, probs + 1 * PMULT_2 + PMULT
        cmovae  probNext, t0
        mov     t0, kBitModelOffset
        cmovb   cod, t1
        cmovb   t0, kBitModelTotal_reg
        mov     sym, 2
        PUP_SUB prob, probs + 1 * PMULT, 0 - 1
endm


BIT_1 macro prob:req, probNext:req
        PLOAD   probNext, probs + sym_R * PMULT_2
        add     sym, sym
        
        NORM_CALC prob
        
        cmovae  range, t0
        PLOAD   t0, probs + sym_R * PMULT + PMULT
        cmovae  probNext, t0
        PUP_COD prob, probs + t1_R * PMULT_HALF, 0 - 1
endm


BIT_2 macro prob:req, symSub:req
        add     sym, sym

        NORM_CALC prob
        
        cmovae  range, t0
        PUP_COD prob, probs + t1_R * PMULT_HALF, symSub
endm


; ---------- MATCHED LITERAL ----------

LITM_0 macro
        mov     offs, 256 * PMULT
        shl     match, (PSHIFT + 1)
        mov     bit, offs
        and     bit, match
        PLOAD   x1, probs + 256 * PMULT + bit_R * 1 + 1 * PMULT
        lea     prm, [probs + 256 * PMULT + bit_R * 1 + 1 * PMULT]
        ; lea     prm, [probs + 256 * PMULT + 1 * PMULT]
        ; add     prm, bit_R
        xor     offs, bit
        add     match, match

        NORM_CALC x1

        cmovae  offs, bit
        mov     bit, match
        cmovae  range, t0
        mov     t0, kBitModelOffset
        cmovb   cod, t1
        cmovb   t0, kBitModelTotal_reg
        mov     sym, 0
        PUP_SUB x1, prm, -2-1
endm


LITM macro
        and     bit, offs
        lea     prm, [probs + offs_R * 1]
        add     prm, bit_R
        PLOAD   x1, prm + sym_R * PMULT
        xor     offs, bit
        add     sym, sym
        add     match, match

        NORM_CALC x1

        cmovae  offs, bit
        mov     bit, match
        cmovae  range, t0
        PUP_COD x1, prm + t1_R * PMULT_HALF, - 1
endm


LITM_2 macro
        and     bit, offs
        lea     prm, [probs + offs_R * 1]
        add     prm, bit_R
        PLOAD   x1, prm + sym_R * PMULT
        add     sym, sym

        NORM_CALC x1

        cmovae  range, t0
        PUP_COD x1, prm + t1_R * PMULT_HALF, 256 - 1
endm


; ---------- REVERSE BITS ----------

REV_0 macro prob:req, probNext:req
        ; PLOAD   prob, probs + 1 * PMULT
        ; lea     sym2_R, [probs + 2 * PMULT]
        ; PLOAD   probNext, probs + 2 * PMULT
        PLOAD   probNext, sym2_R

        NORM_CALC prob

        cmovae  range, t0
        PLOAD   t0, probs + 3 * PMULT
        cmovae  probNext, t0
        cmovb   cod, t1
        mov     t0, kBitModelOffset
        cmovb   t0, kBitModelTotal_reg
        lea     t1_R, [probs + 3 * PMULT]
        cmovae  sym2_R, t1_R
        PUP prob, probs + 1 * PMULT
endm


REV_1 macro prob:req, probNext:req, step:req
        add     sym2_R, step * PMULT
        PLOAD   probNext, sym2_R

        NORM_CALC prob

        cmovae  range, t0
        PLOAD   t0, sym2_R + step * PMULT
        cmovae  probNext, t0
        cmovb   cod, t1
        mov     t0, kBitModelOffset
        cmovb   t0, kBitModelTotal_reg
        lea     t1_R, [sym2_R + step * PMULT]
        cmovae  sym2_R, t1_R
        PUP prob, t1_R - step * PMULT_2
endm


REV_2 macro prob:req, step:req
        sub     sym2_R, probs
        shr     sym2, PSHIFT
        or      sym, sym2

        NORM_CALC prob

        cmovae  range, t0
        lea     t0, [sym - step]
        cmovb   sym, t0
        cmovb   cod, t1
        mov     t0, kBitModelOffset
        cmovb   t0, kBitModelTotal_reg
        PUP prob, probs + sym2_R * PMULT
endm


REV_1_VAR macro prob:req
        PLOAD   prob, sym_R
        mov     probs, sym_R
        add     sym_R, sym2_R

        NORM_CALC prob

        cmovae  range, t0
        lea     t0_R, [sym_R + sym2_R]
        cmovae  sym_R, t0_R
        mov     t0, kBitModelOffset
        cmovb   cod, t1
        ; mov     t1, kBitModelTotal
        ; cmovb   t0, t1
        cmovb   t0, kBitModelTotal_reg
        add     sym2, sym2
        PUP prob, probs
endm


LIT_PROBS macro lpMaskParam:req
        ; prob += (UInt32)3 * ((((processedPos << 8) + dic[(dicPos == 0 ? dicBufSize : dicPos) - 1]) & lpMask) << lc);
        mov     t0, processedPos
        shl     t0, 8
        add     sym, t0
        and     sym, lpMaskParam
        add     probs_state_R, pbPos_R
        mov     x1, LOC lc2
        lea     sym, dword ptr[sym_R + 2 * sym_R]
        add     probs, Literal * PMULT
        shl     sym, x1_L
        add     probs, sym_R
        UPDATE_0 probs_state_R, 0, IsMatch
        inc     processedPos
endm



kNumPosBitsMax          equ 4
kNumPosStatesMax        equ (1 SHL kNumPosBitsMax)

kLenNumLowBits          equ 3
kLenNumLowSymbols       equ (1 SHL kLenNumLowBits)
kLenNumHighBits         equ 8
kLenNumHighSymbols      equ (1 SHL kLenNumHighBits)

LenLow                  equ 0
LenChoice               equ LenLow
LenChoice2              equ (LenLow + kLenNumLowSymbols)
LenHigh                 equ (LenLow + 2 * kLenNumLowSymbols * kNumPosStatesMax)
kNumLenProbs			equ (LenHigh + kLenNumHighSymbols)

kLcLpMax equ 4
kLiteralCodersMax equ (1 SHL kLcLpMax)
kLiteralCoderSize equ 300h

kNumStates              equ 12
kNumStates2             equ 16
kNumLitStates           equ 7

kStartPosModelIndex     equ 4
kEndPosModelIndex       equ 14
kNumFullDistances       equ (1 SHL (kEndPosModelIndex SHR 1))

kNumPosSlotBits         equ 6
kNumLenToPosStates      equ 4

kNumAlignBits           equ 4
kAlignTableSize         equ (1 SHL kNumAlignBits)

kMatchMinLen            equ 2
kMatchSpecLenStart      equ (kMatchMinLen + kLenNumLowSymbols * 2 + kLenNumHighSymbols)

Literal equ 0
IsMatch equ (Literal + kLiteralCodersMax * kLiteralCoderSize)
IsRep equ (IsMatch + (kNumStates2 SHL kNumPosBitsMax))
IsRepG0 equ (IsRep + kNumStates)
IsRepG1 equ (IsRepG0 + kNumStates)
IsRepG2 equ (IsRepG1 + kNumStates)
IsRep0Long equ (IsRepG2 + kNumStates)
PosSlot equ (IsRep0Long + (kNumStates2 SHL kNumPosBitsMax))
SpecPos equ (PosSlot + (kNumLenToPosStates SHL kNumPosSlotBits))
kAlign equ (SpecPos + kNumFullDistances)
LenCoder equ (kAlign + kAlignTableSize)
RepLenCoder equ (LenCoder + kNumLenProbs)
NUM_PROBS equ (RepLenCoder + kNumLenProbs)


PTR_FIELD equ dq ?

lzma_dict_asm struct
	dict_buf PTR_FIELD
	dict_pos PTR_FIELD
	dict_full PTR_FIELD
	dict_limit PTR_FIELD
	dict_size PTR_FIELD
lzma_dict_asm ends

CLzmaDec_Asm struct
        range_Spec      dd ?
        code_Spec       dd ?
		init_count	dd ?
        state_Spec      dd ?
        rep0    dd ?
        rep1    dd ?
        rep2    dd ?
        rep3    dd ?
		pbMask	dd ?
        lc      dd ?
        lpMask_Spec      dd ?
        sequence   dd ?
        remainLen dd ?
CLzmaDec_Asm ends


CLzmaDec_Asm_Loc struct
        OLD_RSP    PTR_FIELD
        lzmaPtr    PTR_FIELD
        _pad0_     PTR_FIELD
        _pad1_     PTR_FIELD
        _pad2_     PTR_FIELD
        dicBufSize PTR_FIELD
        probs_Spec PTR_FIELD
        dic_Spec   PTR_FIELD
        
        limit      PTR_FIELD
		bufPosPtr  PTR_FIELD
        bufLimit   PTR_FIELD
        lc2       dd ?
        lpMask    dd ?
        pbMask    dd ?

        remainLen dd ?
        dicPos_Spec     PTR_FIELD
        rep0      dd ?
        rep1      dd ?
        rep2      dd ?
        rep3      dd ?
        buf_Loc     PTR_FIELD
        dict    PTR_FIELD
        full    PTR_FIELD
CLzmaDec_Asm_Loc ends


GLOB_2  equ [sym_R].CLzmaDec_Asm.
GLOB    equ [r1].CLzmaDec_Asm.
LOC_0   equ [r0].CLzmaDec_Asm_Loc.
LOC     equ [RSP].CLzmaDec_Asm_Loc.


COPY_VAR macro name
        mov     t0, GLOB_2 name
        mov     LOC_0 name, t0
endm


RESTORE_VAR macro name
        mov     t0, LOC name
        mov     GLOB name, t0
endm



IsMatchBranch_Pre macro reg
        ; prob = probs + IsMatch + (state << kNumPosBitsMax) + posState;
        mov     pbPos, LOC pbMask
        and     pbPos, processedPos
        shl     pbPos, (kLenNumLowBits + 1 + PSHIFT)
        lea     probs_state_R, [probs + state_R]
endm


IsMatchBranch macro reg
        IsMatchBranch_Pre
        IF_BIT_1 probs_state_R, pbPos_R, IsMatch, IsMatch_label
endm
        

CheckLimits macro reg
        cmp     buf, LOC bufLimit
        jae     fin_OK
        cmp     dicPos, LOC limit
        jae     fin_OK
endm



PARAM_lzma      equ REG_PARAM_0
PARAM_dict     equ REG_PARAM_1
PARAM_buf  equ REG_PARAM_2
PARAM_bufPosPtr equ REG_PARAM_3

		.code

_TEXT$LZMADECOPT SEGMENT ALIGN(64) 'CODE'

LZMA_decodeReal_asm_5 PROC
MY_PUSH_PRESERVED_REGS

; RSP is (16x + 8) bytes aligned in WIN64-x64
; LocalSize equ ((((SIZEOF CLzmaDec_Asm_Loc) + 7) / 16 * 16) + 8)

        lea     r0, [RSP - (SIZEOF CLzmaDec_Asm_Loc)]
        and     r0, -128
        mov     r5, RSP
        mov     RSP, r0
        mov     LOC_0 Old_RSP, r5
        mov     t0_R, [r5 + 68h]
        add     t0_R, PARAM_buf
        mov     LOC_0 bufLimit, t0_R
        mov     LOC_0 dict, PARAM_dict
        mov     LOC_0 bufPosPtr, PARAM_bufPosPtr
        mov     LOC_0 buf_Loc, PARAM_buf
        mov     buf, [PARAM_bufPosPtr]
        add     buf, PARAM_buf
        mov     dic, [PARAM_dict].lzma_dict_asm.dict_buf
        mov     dicPos, [PARAM_dict].lzma_dict_asm.dict_pos
        mov     r13, [PARAM_dict].lzma_dict_asm.dict_pos

        add     dicPos, dic
        mov     LOC_0 dicPos_Spec, dicPos
        mov     LOC_0 dic_Spec, dic
        
        mov     t0_R, [PARAM_dict].lzma_dict_asm.dict_limit
        add     t0_R, dic
        mov     LOC_0 limit, t0_R

        mov     t0_R, [PARAM_dict].lzma_dict_asm.dict_size
        mov     LOC_0 dicBufSize, t0_R
        mov     t0_R, [PARAM_dict].lzma_dict_asm.dict_full
        add     t0_R, dic
        mov     LOC_0 full, t0_R

        mov     LOC_0 remainLen, 0  ; remainLen must be ZERO

        mov     sym_R, PARAM_lzma  ;  CLzmaDec_Asm_Loc pointer for GLOB_2
        mov     LOC_0 probs_Spec, sym_R
        mov     probs, sym_R

        add     sym_R, (NUM_PROBS SHL PSHIFT)
        mov     LOC_0 lzmaPtr, sym_R      

        COPY_VAR(rep0)
        COPY_VAR(rep1)
        COPY_VAR(rep2)
        COPY_VAR(rep3)
        COPY_VAR(pbMask)
        
        ; unsigned pbMask = ((unsigned)1 << (p->prop.pb)) - 1;
        ; unsigned lc = p->prop.lc;
        ; unsigned lpMask = ((unsigned)0x100 << p->prop.lp) - ((unsigned)0x100 >> lc);

        mov     x1, GLOB_2 lc
        add     x1_L, PSHIFT
        mov     LOC_0 lc2, x1
        mov     t0, GLOB_2 lpMask_Spec
        mov     LOC_0 lpMask, t0
        mov     lpMask_reg, t0
        
        mov     state, GLOB_2 state_Spec
        shl     state, PSHIFT

        mov     range, GLOB_2 range_Spec
        mov     cod,   GLOB_2 code_Spec
        mov     kBitModelTotal_reg, kBitModelTotal
        xor     sym_R, sym_R

        ; if (processedPos != 0 || checkDicSize != 0)
        or      processedPos, processedPos
        jz      @f
        
        mov     t0_R, LOC dicBufSize
        add     t0_R, dic
        cmp     dicPos, dic
        cmovnz  t0_R, dicPos
        movzx   sym, byte ptr[t0_R - 1]

@@:
        IsMatchBranch_Pre
        cmp     state, 4 * PMULT
        jb      lit_end
        cmp     state, kNumLitStates * PMULT
        jb      lit_matched_end
        jmp     lz_end
        

        

; ---------- LITERAL ----------
MY_ALIGN_64
lit_start:
        xor     state, state
lit_start_2:
        LIT_PROBS lpMask_reg

    ifdef _LZMA_SIZE_OPT

        PLOAD   x1, probs + 1 * PMULT
        mov     sym, 1
MY_ALIGN_16
lit_loop:
        BIT_1   x1, x2
        mov     x1, x2
        cmp     sym, 127
        jbe     lit_loop
        
    else
        
        BIT_0   x1, x2
        BIT_1   x2, x1
        BIT_1   x1, x2
        BIT_1   x2, x1
        BIT_1   x1, x2
        BIT_1   x2, x1
        BIT_1   x1, x2
        
    endif

        BIT_2   x2, 256 - 1
        
        mov     probs, LOC probs_Spec
        IsMatchBranch_Pre
        mov     byte ptr[dicPos], sym_L
        inc     dicPos
        mov     t0_R, LOC full
        cmp     dicPos, t0_R
        cmova   t0_R, dicPos
        mov     LOC full, t0_R
                
        CheckLimits
lit_end:
        IF_BIT_0_NOUP probs_state_R, pbPos_R, IsMatch, lit_start

; ---------- MATCHES ----------
MY_ALIGN_16
IsMatch_label:
        UPDATE_1 probs_state_R, pbPos_R, IsMatch
        IF_BIT_1 probs_state_R, 0, IsRep, IsRep_label

        add     probs, LenCoder * PMULT
        add     state, kNumStates * PMULT

; ---------- LEN DECODE ----------
len_decode:
        mov     len_temp, 8 - 1 - kMatchMinLen
        IF_BIT_0_NOUP probs, 0, 0, len_mid_0
        UPDATE_1 probs, 0, 0
        add     probs, (1 SHL (kLenNumLowBits + PSHIFT))
        mov     len_temp, -1 - kMatchMinLen
        IF_BIT_0_NOUP probs, 0, 0, len_mid_0
        UPDATE_1 probs, 0, 0
        add     probs, LenHigh * PMULT - (1 SHL (kLenNumLowBits + PSHIFT))
        mov     sym, 1
        PLOAD   x1, probs + 1 * PMULT

MY_ALIGN_16
len8_loop:
        BIT_1   x1, x2
        mov     x1, x2
        cmp     sym, 64
        jb      len8_loop
        
        mov     len_temp, (kLenNumHighSymbols - kLenNumLowSymbols * 2) - 1 - kMatchMinLen
        jmp     len_mid_2
        
MY_ALIGN_16
len_mid_0:
        UPDATE_0 probs, 0, 0
        add     probs, pbPos_R
        BIT_0   x2, x1
len_mid_2:
        BIT_1   x1, x2
        BIT_2   x2, len_temp
        mov     probs, LOC probs_Spec
        cmp     state, kNumStates * PMULT
        jb      copy_match
        

; ---------- DECODE DISTANCE ----------
        ; probs + PosSlot + ((len < kNumLenToPosStates ? len : kNumLenToPosStates - 1) << kNumPosSlotBits);

        mov     t0, 3 + kMatchMinLen
        cmp     sym, 3 + kMatchMinLen
        cmovb   t0, sym
        add     probs, PosSlot * PMULT - (kMatchMinLen SHL (kNumPosSlotBits + PSHIFT))
        shl     t0, (kNumPosSlotBits + PSHIFT)
        add     probs, t0_R
        
        ; sym = Len
        mov     len_temp, sym

    ifdef _LZMA_SIZE_OPT

        PLOAD   x1, probs + 1 * PMULT
        mov     sym, 1
MY_ALIGN_16
slot_loop:
        BIT_1   x1, x2
        mov     x1, x2
        cmp     sym, 32
        jb      slot_loop
        
    else
        
        BIT_0   x1, x2
        BIT_1   x2, x1
        BIT_1   x1, x2
        BIT_1   x2, x1
        BIT_1   x1, x2
        
    endif
        
        mov     x1, sym
        BIT_2   x2, 64-1

        and     sym, 3
        mov     probs, LOC probs_Spec
        cmp     x1, 32 + kEndPosModelIndex / 2
        jb      short_dist

        ;  unsigned numDirectBits = (unsigned)(((distance >> 1) - 1));
        sub     x1, (32 + 1 + kNumAlignBits)
        ;  distance = (2 | (distance & 1));
        or      sym, 2
        PLOAD   x2, probs + 1 * PMULT
        shl     sym, kNumAlignBits + 1
        lea     sym2_R, [probs + 2 * PMULT]
        
        jmp     direct_norm
        
; ---------- DIRECT DISTANCE ----------
MY_ALIGN_16
direct_loop:
        shr     range, 1
        mov     t0, cod
        sub     cod, range
        cmovs   cod, t0
        cmovns  sym, t1
        
        dec     x1
        je      direct_end

        add     sym, sym
direct_norm:
        lea     t1, [sym_R + (1 SHL kNumAlignBits)]
        cmp     range, kTopValue
        jae     near ptr direct_loop
        ; we align for 32 here with "near ptr" command above
        NORM_2
        jmp     direct_loop

MY_ALIGN_16
direct_end:
        ;  prob =  + kAlign;
        ;  distance <<= kNumAlignBits;
        REV_0   x2, x1
        REV_1   x1, x2, 2
        REV_1   x2, x1, 4
        REV_2   x1, 8

decode_dist_end:

        ; if (distance >= (checkDicSize == 0 ? processedPos: checkDicSize))

        cmp     sym, processedPos
        jae     end_of_payload
        
        ; rep3 = rep2;
        ; rep2 = rep1;
        ; rep1 = rep0;
        ; rep0 = distance + 1;

        inc     sym
        mov     t0, LOC rep0
        mov     t1, LOC rep1
        mov     x1, LOC rep2
        mov     LOC rep0, sym
        mov     sym, len_temp
        mov     LOC rep1, t0
        mov     LOC rep2, t1
        mov     LOC rep3, x1
        
        ; state = (state < kNumStates + kNumLitStates) ? kNumLitStates : kNumLitStates + 3;
        cmp     state, (kNumStates + kNumLitStates) * PMULT
        mov     state, kNumLitStates * PMULT
        mov     t0, (kNumLitStates + 3) * PMULT
        cmovae  state, t0

        
; ---------- COPY MATCH ----------
copy_match:

        ; if ((rem = limit - dicPos) == 0)
        ; {
        ;   p->dicPos = dicPos;
        ;   return SZ_ERROR_DATA;
        ; }
        mov     cnt_R, LOC limit
        sub     cnt_R, dicPos
        jz      fin_ERROR

        ; curLen = ((rem < len) ? (unsigned)rem : len);
        cmp     cnt_R, sym_R
        ; cmovae  cnt_R, sym_R ; 64-bit
        cmovae  cnt, sym ; 32-bit

        mov     dic, LOC dic_Spec

        mov     t0_R, dicPos
        add     dicPos, cnt_R
        mov     r1, LOC full
        cmp     dicPos, r1
        cmova   r1, dicPos
        mov     LOC full, r1
        mov     x1, LOC rep0
        ; processedPos += curLen;
        add     processedPos, cnt
        ; len -= curLen;
        sub     sym, cnt
        mov     LOC remainLen, sym

        sub     t0_R, dic
        
        ; pos = dicPos - rep0 + (dicPos < rep0 ? dicBufSize : 0);
        sub     t0_R, r1
        jae     @f

        mov     r1, LOC dicBufSize
        add     t0_R, r1
        sub     r1, t0_R
        cmp     cnt_R, r1
        ja      copy_match_cross
@@:
        ; if (curLen <= dicBufSize - pos)

; ---------- COPY MATCH FAST ----------
        ; Byte *dest = dic + dicPos;
        ; ptrdiff_t src = (ptrdiff_t)pos - (ptrdiff_t)dicPos;
        ; dicPos += curLen;

        add     t0_R, dic
        movzx   sym, byte ptr[t0_R]
        add     t0_R, cnt_R
        neg     cnt_R
copy_common:
        dec     dicPos

        IsMatchBranch_Pre
        inc     cnt_R
        jz      copy_end
MY_ALIGN_16
@@:
        mov     byte ptr[cnt_R * 1 + dicPos], sym_L
        movzx   sym, byte ptr[cnt_R * 1 + t0_R]
        inc     cnt_R
        jnz     @b

copy_end:
lz_end_match:
        mov     byte ptr[dicPos], sym_L
        inc     dicPos
  
        CheckLimits
lz_end:
        IF_BIT_1_NOUP probs_state_R, pbPos_R, IsMatch, IsMatch_label



; ---------- LITERAL MATCHED ----------
                
        LIT_PROBS LOC lpMask
        
        ; matchByte = dic[dicPos - rep0 + (dicPos < rep0 ? dicBufSize : 0)];
        mov     x1, LOC rep0
        mov     LOC dicPos_Spec, dicPos
        
        ; state -= (state < 10) ? 3 : 6;
        lea     t0, [state_R - 6 * PMULT]
        sub     state, 3 * PMULT
        cmp     state, 7 * PMULT
        cmovae  state, t0
        
        sub     dicPos, dic
        sub     dicPos, r1
        jae     @f
        add     dicPos, LOC dicBufSize
@@:
        movzx   match, byte ptr[dic + dicPos * 1]

    ifdef _LZMA_SIZE_OPT

        mov     offs, 256 * PMULT
        shl     match, (PSHIFT + 1)
        mov     bit, match
        mov     sym, 1
MY_ALIGN_16
litm_loop:
        LITM
        cmp     sym, 256
        jb      litm_loop
        sub     sym, 256
        
    else
        
        LITM_0
        LITM
        LITM
        LITM
        LITM
        LITM
        LITM
        LITM_2
        
    endif
        
        mov     probs, LOC probs_Spec
        IsMatchBranch_Pre
        mov     dicPos, LOC dicPos_Spec
        mov     byte ptr[dicPos], sym_L
        inc     dicPos
        
        CheckLimits
lit_matched_end:
        IF_BIT_1_NOUP probs_state_R, pbPos_R, IsMatch, IsMatch_label
        mov     lpMask_reg, LOC lpMask
        sub     state, 3 * PMULT
        jmp     lit_start_2
        


; ---------- REP 0 LITERAL ----------
MY_ALIGN_16
IsRep0Short_label:
        UPDATE_0 probs_state_R, pbPos_R, IsRep0Long

        ; dic[dicPos] = dic[dicPos - rep0 + (dicPos < rep0 ? dicBufSize : 0)];
        mov     dic, LOC dic_Spec
        mov     t0_R, dicPos
        mov     probBranch, LOC rep0
        sub     t0_R, dic
        
        sub     probs, RepLenCoder * PMULT
        inc     processedPos
        ; state = state < kNumLitStates ? 9 : 11;
        or      state, 1 * PMULT
        IsMatchBranch_Pre
       
        sub     t0_R, probBranch_R
        jae     @f
        add     t0_R, LOC dicBufSize
@@:
        movzx   sym, byte ptr[dic + t0_R * 1]
        jmp     lz_end_match
  
        
MY_ALIGN_16
IsRep_label:
        UPDATE_1 probs_state_R, 0, IsRep

        ; The (checkDicSize == 0 && processedPos == 0) case was checked before in lzma2_dec.c with kBadRepCode.
        ; So we don't check it here.
        
        ; state = state < kNumLitStates ? 8 : 11;
        cmp     state, kNumLitStates * PMULT
        mov     state, 8 * PMULT
        mov     probBranch, 11 * PMULT
        cmovae  state, probBranch

        ; prob = probs + RepLenCoder;
        add     probs, RepLenCoder * PMULT
        
        IF_BIT_1 probs_state_R, 0, IsRepG0, IsRepG0_label
        IF_BIT_0_NOUP probs_state_R, pbPos_R, IsRep0Long, IsRep0Short_label
        UPDATE_1 probs_state_R, pbPos_R, IsRep0Long
        jmp     len_decode

MY_ALIGN_16
IsRepG0_label:
        UPDATE_1 probs_state_R, 0, IsRepG0
        mov     dist2, LOC rep0
        mov     dist, LOC rep1
        mov     LOC rep1, dist2
        
        IF_BIT_1 probs_state_R, 0, IsRepG1, IsRepG1_label
        mov     LOC rep0, dist
        jmp     len_decode
        
MY_ALIGN_16
IsRepG1_label:
        UPDATE_1 probs_state_R, 0, IsRepG1
        mov     dist2, LOC rep2
        mov     LOC rep2, dist
        
        IF_BIT_1 probs_state_R, 0, IsRepG2, IsRepG2_label
        mov     LOC rep0, dist2
        jmp     len_decode

MY_ALIGN_16
IsRepG2_label:
        UPDATE_1 probs_state_R, 0, IsRepG2
        mov     dist, LOC rep3
        mov     LOC rep3, dist2
        mov     LOC rep0, dist
        jmp     len_decode

        

; ---------- SPEC SHORT DISTANCE ----------

MY_ALIGN_16
short_dist:
        sub     x1, 32 + 1
        jbe     decode_dist_end
        or      sym, 2
        shl     sym, x1_L
        lea     sym_R, [probs + sym_R * PMULT + SpecPos * PMULT + 1 * PMULT]
        mov     sym2, PMULT ; step
MY_ALIGN_16
spec_loop:
        REV_1_VAR x2
        dec     x1
        jnz     spec_loop

        mov     probs, LOC probs_Spec
        sub     sym, sym2
        sub     sym, SpecPos * PMULT
        sub     sym_R, probs
        shr     sym, PSHIFT
        
        jmp     decode_dist_end


; ---------- COPY MATCH CROSS ----------
copy_match_cross:
        ; t0_R - src pos
        ; r1 - len to dicBufSize
        ; cnt_R - total copy len

        mov     t1_R, t0_R         ; srcPos
        mov     t0_R, dic
        mov     r1, LOC dicBufSize   ;
        neg     cnt_R
@@:
        movzx   sym, byte ptr[t1_R * 1 + t0_R]
        inc     t1_R
        mov     byte ptr[cnt_R * 1 + dicPos], sym_L
        inc     cnt_R
        cmp     t1_R, r1
        jne     @b
        
        movzx   sym, byte ptr[t0_R]
        sub     t0_R, cnt_R
        jmp     copy_common




fin_ERROR:
        mov     LOC remainLen, len_temp
        mov     sym, 1
        jmp     fin

end_of_payload:
        cmp     sym, 0FFFFFFFFh ; -1
        jne     fin_ERROR

        mov     LOC remainLen, kMatchSpecLenStart
        sub     state, kNumStates * PMULT

fin_OK:
        xor     sym, sym

fin:
        NORM

        mov     r1, LOC lzmaPtr

        mov     t0_R, LOC dict
        sub     dicPos, LOC dic_Spec
        mov     [t0_R].lzma_dict_asm.dict_pos, dicPos
        mov     t1_R, LOC full
        sub     t1_R, LOC dic_Spec
        mov     [t0_R].lzma_dict_asm.dict_full, t1_R
        mov     t0_R, LOC bufPosPtr
        sub     buf, LOC buf_Loc
        mov     [t0_R], buf
        mov     GLOB range_Spec, range
        mov     GLOB code_Spec, cod
        shr     state, PSHIFT
        mov     GLOB state_Spec, state

        RESTORE_VAR(remainLen)
        RESTORE_VAR(rep0)
        RESTORE_VAR(rep1)
        RESTORE_VAR(rep2)
        RESTORE_VAR(rep3)

        mov     x0, sym
        
        mov     RSP, LOC Old_RSP

		MY_POP_PRESERVED_REGS
		ret
LZMA_decodeReal_asm_5 ENDP

_TEXT$LZMADECOPT ENDS

end
