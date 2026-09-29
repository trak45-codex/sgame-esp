//
// ESP v48 — hero scan via ActorManager.updatableActorList (IL2CPP), with HP.
// Streams binary EspActor[] packets to overlay via TCP 127.0.0.1:47291.
//
// Protocol (host-endian little; sgame is aarch64):
//   header  : magic[4]="ESP2"  count(u32)
//   actors  : EspActor[count]  (56 bytes each, packed)
//
// EspActor v2 (56 bytes — grew from v1's 48 by adding hp/maxHp):
//   key(u32)  type(i32)  configId(i32)  camp(i32)
//   battleOrder(i32) objId(u32) x(f32) y(f32) z(f32)
//   fwdX(i32) fwdY(i32) fwdZ(i32)
//   hp(i32)   maxHp(i32)
//
// type=0 (ActorTypeDef.HERO) is the only value we emit. Overlay filters on
// (type==0 && camp==2) to draw enemy markers.
//
// ---------------------------------------------------------------------------
// DATA PATH (IL2CPP side — verified against dump.cs ground truth)
// ---------------------------------------------------------------------------
//   Assets.Scripts.GameLogic.ActorManager  (s_instance)
//     +0x?? updatableActorList : List<Actor>      <- v33 verified live, FOW-safe
//
//   Actor:
//     +0x10  klass ptr (IL2CPP object header; fields start at +0x10)
//     +0x008 playerCamp        (i32: 1 or 2)
//     +0x180 captainConfigID   (u32 hero id)
//     +0x268 posAnchor         (ptr) -> +0x10 -> +0x00 -> +0x60 = 16B raw (x,z)
//     +0x188 hpAnchor          (ptr) -> +0xA8 = (cur_hp, max_hp)  [i32 pair]
//
// NOTE on the two anchors — they are NOT the same chain:
//   * actor + 0x188 -> + 0xA8 is the HP pair (baba's RE doc called this "pos",
//     which is WRONG; the research journal (2026-05-28, 06-02) proved it is HP).
//   * actor + 0x268 -> +0x10 -> +0x00 -> +0x60 is the live world position.
// Both anchors live on the *native* Actor object, not on ActorLinker.
// ---------------------------------------------------------------------------

#include "esp.h"
#include "log.h"
#include "il2cpp-tabledefs.h"
#include "il2cpp-class.h"
#include <thread>
#include <atomic>
#include <vector>
#include <unistd.h>
#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <errno.h>
#include <dlfcn.h>
#include <chrono>

#define DO_API(r, n, p) extern r (*n) p
#include "il2cpp-api-functions.h"
#undef DO_API

// ===== IL2CPP layout offsets =====
// --- ActorManager / DictionaryView<UInt32, ActorConfig> path (v33, verified) ---
#define AM_UPDATABLE_LIST   0x10   // ActorManager.updatableActorList
#define DICT_ENTRIES        0x10   // mscorlib Dictionary._entries (Entry[])
#define DICT_COUNT          0x18   // mscorlib Dictionary._count (int)
#define ENTRIES_BASE        0x18   // Entry[] managed-array inline data start
#define ENTRY_STRIDE        24     // hash(0) next(4) key(8) pad(12) value(16)
#define ENTRY_HASH          0
#define ENTRY_NEXT          4
#define ENTRY_KEY           8
#define ENTRY_VALUE         16

// --- ActorConfig / ActorConfigInner / ActorLinker ---
#define AC_TYPE             0x18   // ActorConfig.ActorType (0 = HERO)
#define AC_INNER            0x50   // ActorConfig.inner
#define INNER_LINKER        0x08   // ActorConfigInner.actorLinker
#define AL_OBJID            0x4AC  // ActorLinker.ObjID (u32)
#define AL_FORWARD          0x4B8  // ActorLinker.forward (VInt3 = 3*i32, *1000)
#define AL_POSITION         0x4C4  // ActorLinker.position (Vector3 = 3*f32, world)
#define AL_CAMP             0x56C  // ActorLinker camp (1=blue, 2=red)
#define AL_CONFIGID         0x038  // ActorLinker hero config ID

// --- HP chain (device-verified 2026-06-02, sibling truevision research) ---
//   *(ActorConfig + 0x188) + 0xA8 = (cur_hp, max_hp) as i32 pair.
// NOTE: baba's public RE doc mislabels +0x188/+0xA8 as "position"; it is HP.
#define ACTOR_HP_ANCHOR     0x188
#define HP_DATA_OFF         0xA8

