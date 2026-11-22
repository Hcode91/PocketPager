/* bbdump: list a .cod module's classes, routines and fixups. */
#include "../core/bbos/cod.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static uint8_t *slurp(const char *p, size_t *n) {
    FILE *f = fopen(p, "rb"); if (!f) return NULL;
    fseek(f, 0, SEEK_END); *n = (size_t)ftell(f); fseek(f, 0, SEEK_SET);
    uint8_t *b = malloc(*n); if (fread(b, 1, *n, f) != *n) { fclose(f); free(b); return NULL; }
    fclose(f); return b;
}

static void dis(BBModule *m, BBRoutine *r) {
    uint32_t pc = r->off, end = r->off + r->size;
    while (pc < end) {
        int op; int z = cod_insn_size(m->code, pc, &op);
        printf("      %5u %-22s", pc, cod_op_name(op));
        for (int i = 1; i < z && i < 8; i++) printf(" %02x", m->code[pc + i]);
        for (int i = 1; i < z && i < 6; i++) {
            const BBFixup *f = cod_fixup_at(m, (uint16_t)(pc + i));
            if (!f) continue;
            if (f->kind == FX_CLASS) printf("  ; class %s.%s", m->crefs[f->idx].pkg, m->crefs[f->idx].cls);
            else if (f->kind == FX_MODULE) printf("  ; module %d", f->idx);
            else {
                BBMemberRef *mr = &m->mrefs[f->idx];
                if (mr->cref >= 0) printf("  ; %s.%s::%s", m->crefs[mr->cref].pkg, m->crefs[mr->cref].cls, mr->name);
                else printf("  ; local field %d/%d", mr->lcls, mr->lfield);
            }
            break;
        }
        printf("\n");
        pc += z;
    }
}

int main(int argc, char **argv) {
    if (argc < 2) { fprintf(stderr, "usage: bbdump file.cod [-d]\n"); return 1; }
    size_t n; uint8_t *b = slurp(argv[1], &n);
    if (!b) { perror(argv[1]); return 1; }
    char err[256];
    const char *base = strrchr(argv[1], '/'); base = base ? base + 1 : argv[1];
    BBModule *m = cod_parse(base, b, n, err, sizeof err);
    if (!m) { fprintf(stderr, "%s\n", err); return 1; }
    int d = argc > 2 && !strcmp(argv[2], "-d");
    printf("module %s ver %u code %u data %u dver %u statics %u\n", cod_cstr(m, m->modNameOff[0]), m->version, m->codeSize, m->dataSize, m->dver, m->staticSize);
    for (int i = 0; i < m->nmod; i++) printf("  mod[%d] %s\n", i, cod_cstr(m, m->modNameOff[i]));
    printf("  %d class refs, %d member refs, %d fixups, %d routines\n", m->ncref, m->nmref, m->nfix, m->nroutines);
    for (int i = 0; i < m->ncls; i++) {
        BBClass *k = &m->cls[i];
        printf("class %d %s super(%d,%d) attr %x fields %d sfields %d ifaces %d\n", i, k->full, k->supM, k->supC, k->attr, k->nf, k->nsf, k->nif);
        BBRoutine **lists[3] = { k->v, k->nv_, k->s }; int cnt[3] = { k->nv, k->nnv, k->ns };
        const char *tag[3] = { "virtual", "nonvirt", "static" };
        for (int l = 0; l < 3; l++) for (int j = 0; j < cnt[l]; j++) {
            BBRoutine *r = lists[l][j]; if (!r) continue;
            printf("    %s %s @%u size %u args %d ret %d attr %x%s\n", tag[l], r->name, r->off, r->size, r->nargs, r->ret, r->attr, r->exOff ? " [ex]" : "");
            if (d) dis(m, r);
        }
    }
    cod_free(m);
    return 0;
}
