#!/bin/sh
# A thread's thread-local value must survive while another thread is setting up
# its own thread-local storage. With a loader audit module present (the launcher
# sets LD_AUDIT to libhideexports.so), glibc gives the libraries linked at start
# malloc'd per-thread TLS blocks instead of static TLS. If a thread whose TLS
# generation is behind the global one reads such a variable while any other
# thread is inside a TLS allocation, glibc's x86-64 __tls_get_addr slow path
# allocates a fresh block for the module and the value just written is lost.
# Qt 5's per-thread QThreadData pointer is such a variable: losing it makes a
# new QThread adopt itself over and over and end in "QThread: Destroyed while
# thread is still running".
#
# The fixture makes the window deterministic: a holder thread is paused inside
# the malloc of its first access to a dlopen'd library's TLS, and a victim
# thread, created after a TLS library was unloaded (so its generation is
# behind), writes and reads a variable in a DT_NEEDED library.
#
# The shim's __tls_get_addr (shim/tlsgetaddr.cpp) closes the gap. Cases:
#   - no audit module: the value is kept;
#   - audit module, no fix (the control): the value is lost, which shows the
#     fixture reaches the window; a glibc that keeps it prints a skip line;
#   - audit module with a preload built from shim/tlsgetaddr.cpp alone: kept;
#   - the built libmenufix.so exports __tls_get_addr@@GLIBC_2.3, and in a
#     loader trace of the installed Live (Live does not run) libQt5Core.so.5's
#     __tls_get_addr binds to it.
# TLS_RACE_LOOPS=<n> repeats the control and the fixed case n times and counts.
# TLS_RACE_SOAK=<seconds> adds a free-running run: 8 threads keep creating
# threads that store and read back a value in a start-up library, while another
# thread dlopens, touches and dlcloses four libraries with TLS.
# Usage: tests/tls-audit-race.sh [shim folder] [install folder]
#   shim folder: tlsgetaddr.cpp, and libhideexports.so and libmenufix.so when
#   built (default: this repo's shim/, the audit module built from its source)
#   install folder: default ~/.local/share/ableton-live-12
set -eu
HERE=$(cd "$(dirname "$0")" && pwd)
SHIM=$(cd "${1:-$HERE/../shim}" && pwd)
PREFIX=${2:-$HOME/.local/share/ableton-live-12}
LOOPS=${TLS_RACE_LOOPS:-1}
SOAK=${TLS_RACE_SOAK:-0}
T=$(mktemp -d); trap 'rm -rf "$T"' EXIT
if [ -n "${1:-}" ] && [ -s "$SHIM/libhideexports.so" ]; then
    MOD=$SHIM/libhideexports.so
else
    MOD=$T/libhideexports.so
    gcc -O2 -fPIC -shared -nostdlib -fno-stack-protector -fno-builtin -fvisibility=hidden -Wl,--no-undefined \
        "$SHIM/hideexports.c" -o "$MOD"
fi
printf 'GLIBC_2.3 { global: __tls_get_addr; local: *; };\n' > "$T/fix.map"
g++ -O2 -fPIC -shared -std=c++17 "$SHIM/tlsgetaddr.cpp" -o "$T/libfix.so" -Wl,--version-script="$T/fix.map" -ldl

cat > "$T/tlsq.c" <<'EOF'
/* Stands in for libQt5Core's per-thread data pointer. */
static __thread void *cur;
__attribute__((noinline)) void tq_set(void *p) { cur = p; }
__attribute__((noinline)) void *tq_get(void) { return cur; }
EOF
cat > "$T/plug.c" <<'EOF'
/* Stands in for a plug-in or driver library with its own TLS. */
__thread char buf[256];
void touch(void) { buf[0]++; }
EOF
cat > "$T/main.c" <<'EOF'
#define _GNU_SOURCE
#include <dlfcn.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
void tq_set(void *); void *tq_get(void);
extern void *__libc_malloc(size_t);

/* The holder pauses in the first malloc it makes once armed: its first access
   to the dlopen'd library's TLS, made from inside glibc's TLS allocation. */
static pthread_t holder_id;
static atomic_int armed, paused, release_holder;
void *malloc(size_t n)
{
    if (atomic_load(&armed) && pthread_equal(pthread_self(), holder_id)) {
        atomic_store(&armed, 0);
        atomic_store(&paused, 1);
        while (!atomic_load(&release_holder)) usleep(1000);
    }
    return __libc_malloc(n);
}

