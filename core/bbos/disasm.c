/* PocketPager - BlackBerry OS family: symbolic disassembler (debug aid). */
#include "vm.h"
#include <stdio.h>
#include <string.h>

ref_t interp_literal(BBVM *vm, BBModule *m, uint16_t off, int unicode);

static const char *routine_label(BBVM *vm, BBRoutine *r, char *b, size_t n) {
    if (!r) { snprintf(b, n, "?"); return b; }
    char sig[256];
    vm_sig(vm, r->mod, r->proto, !r->isStatic, sig, sizeof sig);
    snprintf(b, n, "%s.%s(%s)", r->cls ? r->cls->full : "?", r->name, sig);
    return b;
}

static const char *member_label(BBVM *vm, BBModule *m, const BBFixup *fx, char *b, size_t n) {
    BBMemberRef *mr = &m->mrefs[fx->idx];
    if (fx->kind == FX_LFIELD) {
        BBClass *k = &m->cls[mr->lcls];
        snprintf(b, n, "%s.%s", k->full, mr->lfield < k->nf ? k->f[mr->lfield].name : "?");
        return b;
    }
    BBClassRef *cr = mr->cref >= 0 ? &m->crefs[mr->cref] : NULL;
    char sig[256] = "";
    if (fx->kind == FX_ROUTINE || fx->kind == FX_SROUTINE || fx->kind == FX_VROUTINE)
        vm_sig(vm, m, mr->tl, fx->kind != FX_SROUTINE || !strcmp(mr->name, "<init>"), sig, sizeof sig);
    snprintf(b, n, "%s.%s.%s%s%s%s", cr ? cr->pkg : "?", cr ? cr->cls : "?", mr->name,
             *sig || fx->kind <= FX_VROUTINE ? "(" : "", sig, *sig || fx->kind <= FX_VROUTINE ? ")" : "");
    return b;
}

static const char *static_label(BBVM *vm, BBModule *m, int mo, int co, int addr, char *b, size_t n) {
    BBClass *k = vm_cls_direct(vm, m, mo, co);
    if (k) for (int i = 0; i < k->nsf; i++) if (k->sf[i].addr == addr) { snprintf(b, n, "%s.%s", k->full, k->sf[i].name); return b; }
    snprintf(b, n, "%s#%d", k ? k->full : "?", addr);
    return b;
}

static void lit(BBVM *vm, BBModule *m, uint16_t off, int uni, char *b, size_t n) {
    ref_t r = interp_literal(vm, m, off, uni);
    char s[200];
    vm_string_utf8(vm, r, s, sizeof s);
    snprintf(b, n, "\"%s\"", s);
}

