/* Patch declarations: host code at guest addresses, written next to its body (docs/patches.md).

   The lifter reads these macros from the source with a token scanner of a fixed grammar (ounce/lift/src/patch.rs), and
   the compiler expands the same text into the host function's prototype and a record in the object section hos_patch
   that the build compares with the scan (ounce/builder): a declaration one of them sees and the other does not stops
   the build. A declaration stands at file scope, outside #if, and is never made by another macro.

     PATCH_AT(pc, word(W), mode, result, fn, operand...) { body }
       mode: after (the instruction at pc; after a BL / BLR when it returns), before (on the edge into it: pc must be no
       branch target), instead (in place of a BL / BLR: the call is not made, x30 set as it would be, the target
       register of a BLR an operand like any other; in place of a BR that is a tail call, no jump table's: fn's result
       is the tail callee's, then the function returns; in place of any other instruction: its result, the
       instruction's destination, is fn's). The containing function is found by the
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
     PATCH_FUNC(addr, word(W), body(H), aapcs, result, fn, [orig,] [guest(A, ...),] operand...) { body }
     PATCH_FUNC_SYM(symbol, aapcs, result, fn, [orig,] [guest(A, ...),] operand...) { body }
       the lifted function at addr (its first word W and its body's hash H, ounce/lift/src/patch.rs body_hash, checked
       at the build), or the one the image exports as symbol, replaced by fn: every call of it is a call of fn with the
       declared operands (x0-x8, w0-w8, s0-s7, q0-q7, x30 for the return address, sp; as(...) names them), its result
       (none, x0, w0, s0, q0) the call's. The operands are what fn itself uses. orig, first: fn may run the function's
       own lifted body, GuestFn *orig, as guest_orig(orig, a0, ...): a0.. go to x0.. (an argument fn changes is passed
       so, never written into the context: fn has none), every other register is the call's own. guest(A, ...): fn
       calls those guest functions through guest_call (GuestCall *g, after orig), on the call's stack: sp is then an
       operand.
     HLE(symbol, effect...) { body }     NV(name, effect...) { body }     HLE_EFFECTS(symbol, effect...);
       a function the image replaces: void imp_<symbol>(Ctx *c) (hooked where the game imports it and at the SDK's
       definition); an NVN proc, static void n_<name>(Ctx *c, void *u_) (runtime/nvn_native.c's table); the effects of a
       replaced function whose body is not declared here (a generated stub or dispatch). effects: reads(reg..., "why")
       the registers a call reads beyond the AAPCS arguments, x8, v0-v7, sp and x30; clobbers("why") a call keeps no
       register; sdk(min) or sdk(min, max) (HLE, HLE_EFFECTS) the SDK majors the symbol exists in: on an image of another
       SDK (its sdk module's nnSdk-..._<major>_... string) the declaration is left out, on one of these a missing symbol
       stops the lift as for any other.
     PATCH_ANSWER(symbol, value, "why", [sdk(min[, max])]);
       a function the image replaces with a constant answer: x0 is value and nothing else is written (any other result
       register keeps what it held). No host function: at a direct call (BL, or a tail branch) the lifter writes value to
       x0 and x30 as the BL would, and makes no call; the function's generic entry f_<addr> (a code pointer, a BLR) is
       lifted IR doing the same (ounce/lift answers_ir). An import the game calls that the host neither
       implements nor answers stops the game, naming it (ounce/lift/src/gen.rs gen_stubs; a direct call of it is noreturn):
       each answer is one the game was seen to need, with the reason it is enough.
     SVC(number, name, inputs, results) { body }
       the host's answer to the guest's `svc #number` (number: 0x and two upper-case hex digits), unsigned __int128
       hos_svc_<number>(x0..x7, pc, uint64_t *more), which the lifter calls with the registers x0 up to inputs as
       values and takes x0 up to results back (x0 and x1 returned, x2 up in more, which holds their values before);
       every other register keeps its value. No context: an SVC sees no register but its inputs. An SVC no declaration
       answers is lifted as a stop (hos_svc_stop). The body gets a SvcCall *c: c->x[n] the registers, c->pc the svc's
       address. */