static void (*touch_hold)(void);
static atomic_int go_holder, go_victim;
static void *holder(void *a)
{
    (void)a;
    while (!atomic_load(&go_holder)) usleep(1000);
    holder_id = pthread_self();
    atomic_store(&armed, 1);
    touch_hold();
    return 0;
}
static void *victim(void *a)
{
    (void)a;
    while (!atomic_load(&go_victim)) usleep(1000);
    static int mark;
    tq_set(&mark);
    return tq_get() == &mark ? (void *)1 : (void *)0;
}
int main(int argc, char **argv)
{
    char p[4096];
    snprintf(p, sizeof p, "%s/libhold.so", argv[1]);
    void *h = dlopen(p, RTLD_NOW);
    snprintf(p, sizeof p, "%s/libgone.so", argv[1]);
    void *g = dlopen(p, RTLD_NOW);
    if (!h || !g) { fprintf(stderr, "%s\n", dlerror()); return 2; }
    touch_hold = (void (*)(void))dlsym(h, "touch");
    dlclose(g); /* the newest TLS generation is now an empty slot */
    (void)argc;
    pthread_t th, tv;
    pthread_create(&th, 0, holder, 0);
    pthread_create(&tv, 0, victim, 0);
    atomic_store(&go_holder, 1);
    for (int i = 0; i < 5000 && !atomic_load(&paused); i++) usleep(1000);
    if (!atomic_load(&paused)) { puts("holder-not-paused"); return 2; }
    atomic_store(&go_victim, 1);
    void *kept;
    pthread_join(tv, &kept);
    atomic_store(&release_holder, 1);
    pthread_join(th, 0);
    puts(kept ? "kept" : "lost");
    return 0;
}
EOF
gcc -O2 -fPIC -shared "$T/tlsq.c" -o "$T/libtlsq.so"
gcc -O2 -fPIC -shared "$T/plug.c" -o "$T/libhold.so"
gcc -O2 -fPIC -shared "$T/plug.c" -o "$T/libgone.so"
gcc -O2 "$T/main.c" -o "$T/race" -L"$T" -ltlsq -Wl,-rpath,"$T" -pthread -ldl

fail=0
check() { # <label> <actual>
    if [ "$2" = kept ]; then echo "  ok   $1: value kept"
    else echo "FAIL: $1: $2"; fail=1; fi
}
runs() { # <count> <env...>: prints "<kept> <lost> <other>"
    n=$1; shift; k=0; l=0; o=0; i=0
    while [ $i -lt "$n" ]; do
        case $(env "$@" "$T/race" "$T") in kept) k=$((k + 1)) ;; lost) l=$((l + 1)) ;; *) o=$((o + 1)) ;; esac
        i=$((i + 1))
    done
    echo "$k $l $o"
}
check "without audit module" "$("$T/race" "$T")"

# shellcheck disable=SC2046
set -- $(runs "$LOOPS" LD_AUDIT="$MOD")
if [ "$1" = 0 ] && [ "$3" = 0 ]; then
    echo "  ok   control, LD_AUDIT=libhideexports.so without the fix: value lost in $2 of $LOOPS"
elif [ "$2" = 0 ] && [ "$3" = 0 ]; then
    echo "skip: control: this glibc keeps the value without the fix ($1 of $LOOPS); the fixed case below proves nothing"
else
    echo "FAIL: control: kept $1, lost $2, other $3 of $LOOPS (expected every run lost)"; fail=1
fi

# shellcheck disable=SC2046
set -- $(runs "$LOOPS" LD_AUDIT="$MOD" LD_PRELOAD="$T/libfix.so")
if [ "$1" = "$LOOPS" ]; then echo "  ok   LD_AUDIT=libhideexports.so with the shim's __tls_get_addr: value kept in $1 of $LOOPS"
else echo "FAIL: with the shim's __tls_get_addr: kept $1, lost $2, other $3 of $LOOPS"; fail=1; fi

if [ "$SOAK" -gt 0 ]; then
    cat > "$T/soak.c" <<'EOF'
