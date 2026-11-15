/* Patch declarations: host code at guest addresses, written next to its body (docs/lifter.md, "Hooks and probes").

   The lifter reads these macros from the source with a token scanner of a fixed grammar (nx2/lift2/patch_scan.py), and
   the compiler expands the same text into the host function's prototype and a record in the object section hos_patch
   that the build compares with the scan (nx2/build.py): a declaration one of them sees and the other does not stops
   the build. A declaration stands at file scope, outside #if, and is never made by another macro.

     PATCH_AT(pc, word(W), mode, result, fn, operand...) { body }
       mode: after (the instruction at pc; after a BL / BLR when it returns), before (on the edge into it: pc must be no
       branch target), instead (in place of a BL / BLR: the call is not made, x30 set as it would be; in place of any
       other instruction: its result, the instruction's destination, is fn's). The containing function is found by the
       lifter from pc; word is the instruction's, checked at the build: another game version stops it.
       result: none, xN (uint64_t), wN (uint32_t, zero-extended), sN (float, the rest of Vn zero), qN (hos_q). A result
       is caller-saved or written by the instruction itself (checked).
       operands, each one parameter in order: xN (uint64_t), wN (uint32_t), sN (float), qN (hos_q), sp (uint64_t),
       ld(xN or sp, K) (uint64_t: the guest word at reg + K, loaded by the lifted code), add(xN, K) (uint64_t: reg + K),
       k(K) (uint32_t: the constant), as(operand, name) (that operand, its parameter named name; else named after it:
       x8, ld_sp_192, k_3). guest(A, ...) or guest(computed), first: the function calls those guest functions (or any)
       through guest_call; its first parameter is GuestCall *g. io(r, ...), last: those registers (x and w as 8-byte
       slots, q as 16) in a struct the function gets as io (tagged hos_io_<line>) and leaves their new values in.
     PATCH_AT_SITE(pc, word(W), mode, result, fn, operand...);
       another site of fn (after its PATCH_AT): the operands' types must be fn's parameters' (a compile error else).
     PATCH_HOOK(addr, word(W), effect...) { body }     PATCH_HOOK_SYM(symbol, effect...) { body }
       host code at the entry of the lifted function at addr (its first word W, checked at the build), or of the one
       the image exports as symbol: int entry_<addr>(Ctx *) / entry_sym_<symbol>(Ctx *), the body seeing the call's
       HookArgs *a and HookCall *h and returning 1 (handled) or 0 (the function runs) (host/hos.h ENTRY_HOOK_COUNTED).
       effects: reads(reg..., "why") the registers it reads beyond x0-x7 and x30 (sp for hook_call_guest); guest(A, ...)
       the guest functions it calls besides its own body.
     HLE(symbol, effect...) { body }     NV(name, effect...) { body }     HLE_EFFECTS(symbol, effect...);
       a function the image replaces: void imp_<symbol>(Ctx *c) (hooked where the game imports it and at the SDK's
       definition); an NVN proc, static void n_<name>(Ctx *c, void *u_) (host/nvn_native.c's table); the effects of a
       replaced function whose body is not declared here (a generated stub or dispatch). effects: reads(reg..., "why")
       the registers a call reads beyond the AAPCS arguments, x8, v0-v7, sp and x30; clobbers("why") a call keeps no
       register. */
#ifndef HOS_PATCH_H
#define HOS_PATCH_H
#include <stdint.h>

#define HOS_CAT_(a, b) a##b
#define HOS_CAT(a, b) HOS_CAT_(a, b)
/* (the record the build compares with the scan: the declaration's own text, as the preprocessor stringifies it) */
#define HOS_PATCH_RECORD(kind, text) \
    static const char HOS_CAT(hos_patch_record_, __COUNTER__)[] __attribute__((used, section("hos_patch"))) = \
        kind "(" text ")";

