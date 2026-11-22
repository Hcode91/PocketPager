/* PocketPager - BlackBerry OS family: calendar, compression, randomness and
 * other small natives. */
#include "natives.h"

/* stb_image (compiled in natives_gfx.c) carries a zlib decoder */
extern char *stbi_zlib_decode_malloc_guesssize_headerflag(const char *buffer, int len, int initial_size, int *outlen, int parse_header);

/* ------------------------------------------------------------------ */
/* GregorianCalendar: fields live in int slots, each followed by a "set"
 * stamp. Field index -> slot: ERA 5, YEAR 7, MONTH 9, DATE 11,
 * DAY_OF_YEAR 13, DAY_OF_WEEK 15, AM_PM 17, HOUR 19, HOUR_OF_DAY 21,
 * MINUTE 23, SECOND 25, MILLISECOND 27; slot 2 is the time (long). */

enum { C_TIME = 2, C_ERA = 5, C_YEAR = 7, C_MONTH = 9, C_DATE = 11, C_DOY = 13, C_DOW = 15,
       C_AMPM = 17, C_HOUR = 19, C_HOD = 21, C_MIN = 23, C_SEC = 25, C_MS = 27 };

static int64_t days_from_civil(int64_t y, int m, int d) {   /* m 1..12 */
    y -= m <= 2;
    int64_t era = (y >= 0 ? y : y - 399) / 400;
    int64_t yoe = y - era * 400;
    int64_t doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    int64_t doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + doe - 719468;
}
static void civil_from_days(int64_t z, int64_t *y, int *m, int *d) {
    z += 719468;
    int64_t era = (z >= 0 ? z : z - 146096) / 146097;
    int64_t doe = z - era * 146097;
    int64_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    int64_t doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    int64_t mp = (5 * doy + 2) / 153;
    *d = (int)(doy - (153 * mp + 2) / 5 + 1);
    *m = (int)(mp < 10 ? mp + 3 : mp - 9);
    *y = yoe + era * 400 + (*m <= 2);
}
static int64_t floordiv(int64_t a, int64_t b) { int64_t q = a / b; if ((a % b) && ((a < 0) != (b < 0))) q--; return q; }

static void cset(Obj *c, int slot, int v) { c->d[slot] = (uint32_t)v; c->d[slot + 1] = 1; }

NAT(cal_timeToDateFields) {
    UNUSED;
    Obj *c = vm_obj(vm, a[0]);
    if (!c || c->len < 34) { ret[0] = 0; return NAT_OK; }
    int64_t ms = nat_arg64(a + 1);
    int64_t days = floordiv(ms, 86400000);
    int msday = (int)(ms - days * 86400000);
    int64_t y; int m, d;
    civil_from_days(days, &y, &m, &d);
    int era = y > 0;
    cset(c, C_ERA, era);
    cset(c, C_YEAR, (int)(era ? y : 1 - y));
    cset(c, C_MONTH, m - 1);
    cset(c, C_DATE, d);
    cset(c, C_DOY, (int)(days - days_from_civil(y, 1, 1)) + 1);
    cset(c, C_DOW, (int)(((days % 7) + 7 + 4) % 7) + 1);   /* 1970-01-01 was a Thursday; Sunday = 1 */
    ret[0] = (uint32_t)msday;
    return NAT_OK;
}
NAT(cal_timeToRemainingFields) {
    UNUSED;
    Obj *c = vm_obj(vm, a[0]);
    if (!c || c->len < 34) return NAT_OK;
    int ms = (int)a[1];
    int hod = ms / 3600000;
    cset(c, C_HOD, hod);
    cset(c, C_AMPM, hod >= 12);
    cset(c, C_HOUR, hod % 12);
    cset(c, C_MIN, ms / 60000 % 60);
    cset(c, C_SEC, ms / 1000 % 60);
    cset(c, C_MS, ms % 1000);
    return NAT_OK;
}
/* Sets the time slot to local milliseconds from the fields; returns the
 * millisecond of the day. */
NAT(cal_fieldsToLocalTime) {
    UNUSED;
    Obj *c = vm_obj(vm, a[0]);
    if (!c || c->len < 34) { ret[0] = 0; return NAT_OK; }
    int64_t y = (int32_t)c->d[C_YEAR];
    if (c->d[C_ERA + 1] && (int32_t)c->d[C_ERA] == 0) y = 1 - y;
    int64_t mon = (int32_t)c->d[C_MONTH];
    y += floordiv(mon, 12); mon -= floordiv(mon, 12) * 12;
    int64_t days;
    if (c->d[C_DOY + 1] > c->d[C_DATE + 1] && c->d[C_DOY + 1] > c->d[C_MONTH + 1])
        days = days_from_civil(y, 1, 1) + (int32_t)c->d[C_DOY] - 1;
    else
        days = days_from_civil(y, (int)mon + 1, 1) + (int32_t)c->d[C_DATE] - 1;
    int64_t hod;
    if (c->d[C_HOUR + 1] > c->d[C_HOD + 1]) hod = (int32_t)c->d[C_HOUR] + 12 * (int32_t)c->d[C_AMPM];
    else hod = (int32_t)c->d[C_HOD];
    int64_t msd = hod * 3600000 + (int64_t)(int32_t)c->d[C_MIN] * 60000 + (int64_t)(int32_t)c->d[C_SEC] * 1000 + (int32_t)c->d[C_MS];
    int64_t t64 = days * 86400000 + msd;
    c->d[C_TIME] = (uint32_t)t64; c->d[C_TIME + 1] = (uint32_t)((uint64_t)t64 >> 32);
    ret[0] = (uint32_t)(t64 - floordiv(t64, 86400000) * 86400000);
    return NAT_OK;
}
NAT(tz_getOffset) { UNUSED; ret[0] = 0; return NAT_OK; }   /* GMT, no daylight saving yet */