#define _GNU_SOURCE
#include <dlfcn.h>
#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
void tq_set(void *); void *tq_get(void);
static atomic_int stop;
static atomic_long tries, lost;
static const char *dir;
static void *child(void *a)
{
    int mark;
    tq_set(&mark);
    sched_yield();
    atomic_fetch_add(&tries, 1);
    if (tq_get() != &mark) atomic_fetch_add(&lost, 1);
    return a;
}
static void *spawner(void *a)
{
    while (!atomic_load(&stop)) {
        pthread_t t;
        if (pthread_create(&t, 0, child, 0) == 0) pthread_join(t, 0);
    }
    return a;
}
static void *churn(void *a)
{
    char p[4096];
    while (!atomic_load(&stop))
        for (int i = 0; i < 4; i++) {
            snprintf(p, sizeof p, "%s/libsoak%d.so", dir, i);
            void *h = dlopen(p, RTLD_NOW);
            if (!h) { fprintf(stderr, "%s\n", dlerror()); exit(2); }
            ((void (*)(void))dlsym(h, "touch"))();
            dlclose(h);
        }
    return a;
}
int main(int argc, char **argv)
{
    dir = argv[1];
    struct timespec ts = {argc > 2 ? atoi(argv[2]) : 1, 0};
    pthread_t s[8], c;
    pthread_create(&c, 0, churn, 0);
    for (int i = 0; i < 8; i++) pthread_create(&s[i], 0, spawner, 0);
    nanosleep(&ts, 0);
    atomic_store(&stop, 1);
    for (int i = 0; i < 8; i++) pthread_join(s[i], 0);
    pthread_join(c, 0);
    printf("%ld %ld\n", atomic_load(&lost), atomic_load(&tries));
    return 0;
}
EOF
    for i in 0 1 2 3; do gcc -O2 -fPIC -shared "$T/plug.c" -o "$T/libsoak$i.so"; done
    gcc -O2 "$T/soak.c" -o "$T/soak" -L"$T" -ltlsq -Wl,-rpath,"$T" -pthread -ldl
    # shellcheck disable=SC2046
    set -- $(LD_AUDIT="$MOD" LD_PRELOAD="$T/libfix.so" "$T/soak" "$T" "$SOAK")
    if [ "${1:-x}" = 0 ] && [ "${2:-0}" -gt 0 ]; then echo "  ok   soak $SOAK s with the fix: 0 lost in $2 threads"
    else echo "FAIL: soak $SOAK s with the fix: lost ${1:-?} in ${2:-?} threads"; fail=1; fi
    # shellcheck disable=SC2046
    set -- $(LD_AUDIT="$MOD" "$T/soak" "$T" "$SOAK")
    echo "  info soak $SOAK s without the fix: lost ${1:-?} in ${2:-?} threads"
fi

# Without the audit module the start-up libraries have static TLS; the shim is
# still preloaded then (LIVE_HIDE_EXPORTS=0), and must give the same answer.
check "without audit module, with the shim's __tls_get_addr" \
    "$(env -u LD_AUDIT LD_PRELOAD="$T/libfix.so" "$T/race" "$T")"

# A module ID freed by dlclose and reused by another library: a thread alive
# throughout must see the new library's block, never the old one's.
# Library A starts its variable at 7, library B at 9.
printf '__thread int tv = 7;\nint get(void) { return tv; }\nvoid set(int v) { tv = v; }\n' > "$T/liba.c"
printf '__thread int tv = 9;\nint get(void) { return tv; }\nvoid set(int v) { tv = v; }\n' > "$T/libb.c"
gcc -O2 -fPIC -shared -mtls-dialect=gnu "$T/liba.c" -o "$T/libreusea.so"
gcc -O2 -fPIC -shared -mtls-dialect=gnu "$T/libb.c" -o "$T/libreuseb.so"
cat > "$T/reuse.c" <<'EOF'
#define _GNU_SOURCE
#include <dlfcn.h>
#include <link.h>
#include <pthread.h>
#include <semaphore.h>
#include <stdio.h>
static sem_t go, done;
static int (*get)(void);
static void (*set)(int);
static int seen;
static void *worker(void *a)
{
    sem_wait(&go); set(42); sem_post(&done);    /* uses A's block */
    sem_wait(&go); seen = get(); sem_post(&done); /* must use B's */
    return a;
}
static size_t modid(void *h) { size_t m = 0; dlinfo(h, RTLD_DI_TLS_MODID, &m); return m; }
int main(int argc, char **argv)
{
    (void)argc;
    char p[4096];
    sem_init(&go, 0, 0); sem_init(&done, 0, 0);
    pthread_t t;
    pthread_create(&t, 0, worker, 0);
    snprintf(p, sizeof p, "%s/libreusea.so", argv[1]);
    void *a = dlopen(p, RTLD_NOW);
    get = (int (*)(void))dlsym(a, "get"); set = (void (*)(int))dlsym(a, "set");
    size_t ida = modid(a);
    sem_post(&go); sem_wait(&done);
    dlclose(a);
    snprintf(p, sizeof p, "%s/libreuseb.so", argv[1]);
    void *b = dlopen(p, RTLD_NOW);
    get = (int (*)(void))dlsym(b, "get"); set = (void (*)(int))dlsym(b, "set");
    if (modid(b) != ida) { printf("no-reuse %zu %zu\n", ida, modid(b)); return 0; }
    sem_post(&go); sem_wait(&done);
    pthread_join(t, 0);
    printf("%d\n", seen);
    return 0;
}
EOF
gcc -O2 "$T/reuse.c" -o "$T/reuse" -pthread -ldl
got=$(LD_AUDIT="$MOD" LD_PRELOAD="$T/libfix.so" "$T/reuse" "$T")
case $got in
9) echo "  ok   reused module ID: a running thread sees the new library's value (9)" ;;
no-reuse*) echo "FAIL: reused module ID: the loader did not reuse the ID ($got), the check proves nothing"; fail=1 ;;
*) echo "FAIL: reused module ID: a running thread read '$got', expected the new library's 9"; fail=1 ;;
esac