/* each operand: its type (HOS_T_), its parameter (HOS_P_), an io register's slot (HOS_IO_) */
#define HOS_T_sp uint64_t
#define HOS_P_sp uint64_t sp
#define HOS_T_ld(r, k) uint64_t
#define HOS_P_ld(r, k) uint64_t ld_##r##_##k
#define HOS_T_add(r, k) uint64_t
#define HOS_P_add(r, k) uint64_t add_##r##_##k
#define HOS_T_k(n) uint32_t
#define HOS_P_k(n) uint32_t k_##n
#define HOS_T_as(op, name) HOS_T_##op
#define HOS_P_as(op, name) HOS_T_##op name
#define HOS_T_guest(...) GuestCall *
#define HOS_P_guest(...) GuestCall *g
#define HOS_P_io(...) struct HOS_CAT(hos_io_, __LINE__) *io
/* (what an operand declares before the function: io's struct, tagged by the declaration's line; nothing else) */
#define HOS_D_io(...) struct HOS_CAT(hos_io_, __LINE__) { HOS_IO_EACH(__VA_ARGS__) };
#define HOS_D_sp
#define HOS_D_ld(r, k)
#define HOS_D_add(r, k)
#define HOS_D_k(n)
#define HOS_D_as(op, name)
#define HOS_D_guest(...)
#define HOS_D_x0
#define HOS_D_x1
#define HOS_D_x2
#define HOS_D_x3
#define HOS_D_x4
#define HOS_D_x5
#define HOS_D_x6
#define HOS_D_x7
#define HOS_D_x8
#define HOS_D_x9
#define HOS_D_x10
#define HOS_D_x11
#define HOS_D_x12
#define HOS_D_x13
#define HOS_D_x14
#define HOS_D_x15
#define HOS_D_x16
#define HOS_D_x17
#define HOS_D_x18
#define HOS_D_x19
#define HOS_D_x20
#define HOS_D_x21
#define HOS_D_x22
#define HOS_D_x23
#define HOS_D_x24
#define HOS_D_x25
#define HOS_D_x26
#define HOS_D_x27
#define HOS_D_x28
#define HOS_D_x29
#define HOS_D_x30
#define HOS_D_w0
#define HOS_D_w1
#define HOS_D_w2
#define HOS_D_w3
#define HOS_D_w4
#define HOS_D_w5
#define HOS_D_w6
#define HOS_D_w7
#define HOS_D_w8
#define HOS_D_w9
#define HOS_D_w10
#define HOS_D_w11
#define HOS_D_w12
#define HOS_D_w13
#define HOS_D_w14
#define HOS_D_w15
#define HOS_D_w16
#define HOS_D_w17
#define HOS_D_w18
#define HOS_D_w19
#define HOS_D_w20
#define HOS_D_w21
#define HOS_D_w22
#define HOS_D_w23
#define HOS_D_w24
#define HOS_D_w25
#define HOS_D_w26
#define HOS_D_w27
#define HOS_D_w28
#define HOS_D_w29
#define HOS_D_w30
#define HOS_D_s0
#define HOS_D_s1
#define HOS_D_s2
#define HOS_D_s3
#define HOS_D_s4
#define HOS_D_s5
#define HOS_D_s6
#define HOS_D_s7
#define HOS_D_s8
#define HOS_D_s9
#define HOS_D_s10
#define HOS_D_s11
#define HOS_D_s12
#define HOS_D_s13
#define HOS_D_s14
#define HOS_D_s15
#define HOS_D_s16
#define HOS_D_s17
#define HOS_D_s18
#define HOS_D_s19
#define HOS_D_s20
#define HOS_D_s21
#define HOS_D_s22
#define HOS_D_s23
#define HOS_D_s24
#define HOS_D_s25
#define HOS_D_s26
#define HOS_D_s27
#define HOS_D_s28
#define HOS_D_s29
#define HOS_D_s30
#define HOS_D_s31
#define HOS_D_q0
#define HOS_D_q1
#define HOS_D_q2
#define HOS_D_q3
#define HOS_D_q4
#define HOS_D_q5
#define HOS_D_q6
#define HOS_D_q7
#define HOS_D_q8
#define HOS_D_q9
#define HOS_D_q10
#define HOS_D_q11
#define HOS_D_q12
#define HOS_D_q13
#define HOS_D_q14
#define HOS_D_q15
#define HOS_D_q16
#define HOS_D_q17
#define HOS_D_q18
#define HOS_D_q19
#define HOS_D_q20
#define HOS_D_q21
#define HOS_D_q22
#define HOS_D_q23
#define HOS_D_q24
#define HOS_D_q25
#define HOS_D_q26
#define HOS_D_q27
#define HOS_D_q28
#define HOS_D_q29
#define HOS_D_q30
#define HOS_D_q31