// --- native (non-IL2CPP) position chain, kept for the KPM port ---
#define ACTOR_POS_ANCHOR    0x268
#define POS_DATA_OFF        0x60

// Generic IL2CPP object header: 2 pointers (klass, monitor) = 0x10 on 64-bit.
#define OBJ_HEADER          0x10

#define SOCK_PORT 47291

// Plausibility guards.
#define HP_MAX_PLAUSIBLE    100000

static inline bool is_plausible_ptr(const void *p) {
    uintptr_t v = (uintptr_t)p;
    return v >= 0x10000 && (v & 7) == 0;
}

#pragma pack(push, 1)
struct EspActor {
    uint32_t key;
    int32_t  type;
    int32_t  configId;
    int32_t  camp;
    int32_t  battleOrder;
    uint32_t objId;
    float    x, y, z;
    int32_t  fwd_x, fwd_y, fwd_z;
    int32_t  hp;
    int32_t  maxHp;
};
struct EspHeader {
    char     magic[4];   // "ESP2"
    uint32_t count;
};
#pragma pack(pop)
static_assert(sizeof(EspActor) == 56, "EspActor v2 must be 56 bytes");

static std::atomic<int> g_client_fd{-1};
static std::atomic<bool> g_stop{false};

static Il2CppClass *find_class_anywhere(const char *ns, const char *name) {
    Il2CppDomain *domain = il2cpp_domain_get();
    if (!domain) return nullptr;
    size_t n = 0;
    const Il2CppAssembly **assemblies = il2cpp_domain_get_assemblies(domain, &n);
    for (size_t i = 0; i < n; ++i) {
        const Il2CppImage *img = il2cpp_assembly_get_image(assemblies[i]);
        if (!img) continue;
        Il2CppClass *k = il2cpp_class_from_name(img, ns, name);
        if (k) return k;
    }
    return nullptr;
}

// Same, but also reports which image it came from (useful for "which assembly
// declares X" diagnostics without a full dump).
static Il2CppClass *find_class_log(const char *ns, const char *name, const char **image_out) {
    Il2CppDomain *domain = il2cpp_domain_get();
    if (!domain) return nullptr;
    size_t n = 0;
    const Il2CppAssembly **assemblies = il2cpp_domain_get_assemblies(domain, &n);
    for (size_t i = 0; i < n; ++i) {
        const Il2CppImage *img = il2cpp_assembly_get_image(assemblies[i]);
        if (!img) continue;
        Il2CppClass *k = il2cpp_class_from_name(img, ns, name);
        if (k) {
            if (image_out) *image_out = il2cpp_image_get_name((Il2CppImage *) img);
            return k;
        }
    }
    return nullptr;
}

// ---------------------------------------------------------------------------
// List<T> / array walking through the IL2CPP API (no raw offsets where possible)
// ---------------------------------------------------------------------------

// Read a managed array's element count.
// IL2CPP managed object header = 0x10 (klass* + monitor*), then for an array:
//   +0x10  bounds (Il2CppArrayBounds*)  -> only for multi-dim; 1-D uses +0x18
//   +0x18  max_length (uintptr_t)       <- element count for a 1-D array
//   +0x20  first element
static uint32_t array_len(void *arr) {
    if (!is_plausible_ptr(arr)) return 0;
    return *(uint32_t *) ((uintptr_t) arr + 0x18);
}

// Read a managed array's element i (pointer-sized elements only).
// Elements start at +0x20, not +0x18.
static void *array_at(void *arr, uint32_t i) {
    if (!is_plausible_ptr(arr)) return nullptr;
    return *(void **) ((uintptr_t) arr + 0x20 + i * sizeof(void *));
}

// Read a field by reflected offset from an object. `obj` is the managed object
// base (i.e. the pointer value stored in the container), field offsets in
// dump.cs are already relative to that base (header included on IL2CPP).
static inline void *fptr(void *obj, uint32_t off) {
    if (!is_plausible_ptr(obj)) return nullptr;
    return *(void **) ((uintptr_t) obj + off);
}
static inline int32_t fi32(void *obj, uint32_t off) {
    if (!is_plausible_ptr(obj)) return -1;
    return *(int32_t *) ((uintptr_t) obj + off);
}