void vm_disasm(BBVM *vm, BBRoutine *r, FILE *out) {
    BBModule *m = r->mod;
    const uint8_t *c = m->code;
    char lbl[600];
    fprintf(out, "  %s %s  [%s @%u, args %d, ret %d]\n", r->isStatic ? "static" : "", routine_label(vm, r, lbl, sizeof lbl), m->name, r->off, r->nargs, r->ret);
    for (uint32_t pc = r->off; pc < (uint32_t)r->off + r->size;) {
        int op; int z = cod_insn_size(c, pc, &op);
        uint32_t o = pc + 1 + (op >= 0x100);
        const BBFixup *fx = cod_fixup_at(m, (uint16_t)o);
        char arg[640] = "";
        switch (op) {
        case 0x91: case 0x92: case 0x93: case 0x94: case 0x95: case 0x96: case 0x97: case 0x98:
        case 0x99: case 0x9a: case 0x9b: case 0x9c: case 0x9d: case 0x9e: case 0x9f: case 0xa0: case 0xa1:
            snprintf(arg, sizeof arg, "-> %d", (int)(o + (int8_t)c[o])); break;
        case 0xa2: snprintf(arg, sizeof arg, "-> %d", (int)(o + (int16_t)rd16(c + o))); break;
        case 0x28: lit(vm, m, rd16(c + o), 0, arg, sizeof arg); break;
        case 0x2a: lit(vm, m, rd16(c + o + 2), 1, arg, sizeof arg); break;
        case 0x24: snprintf(arg, sizeof arg, "%d", (int8_t)c[o]); break;
        case 0x25: snprintf(arg, sizeof arg, "%d", (int16_t)rd16(c + o)); break;
        case 0x26: snprintf(arg, sizeof arg, "%d (0x%x)", (int32_t)rd32(c + o), rd32(c + o)); break;
        case 0x31: case 0x33: case 0x35: case 0x47: case 0x49: case 0x4b: case 0xa5:
            snprintf(arg, sizeof arg, "%d", c[o]); break;
        case 0x78: snprintf(arg, sizeof arg, "%d, %d", c[o], (int8_t)c[o + 1]); break;
        case 0x01:
            if (fx) member_label(vm, m, fx, arg, sizeof arg);
            else snprintf(arg, sizeof arg, "vtable[%d] nargs %d", (int16_t)rd16(c + o), c[o + 2]);
            break;
        case 0xde: snprintf(arg, sizeof arg, "vtable[%d] nargs %d", c[o] >> 2, (c[o] & 3) + 1); break;
        case 0x03: case 0x05: case 0x04: case 0x06: case 0x0c: case 0x0d: {
            int lib = op == 0x04 || op == 0x06 || op == 0x0d;
            if (fx && fx->kind != FX_MODULE) member_label(vm, m, fx, arg, sizeof arg);
            else { BBModule *t = vm_mod_ord(vm, m, lib ? c[o] : 0); routine_label(vm, t ? cod_routine_at(t, rd16(c + o + lib)) : NULL, arg, sizeof arg); }
            break;
        }
        case 0x07: case 0x08: {
            int lib = op == 0x08;
            if (fx && fx->kind != FX_MODULE) member_label(vm, m, fx, arg, sizeof arg);
            else { BBModule *t = vm_mod_ord(vm, m, lib ? c[o] : 0); routine_label(vm, t ? cod_routine_at(t, rd16(c + o + lib + 1)) : NULL, arg, sizeof arg); }
            break;
        }
        case 0x02: {
            uint16_t io = rd16(c + o);
            BBClass *k = vm_cls_ref(vm, m, m->data[io], m->data[io + 1]);
            char sig[256]; vm_sig(vm, m, rd16(m->data + io + 4), 1, sig, sizeof sig);
            snprintf(arg, sizeof arg, "%s.%s(%s)", k ? k->full : "?", cod_name(m, rd16(m->data + io + 2)), sig);
            break;
        }
        case 0x19: case 0x1a: case 0x1c: case 0x1d: case 0x5d: case 0x5e: case 0x5f: case 0x60:
        case 0x61: case 0x62: case 0x63: case 0x64: case 0x65: case 0x66: case 0x67: case 0x68:
            if (fx) member_label(vm, m, fx, arg, sizeof arg);
            else snprintf(arg, sizeof arg, "slot %d", c[o] + ((op & 1) == 0 && op != 0x1c && op != 0x5e ? 0 : 0));
            break;
        case 0x69: case 0x6b: case 0x6d: case 0x6f:
            if (fx) member_label(vm, m, fx, arg, sizeof arg);
            else static_label(vm, m, 0, c[o], rd16(c + o + 1), arg, sizeof arg);
            break;
        case 0x6a: case 0x6c: case 0x6e: case 0x70:
            if (fx && fx->kind != FX_MODULE) member_label(vm, m, fx, arg, sizeof arg);
            else static_label(vm, m, c[o], c[o + 1], rd16(c + o + 2), arg, sizeof arg);
            break;
        case 0xb8: case 0xb9: case 0xba: case 0xbb: case 0xbf: case 0xc0: case 0xc1: case 0xc2:
        case 0xa8: case 0xa9: case 0x13: case 0xc3: case 0xc4: case 0xc6: case 0xc7: case 0xc8: case 0xc9: case 0xaa: case 0xab: {
            int lib = op == 0xb9 || op == 0xbb || op == 0xc0 || op == 0xc2 || op == 0xa9 || op == 0xc4 || op == 0xc7 || op == 0xc9 || op == 0xab;
            BBClass *k = (fx && fx->kind == FX_CLASS) ? vm_resolve_cref(vm, m, (int)fx->idx) : vm_cls_ref(vm, m, lib ? c[o] : 0, c[o + lib]);
            snprintf(arg, sizeof arg, "%s", k ? k->full : "?");
            if (op == 0xc3 || op == 0xc4) { uint32_t q = o + lib + 1; snprintf(arg + strlen(arg), sizeof arg - strlen(arg), " else -> %d", (int)(q + (int16_t)rd16(c + q))); }
            break;
        }
        case 0x2f: {
            int cnt = rd16(c + o); int32_t low = (int32_t)rd32(c + o + 2);
            uint32_t q = o + 6; size_t L = 0;
            for (int i = 0; i < cnt && L + 40 < sizeof arg; i++, q += 2)
                L += (size_t)snprintf(arg + L, sizeof arg - L, "%s%d->%d ", i ? "" : "default:", i ? low + i : 0, (int)(q + (int16_t)rd16(c + q)));
            break;
        }
        case 0xa3: case 0xa4: {
            int cnt = rd16(c + o), w = op == 0xa3 ? 2 : 4;
            uint32_t q = o + 2;
            size_t L = 0;
            for (int i = 0; i < cnt && L + 40 < sizeof arg; i++) {
                int32_t key = w == 2 ? (int16_t)rd16(c + q) : (int32_t)rd32(c + q);
                q += w;
                L += (size_t)snprintf(arg + L, sizeof arg - L, "%d(0x%x)->%d ", key, key, (int)(q + (int16_t)rd16(c + q)));
                q += 2;
            }
            if (L + 20 < sizeof arg) snprintf(arg + L, sizeof arg - L, "default->%d", (int)(q + (int16_t)rd16(c + q)));
            break;
        }
        default:
            for (int i = 1 + (op >= 0x100); i < z && i < 8; i++) snprintf(arg + strlen(arg), sizeof arg - strlen(arg), "%02x ", c[pc + i]);
            break;
        }
        fprintf(out, "    %5u  %-24s %s\n", pc, cod_op_name(op), arg);
        pc += z;
    }
}