#define HOS_R_none void
#define HOS_T_x0 uint64_t
#define HOS_T_w0 uint32_t
#define HOS_T_x1 uint64_t
#define HOS_T_w1 uint32_t
#define HOS_T_x2 uint64_t
#define HOS_T_w2 uint32_t
#define HOS_T_x3 uint64_t
#define HOS_T_w3 uint32_t
#define HOS_T_x4 uint64_t
#define HOS_T_w4 uint32_t
#define HOS_T_x5 uint64_t
#define HOS_T_w5 uint32_t
#define HOS_T_x6 uint64_t
#define HOS_T_w6 uint32_t
#define HOS_T_x7 uint64_t
#define HOS_T_w7 uint32_t
#define HOS_T_x8 uint64_t
#define HOS_T_w8 uint32_t
#define HOS_T_x9 uint64_t
#define HOS_T_w9 uint32_t
#define HOS_T_x10 uint64_t
#define HOS_T_w10 uint32_t
#define HOS_T_x11 uint64_t
#define HOS_T_w11 uint32_t
#define HOS_T_x12 uint64_t
#define HOS_T_w12 uint32_t
#define HOS_T_x13 uint64_t
#define HOS_T_w13 uint32_t
#define HOS_T_x14 uint64_t
#define HOS_T_w14 uint32_t
#define HOS_T_x15 uint64_t
#define HOS_T_w15 uint32_t
#define HOS_T_x16 uint64_t
#define HOS_T_w16 uint32_t
#define HOS_T_x17 uint64_t
#define HOS_T_w17 uint32_t
#define HOS_T_x18 uint64_t
#define HOS_T_w18 uint32_t
#define HOS_T_x19 uint64_t
#define HOS_T_w19 uint32_t
#define HOS_T_x20 uint64_t
#define HOS_T_w20 uint32_t
#define HOS_T_x21 uint64_t
#define HOS_T_w21 uint32_t
#define HOS_T_x22 uint64_t
#define HOS_T_w22 uint32_t
#define HOS_T_x23 uint64_t
#define HOS_T_w23 uint32_t
#define HOS_T_x24 uint64_t
#define HOS_T_w24 uint32_t
#define HOS_T_x25 uint64_t
#define HOS_T_w25 uint32_t
#define HOS_T_x26 uint64_t
#define HOS_T_w26 uint32_t
#define HOS_T_x27 uint64_t
#define HOS_T_w27 uint32_t
#define HOS_T_x28 uint64_t
#define HOS_T_w28 uint32_t
#define HOS_T_x29 uint64_t
#define HOS_T_w29 uint32_t
#define HOS_T_x30 uint64_t
#define HOS_T_w30 uint32_t
#define HOS_T_s0 float
#define HOS_T_q0 hos_q
#define HOS_T_s1 float
#define HOS_T_q1 hos_q
#define HOS_T_s2 float
#define HOS_T_q2 hos_q
#define HOS_T_s3 float
#define HOS_T_q3 hos_q
#define HOS_T_s4 float
#define HOS_T_q4 hos_q
#define HOS_T_s5 float
#define HOS_T_q5 hos_q
#define HOS_T_s6 float
#define HOS_T_q6 hos_q
#define HOS_T_s7 float
#define HOS_T_q7 hos_q
#define HOS_T_s8 float
#define HOS_T_q8 hos_q
#define HOS_T_s9 float
#define HOS_T_q9 hos_q
#define HOS_T_s10 float
#define HOS_T_q10 hos_q
#define HOS_T_s11 float
#define HOS_T_q11 hos_q
#define HOS_T_s12 float
#define HOS_T_q12 hos_q
#define HOS_T_s13 float
#define HOS_T_q13 hos_q
#define HOS_T_s14 float
#define HOS_T_q14 hos_q
#define HOS_T_s15 float
#define HOS_T_q15 hos_q
#define HOS_T_s16 float
#define HOS_T_q16 hos_q
#define HOS_T_s17 float
#define HOS_T_q17 hos_q
#define HOS_T_s18 float
#define HOS_T_q18 hos_q
#define HOS_T_s19 float
#define HOS_T_q19 hos_q
#define HOS_T_s20 float
#define HOS_T_q20 hos_q
#define HOS_T_s21 float
#define HOS_T_q21 hos_q
#define HOS_T_s22 float
#define HOS_T_q22 hos_q
#define HOS_T_s23 float
#define HOS_T_q23 hos_q
#define HOS_T_s24 float
#define HOS_T_q24 hos_q
#define HOS_T_s25 float
#define HOS_T_q25 hos_q
#define HOS_T_s26 float
#define HOS_T_q26 hos_q
#define HOS_T_s27 float
#define HOS_T_q27 hos_q
#define HOS_T_s28 float
#define HOS_T_q28 hos_q
#define HOS_T_s29 float
#define HOS_T_q29 hos_q
#define HOS_T_s30 float
#define HOS_T_q30 hos_q
#define HOS_T_s31 float
#define HOS_T_q31 hos_q
#define HOS_P_x0 uint64_t x0
#define HOS_P_w0 uint32_t w0
#define HOS_P_x1 uint64_t x1
#define HOS_P_w1 uint32_t w1
#define HOS_P_x2 uint64_t x2
#define HOS_P_w2 uint32_t w2
#define HOS_P_x3 uint64_t x3
#define HOS_P_w3 uint32_t w3
#define HOS_P_x4 uint64_t x4
#define HOS_P_w4 uint32_t w4
#define HOS_P_x5 uint64_t x5
#define HOS_P_w5 uint32_t w5
#define HOS_P_x6 uint64_t x6
#define HOS_P_w6 uint32_t w6
#define HOS_P_x7 uint64_t x7
#define HOS_P_w7 uint32_t w7
#define HOS_P_x8 uint64_t x8
#define HOS_P_w8 uint32_t w8
#define HOS_P_x9 uint64_t x9
#define HOS_P_w9 uint32_t w9
#define HOS_P_x10 uint64_t x10
#define HOS_P_w10 uint32_t w10
#define HOS_P_x11 uint64_t x11
#define HOS_P_w11 uint32_t w11
#define HOS_P_x12 uint64_t x12
#define HOS_P_w12 uint32_t w12
#define HOS_P_x13 uint64_t x13
#define HOS_P_w13 uint32_t w13
#define HOS_P_x14 uint64_t x14
#define HOS_P_w14 uint32_t w14
#define HOS_P_x15 uint64_t x15
#define HOS_P_w15 uint32_t w15
#define HOS_P_x16 uint64_t x16
#define HOS_P_w16 uint32_t w16
#define HOS_P_x17 uint64_t x17
#define HOS_P_w17 uint32_t w17
#define HOS_P_x18 uint64_t x18
#define HOS_P_w18 uint32_t w18
#define HOS_P_x19 uint64_t x19
#define HOS_P_w19 uint32_t w19
#define HOS_P_x20 uint64_t x20
#define HOS_P_w20 uint32_t w20
#define HOS_P_x21 uint64_t x21
#define HOS_P_w21 uint32_t w21
#define HOS_P_x22 uint64_t x22
#define HOS_P_w22 uint32_t w22
#define HOS_P_x23 uint64_t x23
#define HOS_P_w23 uint32_t w23
#define HOS_P_x24 uint64_t x24
#define HOS_P_w24 uint32_t w24
#define HOS_P_x25 uint64_t x25
#define HOS_P_w25 uint32_t w25
#define HOS_P_x26 uint64_t x26
#define HOS_P_w26 uint32_t w26
#define HOS_P_x27 uint64_t x27
#define HOS_P_w27 uint32_t w27
#define HOS_P_x28 uint64_t x28
#define HOS_P_w28 uint32_t w28
#define HOS_P_x29 uint64_t x29
#define HOS_P_w29 uint32_t w29
#define HOS_P_x30 uint64_t x30
#define HOS_P_w30 uint32_t w30
#define HOS_P_s0 float s0
#define HOS_P_q0 hos_q q0
#define HOS_P_s1 float s1
#define HOS_P_q1 hos_q q1
#define HOS_P_s2 float s2
#define HOS_P_q2 hos_q q2
#define HOS_P_s3 float s3
#define HOS_P_q3 hos_q q3
#define HOS_P_s4 float s4
#define HOS_P_q4 hos_q q4
#define HOS_P_s5 float s5
#define HOS_P_q5 hos_q q5
#define HOS_P_s6 float s6
#define HOS_P_q6 hos_q q6
#define HOS_P_s7 float s7
#define HOS_P_q7 hos_q q7
#define HOS_P_s8 float s8
#define HOS_P_q8 hos_q q8
#define HOS_P_s9 float s9
#define HOS_P_q9 hos_q q9
#define HOS_P_s10 float s10
#define HOS_P_q10 hos_q q10
#define HOS_P_s11 float s11
#define HOS_P_q11 hos_q q11
#define HOS_P_s12 float s12
#define HOS_P_q12 hos_q q12
#define HOS_P_s13 float s13
#define HOS_P_q13 hos_q q13
#define HOS_P_s14 float s14
#define HOS_P_q14 hos_q q14
#define HOS_P_s15 float s15
#define HOS_P_q15 hos_q q15
#define HOS_P_s16 float s16
#define HOS_P_q16 hos_q q16
#define HOS_P_s17 float s17
#define HOS_P_q17 hos_q q17
#define HOS_P_s18 float s18
#define HOS_P_q18 hos_q q18
#define HOS_P_s19 float s19
#define HOS_P_q19 hos_q q19
#define HOS_P_s20 float s20
#define HOS_P_q20 hos_q q20
#define HOS_P_s21 float s21
#define HOS_P_q21 hos_q q21
#define HOS_P_s22 float s22
#define HOS_P_q22 hos_q q22
#define HOS_P_s23 float s23
#define HOS_P_q23 hos_q q23
#define HOS_P_s24 float s24
#define HOS_P_q24 hos_q q24
#define HOS_P_s25 float s25
#define HOS_P_q25 hos_q q25
#define HOS_P_s26 float s26
#define HOS_P_q26 hos_q q26
#define HOS_P_s27 float s27
#define HOS_P_q27 hos_q q27
#define HOS_P_s28 float s28
#define HOS_P_q28 hos_q q28
#define HOS_P_s29 float s29
#define HOS_P_q29 hos_q q29
#define HOS_P_s30 float s30
#define HOS_P_q30 hos_q q30
#define HOS_P_s31 float s31
#define HOS_P_q31 hos_q q31
#define HOS_R_x0 uint64_t
#define HOS_R_w0 uint32_t
#define HOS_R_x1 uint64_t
#define HOS_R_w1 uint32_t
#define HOS_R_x2 uint64_t
#define HOS_R_w2 uint32_t
#define HOS_R_x3 uint64_t
#define HOS_R_w3 uint32_t
#define HOS_R_x4 uint64_t
#define HOS_R_w4 uint32_t
#define HOS_R_x5 uint64_t
#define HOS_R_w5 uint32_t
#define HOS_R_x6 uint64_t
#define HOS_R_w6 uint32_t
#define HOS_R_x7 uint64_t
#define HOS_R_w7 uint32_t
#define HOS_R_x8 uint64_t
#define HOS_R_w8 uint32_t
#define HOS_R_x9 uint64_t
#define HOS_R_w9 uint32_t
#define HOS_R_x10 uint64_t
#define HOS_R_w10 uint32_t
#define HOS_R_x11 uint64_t
#define HOS_R_w11 uint32_t
#define HOS_R_x12 uint64_t
#define HOS_R_w12 uint32_t
#define HOS_R_x13 uint64_t
#define HOS_R_w13 uint32_t
#define HOS_R_x14 uint64_t
#define HOS_R_w14 uint32_t
#define HOS_R_x15 uint64_t
#define HOS_R_w15 uint32_t
#define HOS_R_x16 uint64_t
#define HOS_R_w16 uint32_t
#define HOS_R_x17 uint64_t
#define HOS_R_w17 uint32_t
#define HOS_R_x18 uint64_t
#define HOS_R_w18 uint32_t
#define HOS_R_x19 uint64_t
#define HOS_R_w19 uint32_t
#define HOS_R_x20 uint64_t
#define HOS_R_w20 uint32_t
#define HOS_R_x21 uint64_t
#define HOS_R_w21 uint32_t
#define HOS_R_x22 uint64_t
#define HOS_R_w22 uint32_t
#define HOS_R_x23 uint64_t
#define HOS_R_w23 uint32_t
#define HOS_R_x24 uint64_t
#define HOS_R_w24 uint32_t
#define HOS_R_x25 uint64_t
#define HOS_R_w25 uint32_t
#define HOS_R_x26 uint64_t
#define HOS_R_w26 uint32_t
#define HOS_R_x27 uint64_t
#define HOS_R_w27 uint32_t
#define HOS_R_x28 uint64_t
#define HOS_R_w28 uint32_t
#define HOS_R_x29 uint64_t
#define HOS_R_w29 uint32_t
#define HOS_R_x30 uint64_t
#define HOS_R_w30 uint32_t
#define HOS_R_s0 float
#define HOS_R_q0 hos_q
#define HOS_R_s1 float
#define HOS_R_q1 hos_q
#define HOS_R_s2 float
#define HOS_R_q2 hos_q
#define HOS_R_s3 float
#define HOS_R_q3 hos_q
#define HOS_R_s4 float
#define HOS_R_q4 hos_q
#define HOS_R_s5 float
#define HOS_R_q5 hos_q
#define HOS_R_s6 float
#define HOS_R_q6 hos_q
#define HOS_R_s7 float
#define HOS_R_q7 hos_q
#define HOS_R_s8 float
#define HOS_R_q8 hos_q
#define HOS_R_s9 float
#define HOS_R_q9 hos_q
#define HOS_R_s10 float
#define HOS_R_q10 hos_q
#define HOS_R_s11 float
#define HOS_R_q11 hos_q
#define HOS_R_s12 float
#define HOS_R_q12 hos_q
#define HOS_R_s13 float
#define HOS_R_q13 hos_q
#define HOS_R_s14 float
#define HOS_R_q14 hos_q
#define HOS_R_s15 float
#define HOS_R_q15 hos_q
#define HOS_R_s16 float
#define HOS_R_q16 hos_q
#define HOS_R_s17 float
#define HOS_R_q17 hos_q
#define HOS_R_s18 float
#define HOS_R_q18 hos_q
#define HOS_R_s19 float
#define HOS_R_q19 hos_q
#define HOS_R_s20 float
#define HOS_R_q20 hos_q
#define HOS_R_s21 float
#define HOS_R_q21 hos_q
#define HOS_R_s22 float
#define HOS_R_q22 hos_q
#define HOS_R_s23 float
#define HOS_R_q23 hos_q
#define HOS_R_s24 float
#define HOS_R_q24 hos_q
#define HOS_R_s25 float
#define HOS_R_q25 hos_q
#define HOS_R_s26 float
#define HOS_R_q26 hos_q
#define HOS_R_s27 float
#define HOS_R_q27 hos_q
#define HOS_R_s28 float
#define HOS_R_q28 hos_q
#define HOS_R_s29 float
#define HOS_R_q29 hos_q
#define HOS_R_s30 float
#define HOS_R_q30 hos_q
#define HOS_R_s31 float
#define HOS_R_q31 hos_q
#define HOS_IO_x0 uint64_t x0;
#define HOS_IO_x1 uint64_t x1;
#define HOS_IO_x2 uint64_t x2;
#define HOS_IO_x3 uint64_t x3;
#define HOS_IO_x4 uint64_t x4;
#define HOS_IO_x5 uint64_t x5;
#define HOS_IO_x6 uint64_t x6;
#define HOS_IO_x7 uint64_t x7;
#define HOS_IO_x8 uint64_t x8;
#define HOS_IO_x9 uint64_t x9;
#define HOS_IO_x10 uint64_t x10;
#define HOS_IO_x11 uint64_t x11;
#define HOS_IO_x12 uint64_t x12;
#define HOS_IO_x13 uint64_t x13;
#define HOS_IO_x14 uint64_t x14;
#define HOS_IO_x15 uint64_t x15;
#define HOS_IO_x16 uint64_t x16;
#define HOS_IO_x17 uint64_t x17;
#define HOS_IO_x18 uint64_t x18;
#define HOS_IO_x19 uint64_t x19;
#define HOS_IO_x20 uint64_t x20;
#define HOS_IO_x21 uint64_t x21;
#define HOS_IO_x22 uint64_t x22;
#define HOS_IO_x23 uint64_t x23;
#define HOS_IO_x24 uint64_t x24;
#define HOS_IO_x25 uint64_t x25;
#define HOS_IO_x26 uint64_t x26;
#define HOS_IO_x27 uint64_t x27;
#define HOS_IO_x28 uint64_t x28;
#define HOS_IO_x29 uint64_t x29;
#define HOS_IO_x30 uint64_t x30;
#define HOS_IO_w0 uint64_t w0;
#define HOS_IO_w1 uint64_t w1;
#define HOS_IO_w2 uint64_t w2;
#define HOS_IO_w3 uint64_t w3;
#define HOS_IO_w4 uint64_t w4;
#define HOS_IO_w5 uint64_t w5;
#define HOS_IO_w6 uint64_t w6;
#define HOS_IO_w7 uint64_t w7;
#define HOS_IO_w8 uint64_t w8;
#define HOS_IO_w9 uint64_t w9;
#define HOS_IO_w10 uint64_t w10;
#define HOS_IO_w11 uint64_t w11;
#define HOS_IO_w12 uint64_t w12;
#define HOS_IO_w13 uint64_t w13;
#define HOS_IO_w14 uint64_t w14;
#define HOS_IO_w15 uint64_t w15;
#define HOS_IO_w16 uint64_t w16;
#define HOS_IO_w17 uint64_t w17;
#define HOS_IO_w18 uint64_t w18;
#define HOS_IO_w19 uint64_t w19;
#define HOS_IO_w20 uint64_t w20;
#define HOS_IO_w21 uint64_t w21;
#define HOS_IO_w22 uint64_t w22;
#define HOS_IO_w23 uint64_t w23;
#define HOS_IO_w24 uint64_t w24;
#define HOS_IO_w25 uint64_t w25;
#define HOS_IO_w26 uint64_t w26;
#define HOS_IO_w27 uint64_t w27;
#define HOS_IO_w28 uint64_t w28;
#define HOS_IO_w29 uint64_t w29;
#define HOS_IO_w30 uint64_t w30;
#define HOS_IO_q0 hos_q q0;
#define HOS_IO_q1 hos_q q1;
#define HOS_IO_q2 hos_q q2;
#define HOS_IO_q3 hos_q q3;
#define HOS_IO_q4 hos_q q4;
#define HOS_IO_q5 hos_q q5;
#define HOS_IO_q6 hos_q q6;
#define HOS_IO_q7 hos_q q7;
#define HOS_IO_q8 hos_q q8;
#define HOS_IO_q9 hos_q q9;
#define HOS_IO_q10 hos_q q10;
#define HOS_IO_q11 hos_q q11;
#define HOS_IO_q12 hos_q q12;
#define HOS_IO_q13 hos_q q13;
#define HOS_IO_q14 hos_q q14;
#define HOS_IO_q15 hos_q q15;
#define HOS_IO_q16 hos_q q16;
#define HOS_IO_q17 hos_q q17;
#define HOS_IO_q18 hos_q q18;
#define HOS_IO_q19 hos_q q19;
#define HOS_IO_q20 hos_q q20;
#define HOS_IO_q21 hos_q q21;
#define HOS_IO_q22 hos_q q22;
#define HOS_IO_q23 hos_q q23;
#define HOS_IO_q24 hos_q q24;
#define HOS_IO_q25 hos_q q25;
#define HOS_IO_q26 hos_q q26;
#define HOS_IO_q27 hos_q q27;
#define HOS_IO_q28 hos_q q28;
#define HOS_IO_q29 hos_q q29;
#define HOS_IO_q30 hos_q q30;
#define HOS_IO_q31 hos_q q31;
/* (a list's elements through a macro, up to 12) */
#define HOS_COMMA() ,
#define HOS_NOSEP()
#define HOS_FE_1(m, s, a) m##a
#define HOS_FE_2(m, s, a, ...) m##a s() HOS_FE_1(m, s, __VA_ARGS__)
#define HOS_FE_3(m, s, a, ...) m##a s() HOS_FE_2(m, s, __VA_ARGS__)
#define HOS_FE_4(m, s, a, ...) m##a s() HOS_FE_3(m, s, __VA_ARGS__)
#define HOS_FE_5(m, s, a, ...) m##a s() HOS_FE_4(m, s, __VA_ARGS__)
#define HOS_FE_6(m, s, a, ...) m##a s() HOS_FE_5(m, s, __VA_ARGS__)
#define HOS_FE_7(m, s, a, ...) m##a s() HOS_FE_6(m, s, __VA_ARGS__)
#define HOS_FE_8(m, s, a, ...) m##a s() HOS_FE_7(m, s, __VA_ARGS__)
#define HOS_FE_9(m, s, a, ...) m##a s() HOS_FE_8(m, s, __VA_ARGS__)
#define HOS_FE_10(m, s, a, ...) m##a s() HOS_FE_9(m, s, __VA_ARGS__)
#define HOS_FE_11(m, s, a, ...) m##a s() HOS_FE_10(m, s, __VA_ARGS__)
#define HOS_FE_12(m, s, a, ...) m##a s() HOS_FE_11(m, s, __VA_ARGS__)
#define HOS_FE_N(_1, _2, _3, _4, _5, _6, _7, _8, _9, _10, _11, _12, n, ...) HOS_FE_##n
#define HOS_FOR_EACH(m, s, ...) \
    HOS_FE_N(__VA_ARGS__, 12, 11, 10, 9, 8, 7, 6, 5, 4, 3, 2, 1, 0)(m, s, __VA_ARGS__)