// ---------------------------------------------------------------------------
// HP read: actor + 0x188 -> +0xA8 = (cur_hp, max_hp). Returns -1 on failure.
// ---------------------------------------------------------------------------
static int32_t read_hp(void *actor, int32_t *max_hp_out) {
    if (max_hp_out) *max_hp_out = 0;
    void *anchor = fptr(actor, ACTOR_HP_ANCHOR);
    if (!is_plausible_ptr(anchor)) return -1;
    int32_t buf[4] = {0};
    memcpy(buf, (void *) ((uintptr_t) anchor + HP_DATA_OFF), sizeof(buf));
    int32_t cur = buf[0];
    if (cur < 0 || cur > HP_MAX_PLAUSIBLE) return -1;
    if (max_hp_out) {
        int32_t mx = buf[1];
        *max_hp_out = (mx > 0 && mx <= HP_MAX_PLAUSIBLE) ? mx : 0;
    }
    return cur;
}

// ---------------------------------------------------------------------------
// Position read — TWO independent paths are provided:
//
//  (A) ActorLinker.position @ +0x4C4  (Vector3 = 3*f32, world units)
//      This is what the upstream zygisk-esp v47 build reads, and it is proven
//      real-time for actors inside view.  Subject to FOW: when an enemy is not
//      visible, the value freezes at the last-seen position.
//
//  (B) actor + 0x268 -> +0x10 -> +0x00 -> +0x60  (2*int32 fixed-point *100)
//      The native (KPM/truevision) chain.  Requires an `actor` base pointer,
//      which the managed DictionaryView path does not hand us; kept here for
//      completeness / the KPM port.
// ---------------------------------------------------------------------------
static bool read_pos_linker(void *al, float *x_out, float *y_out, float *z_out,
                            int32_t *fwd_x, int32_t *fwd_y, int32_t *fwd_z) {
    if (!is_plausible_ptr(al)) return false;

    // ActorLinker.forward @ +0x4B8 = VInt3 (3 * i32, scaled *1000)
    int32_t fwd[3] = {0};
    memcpy(fwd, (void *) ((uintptr_t) al + AL_FORWARD), sizeof(fwd));
    if (fwd_x) *fwd_x = fwd[0];
    if (fwd_y) *fwd_y = fwd[1];
    if (fwd_z) *fwd_z = fwd[2];

    // ActorLinker.position @ +0x4C4 = Vector3 (3 * f32, world units)
    float p[3] = {0};
    memcpy(p, (void *) ((uintptr_t) al + AL_POSITION), sizeof(p));
    // Reject NaN / out-of-map garbage so FOW-frozen or uninitialised entries
    // don't get drawn.  sgame's 5v5 map is ±200 world units around origin.
    if (p[0] != p[0] || p[1] != p[1] || p[2] != p[2]) return false;   // NaN
    if (p[0] < -400.f || p[0] > 400.f) return false;
    if (p[2] < -400.f || p[2] > 400.f) return false;
    if (p[1] < -200.f || p[1] > 400.f) return false;

    *x_out = p[0];
    *y_out = p[1];
    *z_out = p[2];
    return true;
}

static bool read_pos_native(void *actor, float *x_out, float *z_out) {
    void *p = fptr(actor, ACTOR_POS_ANCHOR);
    if (!is_plausible_ptr(p)) return false;
    p = fptr(p, 0x10);
    if (!is_plausible_ptr(p)) return false;
    p = fptr(p, 0x00);
    if (!is_plausible_ptr(p)) return false;
    p = fptr(p, POS_DATA_OFF);
    if (!is_plausible_ptr(p)) return false;

    int32_t raw[4] = {0};
    memcpy(raw, p, sizeof(raw));
    if (raw[0] < -20000 || raw[0] > 20000) return false;
    if (raw[2] < -20000 || raw[2] > 20000) return false;
    *x_out = raw[0] / 100.0f;
    *z_out = raw[2] / 100.0f;
    return true;
}

// ---------------------------------------------------------------------------
// Actor enumeration — ActorManager.updatableActorList, a
// DictionaryView<UInt32, ActorConfig>.  This is the path the upstream project
// proved live (v33) after GamePlayerCenter.playersCache turned out to be empty
// in this sgame build.
//
// ActorManager.s_instance (static, possibly on a parent class)
//   +0x10  updatableActorList  (DictionaryView<UInt32, ActorConfig>)
//            .Context         (reflected at runtime; mscorlib Dictionary)
//              +0x10 _entries (Entry[])
//              +0x18 _count   (int)
//              Entry stride 24: hash(i32@0) next(i32@4) key(u32@8) pad value(ptr@16)
//                value = ActorConfig*
//                  +0x18 ActorType   (0 = ActorTypeDef.HERO)
//                  +0x50 inner       (ActorConfigInner*)
//                      +0x08 actorLinker (ActorLinker*)
// ---------------------------------------------------------------------------
struct ActorEnumCache {
    Il2CppClass     *am_klass   = nullptr;
    FieldInfo *am_sinst   = nullptr;
    int              dv_ctx_off = -1;
    bool             logged     = false;
};
static ActorEnumCache g_enum;

