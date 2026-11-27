// ableton: links for the launcher (tools/launcher.c): makes sure the browser can hand an
// authorization link (ableton://authorize?...) back to Live. Included by launcher.c and
// by tests/link-handler.c; plain C, libc only.
//
// At each start, in the background (its own session, so a closing terminal cannot end
// it): the handler is looked up in the mimeapps.list files in the XDG order
// (<desktop>-mimeapps.list before mimeapps.list; config before data folders; xdg-mime
// itself is not used: on KDE its query needs qtpaths and can report a stale handler).
// When nothing handles x-scheme-handler/ableton, or
// the handler's program is gone (an install folder that was moved or deleted) or is
// the bare Live binary (Live's own registration, which cannot start without the
// launcher), this writes <XDG_DATA_HOME>/applications/ableton-live-<major>-links.desktop
// (hidden from menus, Exec = this launcher) and makes it the default in the list file
// that decided (else <XDG_CONFIG_HOME>/mimeapps.list). A
// working handler, also another install's, is left alone. Portable installs have no
// menu entry, so this is their only handler; for a normal install it repairs the one
// install.sh set. LIVE_LINK_HANDLER=0 turns it off.
#include <ctype.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#define LINK_SCHEME "x-scheme-handler/ableton"

// The program a desktop entry's Exec line starts (first word, desktop-entry quoting undone).
static int link_exec_program(const char *desktop, char *out, size_t size)
{
    FILE *f = fopen(desktop, "r");
    if (!f)
        return 0;
    char line[4096];
    int in_entry = 0, found = 0;
    while (!found && fgets(line, sizeof line, f)) {
        line[strcspn(line, "\r\n")] = '\0';
        if (line[0] == '[') {
            in_entry = !strcmp(line, "[Desktop Entry]");
            continue;
        }
        if (!in_entry || strncmp(line, "Exec=", 5) != 0)
            continue;
        const char *p = line + 5;
        while (*p == ' ')
            ++p;
        size_t n = 0;
        if (*p == '"') {
            for (++p; *p && *p != '"' && n + 1 < size; ++p) {
                if (*p == '\\' && p[1])
                    ++p;
                out[n++] = *p;
            }
        } else {
            for (; *p && *p != ' ' && n + 1 < size; ++p)
                out[n++] = *p;
        }
        out[n] = '\0';
        for (char *pc = strstr(out, "%%"); pc; pc = strstr(pc + 1, "%%")) // %% is a literal %
            memmove(pc + 1, pc + 2, strlen(pc + 2) + 1);
        found = n > 0;
    }
    fclose(f);
    return found;
}

// The desktop entry file for an id such as "ableton-live-12.desktop".
static int link_find_desktop(const char *id, char *out, size_t size)
{
    const char *data_home = getenv("XDG_DATA_HOME"), *home = getenv("HOME");
    const char *dirs = getenv("XDG_DATA_DIRS");
    char list[8192];
    if (data_home && *data_home)
        snprintf(list, sizeof list, "%s:%s", data_home, dirs && *dirs ? dirs : "/usr/local/share:/usr/share");
    else
        snprintf(list, sizeof list, "%s/.local/share:%s", home ? home : "", dirs && *dirs ? dirs : "/usr/local/share:/usr/share");
    for (char *save = NULL, *d = strtok_r(list, ":", &save); d; d = strtok_r(NULL, ":", &save)) {
        snprintf(out, size, "%s/applications/%s", d, id);
        if (access(out, R_OK) == 0)
            return 1;
    }
    return 0;
}

// Whether the program can run: absolute and executable, or found on PATH. The bare
// Live binary (push3/Live) counts as broken.
static int link_program_works(const char *program)
{
    const size_t n = strlen(program);
    if (n >= 11 && !strcmp(program + n - 11, "/push3/Live"))
        return 0;
    if (strchr(program, '/'))
        return access(program, X_OK) == 0;
    const char *path = getenv("PATH");
    char dirs[8192], file[PATH_MAX];
    snprintf(dirs, sizeof dirs, "%s", path && *path ? path : "/usr/local/bin:/usr/bin:/bin");
    for (char *save = NULL, *d = strtok_r(dirs, ":", &save); d; d = strtok_r(NULL, ":", &save)) {
        snprintf(file, sizeof file, "%s/%s", d, program);
        if (access(file, X_OK) == 0)
            return 1;
    }
    return 0;
}