/* (io's slots: a list of its own, as a macro's expansion does not expand that macro again) */
#define HOS_IE_1(a) HOS_IO_##a
#define HOS_IE_2(a, ...) HOS_IO_##a HOS_IE_1(__VA_ARGS__)
#define HOS_IE_3(a, ...) HOS_IO_##a HOS_IE_2(__VA_ARGS__)
#define HOS_IE_4(a, ...) HOS_IO_##a HOS_IE_3(__VA_ARGS__)
#define HOS_IE_5(a, ...) HOS_IO_##a HOS_IE_4(__VA_ARGS__)
#define HOS_IE_6(a, ...) HOS_IO_##a HOS_IE_5(__VA_ARGS__)
#define HOS_IE_7(a, ...) HOS_IO_##a HOS_IE_6(__VA_ARGS__)
#define HOS_IE_8(a, ...) HOS_IO_##a HOS_IE_7(__VA_ARGS__)
#define HOS_IE_9(a, ...) HOS_IO_##a HOS_IE_8(__VA_ARGS__)
#define HOS_IE_10(a, ...) HOS_IO_##a HOS_IE_9(__VA_ARGS__)
#define HOS_IE_11(a, ...) HOS_IO_##a HOS_IE_10(__VA_ARGS__)
#define HOS_IE_12(a, ...) HOS_IO_##a HOS_IE_11(__VA_ARGS__)
#define HOS_IE_N(_1, _2, _3, _4, _5, _6, _7, _8, _9, _10, _11, _12, n, ...) HOS_IE_##n
#define HOS_IO_EACH(...) HOS_IE_N(__VA_ARGS__, 12, 11, 10, 9, 8, 7, 6, 5, 4, 3, 2, 1, 0)(__VA_ARGS__)
#define HOS_PARAMS(...) __VA_OPT__(HOS_FOR_EACH(HOS_P_, HOS_COMMA, __VA_ARGS__))
#define HOS_TYPES(...) __VA_OPT__(HOS_FOR_EACH(HOS_T_, HOS_COMMA, __VA_ARGS__))