#ifndef HOS_PATCH_H
#define HOS_PATCH_H
#include <stdint.h>

/* A PATCH_FUNC's orig: the replaced function's lifted body, called as guest_orig(orig, a0, ...): a0.. go to x0.. (the
   registers after them keep the call's values), the body runs on the call's context and stack, and its x0 comes back.
   The body writes what its summary says it may; the function's callers get that in its record (ounce/lift) */
typedef struct GuestFn { struct GuestContext *c; void (*body)(struct GuestContext *); } GuestFn;
#define HOS_NARGS_(_1, _2, _3, _4, _5, _6, _7, _8, _9, n, ...) n
#define HOS_NARGS(...) HOS_NARGS_(__VA_ARGS__ __VA_OPT__(,) 9, 8, 7, 6, 5, 4, 3, 2, 1, 0)
#define guest_orig(orig, ...) hos_guest_orig(orig, HOS_NARGS(__VA_ARGS__), ((const uint64_t[]){__VA_ARGS__ __VA_OPT__(,) 0}))
#define hos_guest_orig(o, n, a) ({                                                                                    \
    GuestFn *o_ = (o);                                                                                                \
    const uint64_t *a_ = (a);                                                                                         \
    for (int i_ = 0; i_ < (n); i_++) o_->c->x[i_] = a_[i_];                                                           \
    o_->body(o_->c);                                                                                                  \
    o_->c->x[0];                                                                                                      \
})
/* (a PATCH_FUNC's adapter: Sn / Vn of the context, and the result's write) */
#define hos_ctx_s(c, n) ({ float f_; __builtin_memcpy(&f_, (c)->vreg[n], 4); f_; })
#define hos_ctx_q(c, n) ({ hos_q q_; __builtin_memcpy(&q_, (c)->vreg[n], 16); q_; })
#define hos_ctx_set_s(c, n, v)                                                                                        \
    do { float f_ = (v); (c)->vreg[n][0] = (c)->vreg[n][1] = 0; __builtin_memcpy((c)->vreg[n], &f_, 4); } while (0)
#define hos_ctx_set_q(c, n, v) do { hos_q q_ = (v); __builtin_memcpy((c)->vreg[n], &q_, 16); } while (0)

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
/* (a list's elements through a macro, up to 16) */
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
#define HOS_FE_13(m, s, a, ...) m##a s() HOS_FE_12(m, s, __VA_ARGS__)
#define HOS_FE_14(m, s, a, ...) m##a s() HOS_FE_13(m, s, __VA_ARGS__)
#define HOS_FE_15(m, s, a, ...) m##a s() HOS_FE_14(m, s, __VA_ARGS__)
#define HOS_FE_16(m, s, a, ...) m##a s() HOS_FE_15(m, s, __VA_ARGS__)
#define HOS_FE_N(_1, _2, _3, _4, _5, _6, _7, _8, _9, _10, _11, _12, _13, _14, _15, _16, n, ...) HOS_FE_##n
#define HOS_FOR_EACH(m, s, ...) \
    HOS_FE_N(__VA_ARGS__, 16, 15, 14, 13, 12, 11, 10, 9, 8, 7, 6, 5, 4, 3, 2, 1, 0)(m, s, __VA_ARGS__)
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

/* (PATCH_FUNC: each operand's value at the call, read from the context by the function's adapter (HOS_A_), and the
   result's write (HOS_SET_); the registers a function may take: x0-x8, w0-w8, s0-s7, q0-q7, x30, sp) */