// The mimeapps.list files in the order they count, into list (one path per entry).
static int link_list_files(char files[][PATH_MAX], int max)
{
    const char *home = getenv("HOME"), *config = getenv("XDG_CONFIG_HOME"), *data = getenv("XDG_DATA_HOME");
    const char *config_dirs = getenv("XDG_CONFIG_DIRS"), *data_dirs = getenv("XDG_DATA_DIRS");
    const char *desktops = getenv("XDG_CURRENT_DESKTOP");
    char bases[16][PATH_MAX];
    int nb = 0;
    char buf[8192];
    if (config && *config)
        snprintf(bases[nb++], PATH_MAX, "%s", config);
    else
        snprintf(bases[nb++], PATH_MAX, "%s/.config", home ? home : "");
    snprintf(buf, sizeof buf, "%s", config_dirs && *config_dirs ? config_dirs : "/etc/xdg");
    for (char *save = NULL, *d = strtok_r(buf, ":", &save); d && nb < 8; d = strtok_r(NULL, ":", &save))
        snprintf(bases[nb++], PATH_MAX, "%s", d);
    if (data && *data)
        snprintf(bases[nb++], PATH_MAX, "%s/applications", data);
    else
        snprintf(bases[nb++], PATH_MAX, "%s/.local/share/applications", home ? home : "");
    snprintf(buf, sizeof buf, "%s", data_dirs && *data_dirs ? data_dirs : "/usr/local/share:/usr/share");
    for (char *save = NULL, *d = strtok_r(buf, ":", &save); d && nb < 16; d = strtok_r(NULL, ":", &save))
        snprintf(bases[nb++], PATH_MAX, "%s/applications", d);
    int n = 0;
    for (int b = 0; b < nb; ++b) {
        char desk[256];
        snprintf(desk, sizeof desk, "%s", desktops ? desktops : "");
        for (char *save = NULL, *d = strtok_r(desk, ":", &save); d && n < max; d = strtok_r(NULL, ":", &save)) {
            char lower[64];
            size_t i = 0;
            for (; d[i] && i + 1 < sizeof lower; ++i)
                lower[i] = (char)tolower((unsigned char)d[i]);
            lower[i] = '\0';
            snprintf(files[n++], PATH_MAX, "%s/%s-mimeapps.list", bases[b], lower);
        }
        if (n < max)
            snprintf(files[n++], PATH_MAX, "%s/mimeapps.list", bases[b]);
    }
    return n;
}

// The default handler's desktop id ("" when none) and the list file that set it.
static void link_current_handler(char *out, size_t size, char *from, size_t from_size)
{
    out[0] = from[0] = '\0';
    char files[48][PATH_MAX];
    const int n = link_list_files(files, 48);
    for (int i = 0; i < n; ++i) {
        FILE *f = fopen(files[i], "r");
        if (!f)
            continue;
        char line[4096];
        int in_defaults = 0, found = 0;
        while (!found && fgets(line, sizeof line, f)) {
            line[strcspn(line, "\r\n")] = '\0';
            if (line[0] == '[') {
                in_defaults = !strcmp(line, "[Default Applications]");
                continue;
            }
            if (in_defaults && !strncmp(line, LINK_SCHEME "=", sizeof LINK_SCHEME)) {
                snprintf(out, size, "%s", line + sizeof LINK_SCHEME);
                out[strcspn(out, ";")] = '\0'; // a list: the first one counts
                snprintf(from, from_size, "%s", files[i]);
                found = 1;
            }
        }
        fclose(f);
        if (found)
            return;
    }
}

// Sets LINK_SCHEME=id in [Default Applications] of the list file (made when missing).
static int link_set_default(const char *list, const char *id)
{
    char *text = NULL;
    size_t len = 0;
    FILE *f = fopen(list, "r");
    if (f) {
        fseek(f, 0, SEEK_END);
        const long size = ftell(f);
        rewind(f);
        if (size > 0 && size < (1 << 22) && (text = malloc((size_t)size + 1))) {
            len = fread(text, 1, (size_t)size, f);
            text[len] = '\0';
        }
        fclose(f);
    }
    char tmp[PATH_MAX + 8];
    snprintf(tmp, sizeof tmp, "%s.new", list);
    FILE *o = fopen(tmp, "w");
    if (!o) {
        free(text);
        return 0;
    }
    int in_defaults = 0, done = 0, saw_defaults = 0;
    for (char *line = text, *next; line && *line; line = next) {
        next = strchr(line, '\n');
        if (next)
            *next++ = '\0';
        if (line[0] == '[') {
            if (in_defaults && !done) { // the section ends without the key
                fprintf(o, LINK_SCHEME "=%s\n", id);
                done = 1;
            }
            in_defaults = !strcmp(line, "[Default Applications]");
            saw_defaults |= in_defaults;
        } else if (in_defaults && !strncmp(line, LINK_SCHEME "=", sizeof LINK_SCHEME)) {
            if (!done)
                fprintf(o, LINK_SCHEME "=%s\n", id);
            done = 1;
            continue;
        }
        fprintf(o, "%s\n", line);
    }
    if (!done) {
        if (!saw_defaults)
            fprintf(o, "%s[Default Applications]\n", len ? "\n" : "");
        else if (!in_defaults)
            fprintf(o, "\n[Default Applications]\n");
        fprintf(o, LINK_SCHEME "=%s\n", id);
    }
    free(text);
    if (fclose(o) != 0 || rename(tmp, list) != 0) {
        unlink(tmp);
        return 0;
    }
    return 1;
}