/* ------------------------------------------------------------------ */
/* Inflater: slot 2 keeps the window flag given to init */

static int inflate_to(BBVM *vm, Obj *src, uint32_t off, uint32_t len, int gzip, ref_t *out) {
    *out = 0;
    if (!src || off > src->len || len > src->len - off) return 0;
    const uint8_t *p = (const uint8_t *)src->d + off;
    if (gzip) {
        if (len < 18 || p[0] != 0x1f || p[1] != 0x8b) return 0;
        int flg = p[3]; uint32_t i = 10;
        if (flg & 4) i += 2 + (p[i] | p[i + 1] << 8);
        if (flg & 8) while (i < len && p[i++]) {}
        if (flg & 16) while (i < len && p[i++]) {}
        if (flg & 2) i += 2;
        if (i >= len) return 0;
        p += i; len -= i;
    }
    int header = !gzip && len >= 2 && (p[0] & 0x0f) == 8 && ((p[0] << 8) | p[1]) % 31 == 0;
    int n = 0;
    char *d = stbi_zlib_decode_malloc_guesssize_headerflag((const char *)p, (int)len, (int)len * 4 + 64, &n, header);
    if (!d) return 0;
    *out = vm_new_array(vm, BT_BYTE, NULL, 1, (uint32_t)n);
    Obj *o = vm_obj(vm, *out);
    memcpy(o->d, d, (size_t)n);
    free(d);
    return 1;
}
NAT(inf_init) { UNUSED; Obj *o = vm_obj(vm, a[0]); if (o && o->len > 2) o->d[2] = a[1]; return NAT_OK; }
NAT(inf_decompress) {
    UNUSED; ref_t r;
    inflate_to(vm, vm_obj(vm, a[1]), a[2], a[3], 0, &r);
    if (!r) vm_log(vm, "Inflater: bad data");
    ret[0] = r;
    return NAT_OK;
}
NAT(inf_gzip) { UNUSED; ref_t r; inflate_to(vm, vm_obj(vm, a[0]), a[1], a[2], 1, &r); ret[0] = r; return NAT_OK; }

/* ------------------------------------------------------------------ */

static uint64_t g_rng = 0x9e3779b97f4a7c15ull;
static uint32_t rnd32(void) {
    g_rng ^= g_rng << 13; g_rng ^= g_rng >> 7; g_rng ^= g_rng << 17;
    return (uint32_t)(g_rng >> 16);
}
NAT(rs_getInt) { UNUSED; ret[0] = rnd32(); return NAT_OK; }
NAT(rs_getLong) { UNUSED; nat_ret64(ret, (int64_t)((uint64_t)rnd32() << 32 | rnd32())); return NAT_OK; }
NAT(rs_getBytes) {
    UNUSED;
    Obj *o = vm_obj(vm, a[0]);
    if (!o) return NAT_OK;
    for (uint32_t i = 0; i < a[2] && a[1] + i < o->len; i++) ((uint8_t *)o->d)[a[1] + i] = (uint8_t)rnd32();
    return NAT_OK;
}
NAT(k_noop) { UNUSED; return NAT_OK; }
NAT(k_one) { UNUSED; ret[0] = 1; return NAT_OK; }
NAT(battery_status) { UNUSED; ret[0] = 0x0001; return NAT_OK; }   /* BSTAT_CHARGING off, level ok */
NAT(battery_level) { UNUSED; ret[0] = 100; return NAT_OK; }

NAT(mon_owned) {
    UNUSED;
    Monitor *m = a[0] ? vm_monitor(vm, a[0], 0) : NULL;
    ret[0] = m && m->owner == t && m->count > 0;
    return NAT_OK;
}

/* application control: every permission allowed */
NAT(perm_all) {
    UNUSED;
    ref_t r = vm_new_array(vm, BT_BYTE, NULL, 1, 8);
    memset(vm_obj(vm, r)->d, 0xff, 8);
    ret[0] = r;
    return NAT_OK;
}