/* the declarations (word(W) is checked by the lifter, not the compiler: it expands to nothing here) */
#define PATCH_AT(pc, word, mode, result, fn, ...)                                                                     \
    HOS_PATCH_RECORD("PATCH_AT", #pc "," #word "," #mode "," #result "," #fn "," #__VA_ARGS__)                     \
    __VA_OPT__(HOS_FOR_EACH(HOS_D_, HOS_NOSEP, __VA_ARGS__))                                                         \
    HOS_R_##result fn(HOS_PARAMS(__VA_ARGS__))
#define PATCH_HOOK(addr, word, ...)                                                                                   \
    HOS_PATCH_RECORD("PATCH_HOOK", #addr "," #word "," #__VA_ARGS__)                                                 \
    ENTRY_HOOK_COUNTED(entry_##addr, entry_body_##addr)
#define PATCH_HOOK_SYM(sym, ...)                                                                                      \
    HOS_PATCH_RECORD("PATCH_HOOK_SYM", #sym "," #__VA_ARGS__)                                                        \
    ENTRY_HOOK_COUNTED(entry_sym_##sym, entry_body_sym_##sym)
#define HLE(sym, ...) HOS_PATCH_RECORD("HLE", #sym "," #__VA_ARGS__) void imp_##sym(Ctx *c)
#define NV(name, ...) HOS_PATCH_RECORD("NV", #name "," #__VA_ARGS__) static void n_##name(Ctx *c, void *u_)
#define HLE_EFFECTS(sym, ...) HOS_PATCH_RECORD("HLE_EFFECTS", #sym "," #__VA_ARGS__) _Static_assert(1, #sym)
#define PATCH_AT_SITE(pc, word, mode, result, fn, ...)                                                                \
    HOS_PATCH_RECORD("PATCH_AT_SITE", #pc "," #word "," #mode "," #result "," #fn "," #__VA_ARGS__)                \
    _Static_assert(__builtin_types_compatible_p(__typeof__(&fn), HOS_R_##result (*)(HOS_TYPES(__VA_ARGS__))),       \
                   "PATCH_AT_SITE " #pc ": its operands differ from " #fn "'s parameters")
#endif