#define HOS_A_x0 (c->x[0])
#define HOS_A_w0 ((uint32_t)c->x[0])
#define HOS_A_x1 (c->x[1])
#define HOS_A_w1 ((uint32_t)c->x[1])
#define HOS_A_x2 (c->x[2])
#define HOS_A_w2 ((uint32_t)c->x[2])
#define HOS_A_x3 (c->x[3])
#define HOS_A_w3 ((uint32_t)c->x[3])
#define HOS_A_x4 (c->x[4])
#define HOS_A_w4 ((uint32_t)c->x[4])
#define HOS_A_x5 (c->x[5])
#define HOS_A_w5 ((uint32_t)c->x[5])
#define HOS_A_x6 (c->x[6])
#define HOS_A_w6 ((uint32_t)c->x[6])
#define HOS_A_x7 (c->x[7])
#define HOS_A_w7 ((uint32_t)c->x[7])
#define HOS_A_x8 (c->x[8])
#define HOS_A_w8 ((uint32_t)c->x[8])
#define HOS_A_x30 (c->x[30])
#define HOS_A_w30 ((uint32_t)c->x[30])
#define HOS_A_s0 hos_ctx_s(c, 0)
#define HOS_A_q0 hos_ctx_q(c, 0)
#define HOS_A_s1 hos_ctx_s(c, 1)
#define HOS_A_q1 hos_ctx_q(c, 1)
#define HOS_A_s2 hos_ctx_s(c, 2)
#define HOS_A_q2 hos_ctx_q(c, 2)
#define HOS_A_s3 hos_ctx_s(c, 3)
#define HOS_A_q3 hos_ctx_q(c, 3)
#define HOS_A_s4 hos_ctx_s(c, 4)
#define HOS_A_q4 hos_ctx_q(c, 4)
#define HOS_A_s5 hos_ctx_s(c, 5)
#define HOS_A_q5 hos_ctx_q(c, 5)
#define HOS_A_s6 hos_ctx_s(c, 6)
#define HOS_A_q6 hos_ctx_q(c, 6)
#define HOS_A_s7 hos_ctx_s(c, 7)
#define HOS_A_q7 hos_ctx_q(c, 7)
#define HOS_A_sp (c->x[31])
#define HOS_A_as(op, name) HOS_A_##op
#define HOS_A_guest(...) ((GuestCall *)c)
#define HOS_A_orig (&hos_orig_)
#define HOS_T_orig GuestFn *
#define HOS_P_orig GuestFn *orig
#define HOS_SET_none(c, v) (void)(v)
#define HOS_SET_x0(c, v) ((c)->x[0] = (v))
#define HOS_SET_w0(c, v) ((c)->x[0] = (uint32_t)(v))
#define HOS_SET_s0(c, v) hos_ctx_set_s(c, 0, v)
#define HOS_SET_q0(c, v) hos_ctx_set_q(c, 0, v)
#define HOS_ARGS(...) __VA_OPT__(HOS_FOR_EACH(HOS_A_, HOS_COMMA, __VA_ARGS__))
/* the declarations (word(W) is checked by the lifter, not the compiler: it expands to nothing here) */
#define PATCH_AT(pc, word, mode, result, fn, ...)                                                                     \
    HOS_PATCH_RECORD("PATCH_AT", #pc "," #word "," #mode "," #result "," #fn "," #__VA_ARGS__)                     \
    __VA_OPT__(HOS_FOR_EACH(HOS_D_, HOS_NOSEP, __VA_ARGS__))                                                         \
    HOS_R_##result fn(HOS_PARAMS(__VA_ARGS__))
/* (a PATCH_FUNC: the record, the C function's prototype, and the adapter the lifted f_<addr> tail-calls,
   hos_func_<key>(Ctx *): it reads the declared operands from the call's context, calls fn and writes its result. The
   function's own lifted body is hos_orig_<key> (ounce/lift), handed to fn as orig) */
#define HOS_FUNC_(fname, oname, result, fn, ...)                                                                      \
    static HOS_R_##result fn(HOS_PARAMS(__VA_ARGS__));                                                               \
    void oname(Ctx *A64_NOESC c) __attribute__((weak));   /* (referenced only through orig) */                        \
    void fname(Ctx *A64_NOESC c);                                                                                     \
    void fname(Ctx *A64_NOESC c) {                                                                                    \
        GuestFn hos_orig_ = {c, oname};                                                                               \
        (void)hos_orig_;                                                                                              \
        HOS_SET_##result(c, fn(HOS_ARGS(__VA_ARGS__)));                                                               \
    }                                                                                                                 \
    static HOS_R_##result fn(HOS_PARAMS(__VA_ARGS__))
#define PATCH_FUNC(addr, word, body, abi, result, fn, ...)                                                            \
    HOS_PATCH_RECORD("PATCH_FUNC", #addr "," #word "," #body "," #abi "," #result "," #fn "," #__VA_ARGS__)          \
    HOS_FUNC_(HOS_CAT(hos_func_, addr), HOS_CAT(hos_orig_, addr), result, fn, __VA_ARGS__)
#define PATCH_FUNC_SYM(sym, abi, result, fn, ...)                                                                     \
    HOS_PATCH_RECORD("PATCH_FUNC_SYM", #sym "," #abi "," #result "," #fn "," #__VA_ARGS__)                           \
    HOS_FUNC_(HOS_CAT(hos_func_sym_, sym), HOS_CAT(hos_orig_sym_, sym), result, fn, __VA_ARGS__)
#define HLE(sym, ...) HOS_PATCH_RECORD("HLE", #sym "," #__VA_ARGS__) void imp_##sym(Ctx *c)
#define NV(name, ...) HOS_PATCH_RECORD("NV", #name "," #__VA_ARGS__) static void n_##name(Ctx *c, void *u_)
#define HOS_SVC_PARAMS                                                                                                \
    uint64_t a0_, uint64_t a1_, uint64_t a2_, uint64_t a3_, uint64_t a4_, uint64_t a5_, uint64_t a6_, uint64_t a7_,  \
        uint64_t pc_
/* An SVC's call: its registers x0-x7 (beyond its inputs unspecified; its results written here), the svc's address */
typedef struct SvcCall {
    uint64_t x[8];
    uint64_t pc;
} SvcCall;
#define SVC(number, name, inputs, results)                                                                            \
    HOS_PATCH_RECORD("SVC", #number "," #name "," #inputs "," #results)                                               \
    _Static_assert((inputs) >= 0 && (inputs) <= 8 && (results) >= 0 && (results) <= 8, "SVC " #name);              \
    static inline __attribute__((always_inline)) void svc_##name(SvcCall *c);                                       \
    unsigned __int128 hos_svc_##number(HOS_SVC_PARAMS, uint64_t *more_);                                             \
    unsigned __int128 hos_svc_##number(HOS_SVC_PARAMS, uint64_t *more_) {                                            \
        SvcCall s_ = {{a0_, a1_, a2_, a3_, a4_, a5_, a6_, a7_}, pc_};                                                 \
        for (int i_ = 2; i_ < (results); i_++) s_.x[i_] = more_[i_ - 2];                                              \
        svc_##name(&s_);                                                                                              \
        for (int i_ = 2; i_ < (results); i_++) more_[i_ - 2] = s_.x[i_];                                              \
        return ((results) > 0 ? s_.x[0] : 0) | (unsigned __int128)((results) > 1 ? s_.x[1] : 0) << 64;                \
    }                                                                                                                 \
    static inline __attribute__((always_inline)) void svc_##name(SvcCall *c)
#define PATCH_ANSWER(sym, value, why, ...)                                                                            \
    HOS_PATCH_RECORD("PATCH_ANSWER", #sym "," #value "," #why "," #__VA_ARGS__) _Static_assert(1, #sym)
#define HLE_EFFECTS(sym, ...) HOS_PATCH_RECORD("HLE_EFFECTS", #sym "," #__VA_ARGS__) _Static_assert(1, #sym)
#define PATCH_AT_SITE(pc, word, mode, result, fn, ...)                                                                \
    HOS_PATCH_RECORD("PATCH_AT_SITE", #pc "," #word "," #mode "," #result "," #fn "," #__VA_ARGS__)                \
    _Static_assert(__builtin_types_compatible_p(__typeof__(&fn), HOS_R_##result (*)(HOS_TYPES(__VA_ARGS__))),       \
                   "PATCH_AT_SITE " #pc ": its operands differ from " #fn "'s parameters")
#endif