# A start-up library's initialised thread-local variable reads back its
# initial value in a new thread through the shim, with static TLS (no audit
# module) and dynamic TLS (audit module): a wrong block or offset reads
# something else. The address must match the one the library reports itself.
printf '__thread int iv = 1234;\nint *ivp(void) { return &iv; }\nint ivget(void) { return iv; }\n' > "$T/libinit.c"
gcc -O2 -fPIC -shared -mtls-dialect=gnu "$T/libinit.c" -o "$T/libinit.so"
cat > "$T/init.c" <<'EOF'
#include <pthread.h>
#include <stdio.h>
int *ivp(void); int ivget(void);
static void *run(void *a) { (void)a; printf("%d\n", ivget() == 1234 && *ivp() == 1234 ? 1234 : ivget()); return 0; }
int main(void) { pthread_t t; pthread_create(&t, 0, run, 0); pthread_join(t, 0); return 0; }
EOF
gcc -O2 "$T/init.c" -o "$T/init" -L"$T" -linit -Wl,-rpath,"$T" -pthread
got=$(env -u LD_AUDIT LD_PRELOAD="$T/libfix.so" "$T/init" 2>&1) || got="exit $? $got"
[ "$got" = 1234 ] && echo "  ok   static TLS through the shim: initial value 1234" \
    || { echo "FAIL: static TLS through the shim: read '$got', expected 1234"; fail=1; }
got=$(LD_AUDIT="$MOD" LD_PRELOAD="$T/libfix.so" "$T/init" 2>&1) || got="exit $? $got"
[ "$got" = 1234 ] && echo "  ok   dynamic TLS through the shim: initial value 1234" \
    || { echo "FAIL: dynamic TLS through the shim: read '$got', expected 1234"; fail=1; }

# The built shim exports it, and the start-up libraries bind to it.
if [ -s "$SHIM/libmenufix.so" ]; then
    if readelf --dyn-syms -W "$SHIM/libmenufix.so" | grep -Eq ' FUNC +GLOBAL +DEFAULT +[0-9]+ __tls_get_addr@@GLIBC_2\.3$'; then
        echo "  ok   libmenufix.so exports __tls_get_addr@@GLIBC_2.3"
    else echo "FAIL: libmenufix.so does not export __tls_get_addr@@GLIBC_2.3"; fail=1; fi
    APP=$PREFIX/push3_opt/push3
    if [ -x "$APP/Live" ]; then
        mkdir -p "$T/b"
        env -u DISPLAY -u WAYLAND_DISPLAY LD_LIBRARY_PATH="$APP/lib:$PREFIX/compat_lib" \
            LD_PRELOAD="$SHIM/libmenufix.so" LD_AUDIT="$MOD" LD_TRACE_LOADED_OBJECTS=1 LD_WARN=yes LD_BIND_NOW=1 \
            LD_DEBUG=bindings LD_DEBUG_OUTPUT="$T/b/b" "$APP/Live" > /dev/null 2>&1 || true
        if cat "$T"/b/b.* 2>/dev/null | grep -Eq "binding file [^ ]*/libQt5Core\.so\.5 \[0\] to [^ ]*/libmenufix\.so \[0\]: normal symbol .__tls_get_addr' \[GLIBC_2\.3\]"; then
            echo "  ok   installed Live: libQt5Core.so.5's __tls_get_addr binds to libmenufix.so"
        else echo "FAIL: installed Live: libQt5Core.so.5's __tls_get_addr does not bind to libmenufix.so"; fail=1; fi
    else
        echo "skip: no installed Live at $APP: binding check not run"
    fi
else
    echo "skip: no libmenufix.so in $SHIM: export and binding checks not run"
fi
[ "$fail" = 0 ] && echo "PASS: thread-local values survive another thread's TLS set-up with the audit module"
exit $fail