/* NvStore: small non-volatile values, kept in memory */
typedef struct NvEnt { int id; int isData; int64_t v; uint8_t *data; uint32_t len; } NvEnt;
static NvEnt g_nv[256]; static int g_nnv;
static NvEnt *nv_find(int id, int create) {
    for (int i = 0; i < g_nnv; i++) if (g_nv[i].id == id) return &g_nv[i];
    if (!create || g_nnv == 256) return NULL;
    memset(&g_nv[g_nnv], 0, sizeof g_nv[0]);
    g_nv[g_nnv].id = id;
    return &g_nv[g_nnv++];
}
NAT(nv_readInt) { UNUSED; NvEnt *e = nv_find((int)a[0], 0); ret[0] = e && !e->isData ? (uint32_t)e->v : a[1]; return NAT_OK; }
NAT(nv_readLong) { UNUSED; NvEnt *e = nv_find((int)a[0], 0); nat_ret64(ret, e && !e->isData ? e->v : nat_arg64(a + 1)); return NAT_OK; }
NAT(nv_writeInt) { UNUSED; NvEnt *e = nv_find((int)a[0], 1); if (e) { e->isData = 0; e->v = (int32_t)a[1]; } ret[0] = e != NULL; return NAT_OK; }
NAT(nv_writeLong) { UNUSED; NvEnt *e = nv_find((int)a[0], 1); if (e) { e->isData = 0; e->v = nat_arg64(a + 1); } ret[0] = e != NULL; return NAT_OK; }
NAT(nv_readData) {
    UNUSED;
    NvEnt *e = nv_find((int)a[0], 0);
    ret[0] = 0;
    if (e && e->isData) {
        ref_t r = vm_new_array(vm, BT_BYTE, NULL, 1, e->len);
        memcpy(vm_obj(vm, r)->d, e->data, e->len);
        ret[0] = r;
    }
    return NAT_OK;
}
NAT(nv_writeData) {   /* (id, byte[], off, len) */
    UNUSED;
    Obj *b = vm_obj(vm, a[1]);
    NvEnt *e = nv_find((int)a[0], 1);
    ret[0] = 0;
    if (!e || !b || a[2] > b->len || a[3] > b->len - a[2]) return NAT_OK;
    free(e->data);
    e->isData = 1; e->len = a[3]; e->data = malloc(a[3] + 1);
    memcpy(e->data, (uint8_t *)b->d + a[2], a[3]);
    ret[0] = 1;
    return NAT_OK;
}
NAT(nv_delete) { UNUSED; NvEnt *e = nv_find((int)a[0], 0); if (e) { free(e->data); *e = g_nv[--g_nnv]; } ret[0] = 1; return NAT_OK; }

const NatEnt kNativesMisc[] = {
    { "net.rim.device.internal.applicationcontrol.SystemPermissions.getConsumerDefaults", perm_all },
    { "net.rim.device.internal.applicationcontrol.SystemPermissions.getCorporateDefaults", perm_all },
    { "net.rim.device.internal.applicationcontrol.SystemPermissions.getDefaultPermissionsImpl", perm_all },
    { "net.rim.device.internal.applicationcontrol.SystemPermissions.getMaxPermissiblePermissions", perm_all },
    { "net.rim.device.internal.system.NvStore.readInt", nv_readInt },
    { "net.rim.device.internal.system.NvStore.readLong", nv_readLong },
    { "net.rim.device.internal.system.NvStore.writeInt", nv_writeInt },
    { "net.rim.device.internal.system.NvStore.writeLong", nv_writeLong },
    { "net.rim.device.internal.system.NvStore.readDataInternal", nv_readData },
    { "net.rim.device.internal.system.NvStore.writeDataInternal", nv_writeData },
    { "net.rim.device.internal.system.NvStore.deleteData", nv_delete },
    { "net.rim.vm.Monitor.monitorOwned", mon_owned },
    { "net.rim.device.cldc.util.GregorianCalendar.timeToDateFields", cal_timeToDateFields },
    { "net.rim.device.cldc.util.GregorianCalendar.timeToRemainingFields", cal_timeToRemainingFields },
    { "net.rim.device.cldc.util.GregorianCalendar.fieldsToLocalTime", cal_fieldsToLocalTime },
    { "net.rim.device.cldc.util.TimeZoneImpl.getOffset", tz_getOffset },
    { "net.rim.device.internal.compress.Inflater.init", inf_init },
    { "net.rim.device.internal.compress.Inflater.decompress", inf_decompress },
    { "net.rim.device.internal.compress.Inflater.gzipDecompress", inf_gzip },
    { "net.rim.device.api.crypto.RandomSource.getInt", rs_getInt },
    { "net.rim.device.api.crypto.RandomSource.getLong", rs_getLong },
    { "net.rim.device.api.crypto.RandomSource.getBytes", rs_getBytes },
    { "net.rim.device.api.crypto.RandomSource.add", k_noop },
    { "java.lang.ref.Reference.register", k_noop },
    { "net.rim.vm.WeakReference.register", k_noop },
    { "net.rim.vm.SoftReference.register", k_noop },
    { "net.rim.device.api.system.DeviceInfo.getBatteryLevel", battery_level },
    { NULL, NULL }
};