static bool actor_enum_init() {
    const char *img_name = nullptr;
    Il2CppClass *am = find_class_log("Assets.Scripts.GameLogic", "ActorManager", &img_name);
    if (!am) {
        LOGW("[esp] ActorManager not found");
        return false;
    }
    LOGI("[esp] ActorManager klass=%p image=%s", am, img_name ? img_name : "?");

    FieldInfo *si = il2cpp_class_get_field_from_name(am, "s_instance");
    if (!si) {
        Il2CppClass *parent = il2cpp_class_get_parent(am);
        while (!si && parent) {
            si = il2cpp_class_get_field_from_name(parent, "s_instance");
            parent = il2cpp_class_get_parent(parent);
        }
    }
    if (!si) {
        LOGE("[esp] ActorManager.s_instance not found");
        return false;
    }

    g_enum.am_klass = am;
    g_enum.am_sinst = si;
    LOGI("[esp] ActorManager.s_instance @ +0x%zx (static)",
         (size_t) il2cpp_field_get_offset(si));
    return true;
}

static int scan_heroes(std::vector<EspActor> &out) {
    if (!g_enum.am_klass && !actor_enum_init()) return 0;

    void *am = nullptr;
    il2cpp_field_static_get_value(g_enum.am_sinst, &am);
    if (!is_plausible_ptr(am)) {
        static int n = 0;
        if (n++ % 120 == 0) LOGW("[esp] ActorManager.s_instance null");
        return 0;
    }

    // updatableActorList sits at +0x10 on ActorManager in this build.
    void *dv = fptr(am, 0x10);
    if (!is_plausible_ptr(dv)) {
        static int n = 0;
        if (n++ % 120 == 0) LOGW("[esp] updatableActorList null");
        return 0;
    }

    // DictionaryView<,>.Context — reflect its real offset once, to absorb
    // IL2CPP-version drift instead of hard-coding 0x18.
    if (g_enum.dv_ctx_off < 0) {
        Il2CppClass *dv_klass = il2cpp_object_get_class((Il2CppObject *) dv);
        FieldInfo *fctx =
            dv_klass ? il2cpp_class_get_field_from_name(dv_klass, "Context") : nullptr;
        int raw = fctx ? (int) il2cpp_field_get_offset(fctx) : 0x8;
        // Field offsets from IL2CPP already include the object header; upstream
        // observed raw==0x8 needing +0x10 for the managed pointer to land.
        g_enum.dv_ctx_off = (raw >= 0x10) ? raw : (raw + 0x10);
        LOGI("[esp] DictionaryView.Context @ +0x%x", (unsigned) g_enum.dv_ctx_off);
    }

    void *dict = fptr(dv, g_enum.dv_ctx_off);
    if (!is_plausible_ptr(dict)) {
        static int n = 0;
        if (n++ % 120 == 0) LOGW("[esp] DictionaryView.Context null");
        return 0;
    }

    int32_t count = fi32(dict, DICT_COUNT);
    void   *ents  = fptr(dict, DICT_ENTRIES);
    if (!is_plausible_ptr(ents) || count <= 0 || count > 256) {
        static int n = 0;
        if (n++ % 120 == 0) LOGW("[esp] dict count=%d ents=%p", count, ents);
        return 0;
    }

    int hero_emitted = 0;
    for (int i = 0; i < count + 8 && i < 256; ++i) {
        char *e = (char *) ents + ENTRIES_BASE + i * ENTRY_STRIDE;
        int32_t hash = *(int32_t *) (e + ENTRY_HASH);
        int32_t next = *(int32_t *) (e + ENTRY_NEXT);
        if (hash < 0 && next < 0) continue;              // free slot
        void *ac = *(void **) (e + ENTRY_VALUE);
        if (!is_plausible_ptr(ac)) continue;

        int32_t atype = *(int32_t *) ((uintptr_t) ac + AC_TYPE);
        if (atype != 0) continue;                        // HERO only

        void *inner = fptr(ac, AC_INNER);
        if (!is_plausible_ptr(inner)) continue;
        void *al = fptr(inner, INNER_LINKER);
        if (!is_plausible_ptr(al)) continue;

        EspActor a{};
        a.key       = *(uint32_t *) (e + ENTRY_KEY);
        a.type      = 0;
        a.objId     = *(uint32_t *) ((uintptr_t) al + AL_OBJID);
        // Live values were found on ActorLinker (ActorConfig's CmpType/ConfigID
        // are placeholder zeros in this build).
        a.camp      = *(int32_t  *) ((uintptr_t) al + AL_CAMP);      // +0x56c
        a.configId  = *(int32_t  *) ((uintptr_t) al + AL_CONFIGID);  // +0x38

        // Position: prefer ActorLinker.position.
        float x = 0, y = 0, z = 0;
        int32_t fx = 0, fy = 0, fz = 0;
        if (!read_pos_linker(al, &x, &y, &z, &fx, &fy, &fz)) continue;

        a.x = x; a.y = y; a.z = z;
        a.fwd_x = fx; a.fwd_y = fy; a.fwd_z = fz;

        // HP: ActorLinker has no HP; read via the ActorConfig chain.
        // In this build HP lives at *(ActorConfig + 0x188) + 0xA8 (i32 pair),
        // per the 2026-06-02 device-verified RE in the sibling truevision work.
        int32_t max_hp = 0;
        int32_t hp = read_hp(ac, &max_hp);
        a.hp    = hp;
        a.maxHp = max_hp;

        a.battleOrder = i;
        out.push_back(a);

        if (hero_emitted < 8) {
            LOGI("[esp] hero[%d] key=%u cfg=%d camp=%d obj=%u pos=(%.1f,%.1f,%.1f) hp=%d/%d",
                 hero_emitted, a.key, a.configId, a.camp, a.objId, a.x, a.y, a.z,
                 a.hp, a.maxHp);
        }
        hero_emitted++;
    }

    static int tick = 0;
    if (tick++ % 60 == 0) {
        LOGI("[esp] scan: dict_count=%d heroes=%d", count, hero_emitted);
    }
    return hero_emitted;
}