// Runs a program with its output discarded; returns its exit status (-1 when it did not run).
static int link_run(char *const argv[])
{
    const pid_t pid = fork();
    if (pid == 0) {
        FILE *null = freopen("/dev/null", "w", stdout);
        (void)null;
        null = freopen("/dev/null", "w", stderr);
        execvp(argv[0], argv);
        _exit(127);
    }
    int status = 0;
    if (pid < 0 || waitpid(pid, &status, 0) < 0 || !WIFEXITED(status))
        return -1;
    return WEXITSTATUS(status);
}

// Exec quoting for a desktop entry: "path" with \ " ` $ escaped, % doubled.
static void link_quote(const char *in, char *out, size_t size)
{
    size_t n = 0;
    out[n++] = '"';
    for (; *in && n + 4 < size; ++in) {
        if (*in == '"' || *in == '`' || *in == '$' || *in == '\\')
            out[n++] = '\\';
        else if (*in == '%')
            out[n++] = '%';
        out[n++] = *in;
    }
    out[n++] = '"';
    out[n] = '\0';
}

// Writes the links entry for launcher and makes it the default; returns 1 when it did.
static int link_register(const char *launcher, const char *major, const char *list)
{
    const char *data_home = getenv("XDG_DATA_HOME"), *home = getenv("HOME");
    char dir[PATH_MAX], file[PATH_MAX], tmp[PATH_MAX + 8], quoted[2 * PATH_MAX], id[64];
    if (data_home && *data_home)
        snprintf(dir, sizeof dir, "%s/applications", data_home);
    else if (home && *home)
        snprintf(dir, sizeof dir, "%s/.local/share/applications", home);
    else
        return 0;
    for (char *slash = strchr(dir + 1, '/'); slash; slash = strchr(slash + 1, '/')) { // mkdir -p
        *slash = '\0';
        mkdir(dir, 0755);
        *slash = '/';
    }
    mkdir(dir, 0755);
    snprintf(id, sizeof id, "ableton-live-%s-links.desktop", major);
    snprintf(file, sizeof file, "%s/%s", dir, id);
    snprintf(tmp, sizeof tmp, "%s.new", file);
    link_quote(launcher, quoted, sizeof quoted);
    FILE *f = fopen(tmp, "w");
    if (!f)
        return 0;
    const int ok = fprintf(f,
                           "[Desktop Entry]\n"
                           "Type=Application\n"
                           "Name=Ableton Live %s (links)\n"
                           "Comment=Hands ableton: links (authorization in the browser) to Live; written by its launcher\n"
                           "Exec=%s %%u\n"
                           "NoDisplay=true\n"
                           "Terminal=false\n"
                           "MimeType=" LINK_SCHEME ";\n",
                           major, quoted) > 0;
    if (fclose(f) != 0 || !ok || rename(tmp, file) != 0) {
        unlink(tmp);
        return 0;
    }
    char config_list[PATH_MAX];
    if (!list || !*list || access(list, W_OK) != 0) { // a system file: the user's own list wins over it
        const char *config = getenv("XDG_CONFIG_HOME");
        if (config && *config)
            snprintf(config_list, sizeof config_list, "%s/mimeapps.list", config);
        else
            snprintf(config_list, sizeof config_list, "%s/.config/mimeapps.list", home ? home : "");
        list = config_list;
    }
    const int set = link_set_default(list, id);
    char *const update[] = {"update-desktop-database", dir, NULL}; // optional: not every system has it
    link_run(update);
    return set;
}

// The whole check; returns 0 nothing to do, 1 registered, -1 off or failed.
static int link_check(const char *launcher, const char *major)
{
    const char *off = getenv("LIVE_LINK_HANDLER");
    if (off && !strcmp(off, "0"))
        return -1;
    char id[256], from[PATH_MAX], desktop[PATH_MAX], program[PATH_MAX];
    link_current_handler(id, sizeof id, from, sizeof from);
    if (*id && link_find_desktop(id, desktop, sizeof desktop) && link_exec_program(desktop, program, sizeof program)
        && link_program_works(program))
        return 0;
    return link_register(launcher, major, from) ? 1 : -1;
}