void vm_disasm_class(BBVM *vm, BBClass *k, FILE *out) {
    vm_link(vm, k);
    fprintf(out, "class %s extends %s  (%s, %d slots)\n", k->full, k->super ? k->super->full : "-", k->mod->name, k->nslots);
    for (int i = 0; i < k->nf; i++) {
        char s[200]; vm_sig(vm, k->mod, k->f[i].tl, 0, s, sizeof s);
        fprintf(out, "  field %s %s  slot %d\n", s, k->f[i].name, k->f[i].slot);
    }
    for (int i = 0; i < k->nsf; i++) {
        char s[200]; vm_sig(vm, k->mod, k->sf[i].tl, 0, s, sizeof s);
        fprintf(out, "  static %s %s  @%d\n", s, k->sf[i].name, k->sf[i].addr);
    }
    for (int i = 0; i < k->vtsize; i++) {
        char lb[600];
        fprintf(out, "  vtable[%d] = %s\n", i, routine_label(vm, k->vtable[i], lb, sizeof lb));
    }
    BBRoutine **lists[3] = { k->v, k->nv_, k->s };
    int cnt[3] = { k->nv, k->nnv, k->ns };
    for (int l = 0; l < 3; l++) for (int i = 0; i < cnt[l]; i++) if (lists[l][i]) vm_disasm(vm, lists[l][i], out);
}