// ---------------------------------------------------------------------------
// TCP server: accept one client at a time, push snapshots.
// ---------------------------------------------------------------------------
static bool send_snapshot(int fd, const std::vector<EspActor> &actors) {
    EspHeader hdr{};
    memcpy(hdr.magic, "ESP2", 4);
    hdr.count = (uint32_t) actors.size();

    if (send(fd, &hdr, sizeof(hdr), MSG_NOSIGNAL) != (ssize_t) sizeof(hdr)) return false;
    if (!actors.empty()) {
        ssize_t want = (ssize_t) (actors.size() * sizeof(EspActor));
        if (send(fd, actors.data(), want, MSG_NOSIGNAL) != want) return false;
    }
    return true;
}

static void server_thread() {
    int srv = socket(AF_INET, SOCK_STREAM, 0);
    if (srv < 0) { LOGE("[esp] socket() failed: %s", strerror(errno)); return; }

    int one = 1;
    setsockopt(srv, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = htons(SOCK_PORT);

    if (bind(srv, (sockaddr *) &addr, sizeof(addr)) < 0) {
        LOGE("[esp] bind(%d) failed: %s", SOCK_PORT, strerror(errno));
        close(srv);
        return;
    }
    if (listen(srv, 4) < 0) {
        LOGE("[esp] listen failed: %s", strerror(errno));
        close(srv);
        return;
    }
    LOGI("[esp] listening on 127.0.0.1:%d", SOCK_PORT);

    while (!g_stop.load()) {
        sockaddr_in cli{};
        socklen_t cl = sizeof(cli);
        int fd = accept(srv, (sockaddr *) &cli, &cl);
        if (fd < 0) {
            if (errno == EINTR) continue;
            usleep(200000);
            continue;
        }
        LOGI("[esp] client connected");
        g_client_fd.store(fd);

        std::vector<EspActor> actors;
        while (!g_stop.load()) {
            actors.clear();
            scan_heroes(actors);
            if (!send_snapshot(fd, actors)) {
                LOGW("[esp] send failed, dropping client");
                break;
            }
            usleep(50000);   // 20 Hz
        }

        g_client_fd.store(-1);
        close(fd);
        LOGI("[esp] client gone");
    }
    close(srv);
}

void esp_start(const char *game_data_dir) {
    (void) game_data_dir;
    LOGI("[esp] esp_start — probing IL2CPP runtime");

    // Wait for ActorManager to become available (game may still be loading).
    bool ok = false;
    for (int i = 0; i < 120 && !g_stop.load(); i++) {
        if (actor_enum_init()) { ok = true; break; }
        usleep(500000);
    }
    if (!ok) {
        LOGE("[esp] ActorManager never appeared — aborting");
        return;
    }

    std::thread t(server_thread);
    t.detach();
    LOGI("[esp] server thread started");
}
