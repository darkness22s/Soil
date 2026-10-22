#define _GNU_SOURCE
#include "json.h"
#include "sol.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/loop.h>
#include <limits.h>
#include <pwd.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#define SOIL_SHARE "/usr/local/share/soil"
#define SOIL_SAMPLES SOIL_SHARE "/samples"
#define SOIL_DOCS SOIL_SHARE "/docs"
#define SOIL_APP_BIN "/usr/local/bin/soil-app"
#define SOIL_BROWSER "/usr/local/bin/soil-browser"
#define SOIL_TRUST_SYS SOIL_SHARE "/trust"

static void die(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    fprintf(stderr, "soil: ");
    vfprintf(stderr, fmt, ap);
    fputc('\n', stderr);
    va_end(ap);
    exit(1);
}

static void warn(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    fprintf(stderr, "soil: warning: ");
    vfprintf(stderr, fmt, ap);
    fputc('\n', stderr);
    va_end(ap);
}

static const char *home_dir(void)
{
    const char *h = getenv("HOME");
    if (h && *h)
        return h;
    struct passwd *pw = getpwuid(getuid());
    return pw && pw->pw_dir ? pw->pw_dir : "/home/tc";
}

static int mkdir_p(const char *path, mode_t mode)
{
    char tmp[PATH_MAX];
    snprintf(tmp, sizeof(tmp), "%s", path);
    for (char *p = tmp + 1; *p; p++) {
        if (*p == '/') {
            *p = 0;
            if (mkdir(tmp, mode) != 0 && errno != EEXIST)
                return -1;
            *p = '/';
        }
    }
    if (mkdir(tmp, mode) != 0 && errno != EEXIST)
        return -1;
    return 0;
}

static int copy_file(const char *src, const char *dst, mode_t mode)
{
    FILE *in = fopen(src, "rb");
    if (!in)
        return -1;
    FILE *out = fopen(dst, "wb");
    if (!out) {
        fclose(in);
        return -1;
    }
    char buf[65536];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), in)) > 0) {
        if (fwrite(buf, 1, n, out) != n) {
            fclose(in);
            fclose(out);
            return -1;
        }
    }
    int bad = ferror(in);
    fclose(in);
    fclose(out);
    if (bad)
        return -1;
    chmod(dst, mode);
    return 0;
}

static int rm_rf(const char *path)
{
    struct stat st;
    if (lstat(path, &st) != 0)
        return errno == ENOENT ? 0 : -1;
    if (S_ISDIR(st.st_mode)) {
        DIR *d = opendir(path);
        if (d) {
            struct dirent *de;
            while ((de = readdir(d)) != NULL) {
                if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, ".."))
                    continue;
                char sub[PATH_MAX];
                snprintf(sub, sizeof(sub), "%s/%s", path, de->d_name);
                if (rm_rf(sub) != 0) {
                    closedir(d);
                    return -1;
                }
            }
            closedir(d);
        }
        return rmdir(path);
    }
    return unlink(path);
}

static int valid_id(const char *id)
{
    if (!id || !*id)
        return 0;
    size_t n = strlen(id);
    if (n < 2 || n > 64)
        return 0;
    for (size_t i = 0; i < n; i++) {
        char c = id[i];
        int ok = (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
                 c == '.' || c == '_' || c == '-';
        if (!ok)
            return 0;
        if (i == 0 && !((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9')))
            return 0;
    }
    if (strstr(id, "..") || strstr(id, "/"))
        return 0;
    return 1;
}

static int valid_rel(const char *p)
{
    if (!p || !*p || p[0] == '/')
        return 0;
    const char *s = p;
    while (*s) {
        const char *slash = strchr(s, '/');
        size_t n = slash ? (size_t)(slash - s) : strlen(s);
        if (n == 0 || (n == 2 && s[0] == '.' && s[1] == '.'))
            return 0;
        if (!slash)
            break;
        s = slash + 1;
    }
    return 1;
}

static char *self_exe(void)
{
    static char buf[PATH_MAX];
    ssize_t n = readlink("/proc/self/exe", buf, sizeof(buf) - 1);
    if (n <= 0)
        return "/usr/local/bin/soil";
    buf[n] = 0;
    return buf;
}

static int spawn_wait(char *const argv[], const char *cwd, char *const envp[])
{
    pid_t pid = fork();
    if (pid < 0)
        return -1;
    if (pid == 0) {
        if (cwd && chdir(cwd) != 0)
            _exit(127);
        execve(argv[0], argv, envp ? envp : environ);
        execvp(argv[0], argv);
        _exit(127);
    }
    int status = 0;
    if (waitpid(pid, &status, 0) < 0)
        return -1;
    if (WIFEXITED(status))
        return WEXITSTATUS(status);
    return 128 + (WIFSIGNALED(status) ? WTERMSIG(status) : 0);
}

static int spawn_detached(char *const argv[])
{
    pid_t pid = fork();
    if (pid < 0)
        return -1;
    if (pid == 0) {
        int devnull = open("/dev/null", O_RDWR);
        if (devnull >= 0) {
            dup2(devnull, 0);
            dup2(devnull, 1);
            dup2(devnull, 2);
        }
        execvp(argv[0], argv);
        _exit(127);
    }
    return 0;
}

static int do_mount(const char *file, const char *dir)
{
    int ffd = open(file, O_RDONLY);
    if (ffd < 0) {
        fprintf(stderr, "soil: cannot open %s: %s\n", file, strerror(errno));
        return 1;
    }
    char loopname[32];
    int lfd = -1;
    int ctl = open("/dev/loop-control", O_RDWR);
    if (ctl >= 0) {
        int n = ioctl(ctl, LOOP_CTL_GET_FREE);
        close(ctl);
        if (n >= 0) {
            snprintf(loopname, sizeof(loopname), "/dev/loop%d", n);
            lfd = open(loopname, O_RDWR);
            int oe = errno;
            if (lfd < 0 && (oe == ENOENT || oe == EACCES)) {
                if (mknod(loopname, S_IFBLK | 0600, makedev(7, n)) == 0 || errno == EEXIST) {
                    usleep(30000);
                    lfd = open(loopname, O_RDWR);
                }
            }
        }
    }
    if (lfd < 0) {
        for (int i = 0; i < 32 && lfd < 0; i++) {
            snprintf(loopname, sizeof(loopname), "/dev/loop%d", i);
            int fd = open(loopname, O_RDWR);
            if (fd < 0)
                continue;
            struct loop_info64 info;
            if (ioctl(fd, LOOP_GET_STATUS64, &info) != 0 && errno == ENXIO)
                lfd = fd;
            else
                close(fd);
        }
    }
    if (lfd < 0) {
        fprintf(stderr, "soil: no free loop device\n");
        close(ffd);
        return 1;
    }
    if (ioctl(lfd, LOOP_SET_FD, ffd) != 0) {
        fprintf(stderr, "soil: LOOP_SET_FD failed: %s\n", strerror(errno));
        close(lfd);
        close(ffd);
        return 1;
    }
    struct loop_info64 info;
    memset(&info, 0, sizeof(info));
    info.lo_flags = LO_FLAGS_AUTOCLEAR | LO_FLAGS_READ_ONLY;
    sol_header h;
    if (sol_read_header(file, &h, NULL, 0) == 0)
        info.lo_offset = h.payload_offset;
    snprintf((char *)info.lo_file_name, LO_NAME_SIZE, "%s", file);
    if (ioctl(lfd, LOOP_SET_STATUS64, &info) != 0) {
        fprintf(stderr, "soil: LOOP_SET_STATUS64 failed: %s\n", strerror(errno));
        ioctl(lfd, LOOP_CLR_FD, 0);
        close(lfd);
        close(ffd);
        return 1;
    }
    /* keep the loop fd open across mount(): LO_FLAGS_AUTOCLEAR would else
       detach the device before the filesystem gets mounted */
    if (mount(loopname, dir, "squashfs", MS_RDONLY | MS_NOSUID | MS_NODEV, NULL) != 0) {
        fprintf(stderr, "soil: mount %s on %s failed: %s\n", loopname, dir,
                strerror(errno));
        ioctl(lfd, LOOP_CLR_FD, 0);
        close(lfd);
        close(ffd);
        return 1;
    }
    close(lfd);
    close(ffd);
    return 0;
}

static int do_umount(const char *dir)
{
    if (umount2(dir, MNT_DETACH) != 0) {
        fprintf(stderr, "soil: umount %s failed: %s\n", dir, strerror(errno));
        return 1;
    }
    return 0;
}

static int sudo_self(const char *arg1, const char *arg2, const char *arg3)
{
    char *args[8];
    int i = 0;
    args[i++] = "sudo";
    args[i++] = "-n";
    args[i++] = self_exe();
    args[i++] = (char *)arg1;
    if (arg2) args[i++] = (char *)arg2;
    if (arg3) args[i++] = (char *)arg3;
    args[i] = NULL;
    int rc = spawn_wait(args, NULL, NULL);
    if (rc == 127)
        fprintf(stderr, "soil: sudo unavailable\n");
    return rc;
}

static int mount_pkg(const char *file, const char *dir)
{
    if (geteuid() == 0)
        return do_mount(file, dir);
    return sudo_self("_mount", file, dir);
}

static int umount_pkg(const char *dir)
{
    if (geteuid() == 0)
        return do_umount(dir);
    return sudo_self("_umount", dir, NULL);
}

static int verify_package(const char *file, sol_header *h, jval **manifest, int strict)
{
    char err[256];
    if (sol_read_header(file, h, err, sizeof(err)) != 0)
        die("%s", err);
    uint8_t digest[32];
    if (sol_compute_digest(file, h, digest) != 0)
        die("cannot read package data");
    if (memcmp(digest, h->digest, 32) != 0)
        die("%s: integrity check failed (digest mismatch)", file);
    char *raw = NULL;
    if (sol_read_manifest(file, h, &raw, NULL) != 0)
        die("cannot read manifest");
    char perr[160];
    jval *m = json_parse(raw, strlen(raw), perr, sizeof(perr));
    if (!m) {
        fprintf(stderr, "soil: invalid manifest: %s\n", perr);
        exit(1);
    }
    const char *id = json_str_get(m, "id");
    if (!valid_id(id))
        die("manifest: invalid id");
    const char *kind = json_str_get(m, "kind");
    if (!kind || sol_kind_from_name(kind) != h->kind)
        die("manifest kind does not match header");
    const char *ver = json_str_get(m, "version");
    if (!ver || !*ver)
        die("manifest: missing version");
    char signer[128];
    int sig = sol_verify_signature(file, h, signer, sizeof(signer));
    if (sig == SOL_SIG_TRUSTED) {
        printf("soil: trusted package (signer: %s)\n", signer);
    } else if (sig == SOL_SIG_UNTRUSTED) {
        if (strict || getenv("SOIL_STRICT"))
            die("%s is signed but not trusted", file);
        if (!getenv("SOIL_ALLOW_UNTRUSTED"))
            warn("untrusted package (signer not in trust store): %s", file);
    } else {
        if (strict || getenv("SOIL_STRICT"))
            die("%s is unsigned (strict mode)", file);
        if (!getenv("SOIL_ALLOW_UNTRUSTED"))
            warn("unsigned package: %s", file);
    }
    free(raw);
    *manifest = m;
    return sig;
}

static void free_manifest(jval *m)
{
    json_free(m);
}

static int cmd_run(const char *file, int strict, int debug, int extra_argc, char **extra_argv)
{
    sol_header h;
    jval *m = NULL;
    (void)extra_argc;
    (void)extra_argv;
    verify_package(file, &h, &m, strict);

    const char *id = json_str_get(m, "id");
    const char *name = json_str_get(m, "name");
    char tmpdir[] = "/tmp/soil-run-XXXXXX";
    if (!mkdtemp(tmpdir))
        die("cannot create temp dir: %s", strerror(errno));
    char mnt[PATH_MAX], manpath[PATH_MAX], data[PATH_MAX];
    snprintf(mnt, sizeof(mnt), "%s/mnt", tmpdir);
    snprintf(manpath, sizeof(manpath), "%s/manifest.json", tmpdir);
    if (mkdir(mnt, 0755) != 0)
        die("mkdir failed");

    char *raw = NULL;
    if (sol_read_manifest(file, &h, &raw, NULL) != 0)
        die("cannot extract manifest");
    FILE *mf = fopen(manpath, "w");
    if (!mf)
        die("cannot write manifest");
    fputs(raw, mf);
    fclose(mf);
    free(raw);

    char datadir[PATH_MAX];
    snprintf(datadir, sizeof(datadir), "%s/.local/share/soil/data", home_dir());
    mkdir_p(datadir, 0755);
    snprintf(data, sizeof(data), "%s/%s", datadir, id);
    mkdir_p(data, 0755);

    if (mount_pkg(file, mnt) != 0) {
        fprintf(stderr, "soil: failed to mount package\n");
        rm_rf(tmpdir);
        free_manifest(m);
        return 1;
    }

    printf("soil: running %s %s (%s)\n", name ? name : id,
           json_str_get(m, "version") ? json_str_get(m, "version") : "", id);
    fflush(stdout);

    if (debug)
        setenv("SOIL_DEBUG", "1", 1);

    char *args[] = {
        SOIL_APP_BIN,
        "--root", mnt,
        "--manifest", manpath,
        "--id", (char *)id,
        "--data", data,
        NULL
    };
    int rc = spawn_wait(args, mnt, NULL);
    umount_pkg(mnt);
    rm_rf(tmpdir);
    free_manifest(m);
    return rc;
}

static int find_installed(const char *id, char *out, size_t outlen)
{
    char pkgdir[PATH_MAX];
    snprintf(pkgdir, sizeof(pkgdir), "%s/.local/share/soil/pkg", home_dir());
    DIR *d = opendir(pkgdir);
    if (!d)
        return -1;
    char prefix[96];
    snprintf(prefix, sizeof(prefix), "%s-", id);
    size_t plen = strlen(prefix);
    char best[PATH_MAX] = "";
    struct dirent *de;
    while ((de = readdir(d)) != NULL) {
        size_t n = strlen(de->d_name);
        if (n < plen + 4 || strncmp(de->d_name, prefix, plen) != 0)
            continue;
        if (strcmp(de->d_name + n - 4, ".sol") != 0)
            continue;
        if (strcmp(de->d_name, best) > 0) {
            snprintf(best, sizeof(best), "%s", de->d_name);
        }
    }
    closedir(d);
    if (!best[0]) {
        snprintf(best, sizeof(best), "%s", id);
    }
    if (!best[0])
        return -1;
    snprintf(out, outlen, "%s/%s", pkgdir, best);
    return access(out, R_OK) == 0 ? 0 : -1;
}

static void desktop_escape(char *s)
{
    for (char *p = s; *p; p++) {
        if (*p == '\n' || *p == '\r' || *p == '\t')
            *p = ' ';
    }
}

static int write_desktop(const char *path, const char *name, const char *comment,
                         const char *id, const char *icon, int exec_perm)
{
    FILE *f = fopen(path, "w");
    if (!f)
        return -1;
    fprintf(f,
            "[Desktop Entry]\n"
            "Type=Application\n"
            "Name=%s\n"
            "Comment=%s\n"
            "Exec=/usr/local/bin/soil launch %s\n"
            "Icon=%s\n"
            "Terminal=false\n"
            "Categories=Soil;\n",
            name, comment ? comment : "", id, icon);
    fclose(f);
    if (exec_perm)
        chmod(path, 0755);
    else
        chmod(path, 0644);
    return 0;
}

static int run_installer_hook(const char *optdir, const char *datadir, const char *id,
                              const char *version, const char *command, jval *args,
                              int as_root)
{
    char script[PATH_MAX];
    snprintf(script, sizeof(script), "%s/%s", optdir, command);
    if (access(script, F_OK) != 0) {
        fprintf(stderr, "soil: install.command not found: %s\n", script);
        return 1;
    }
    int argc = 0;
    char *args_arr[64];
    args_arr[argc++] = "/bin/sh";
    args_arr[argc++] = script;
    if (args && args->type == JSON_ARR) {
        for (size_t i = 0; i < args->n && argc < 60; i++) {
            if (args->items[i]->type == JSON_STR)
                args_arr[argc++] = args->items[i]->str;
        }
    }
    args_arr[argc] = NULL;

    if (as_root && geteuid() != 0) {
        char *sargs[64];
        int i = 0;
        sargs[i++] = "sudo";
        sargs[i++] = "-n";
        sargs[i++] = "--";
        char envid[128], envver[64], envdir[PATH_MAX], envdata[PATH_MAX];
        snprintf(envid, sizeof(envid), "SOIL_APP_ID=%s", id);
        snprintf(envver, sizeof(envver), "SOIL_VERSION=%s", version ? version : "");
        snprintf(envdir, sizeof(envdir), "SOIL_APPDIR=%s", optdir);
        snprintf(envdata, sizeof(envdata), "SOIL_DATA=%s", datadir);
        sargs[i++] = "env";
        sargs[i++] = envid;
        sargs[i++] = envver;
        sargs[i++] = envdir;
        sargs[i++] = envdata;
        for (int k = 0; k < argc && i < 62; k++)
            sargs[i++] = args_arr[k];
        sargs[i] = NULL;
        return spawn_wait(sargs, optdir, NULL);
    }

    char envid[128], envver[64], envdir[PATH_MAX], envdata[PATH_MAX];
    snprintf(envid, sizeof(envid), "SOIL_APP_ID=%s", id);
    snprintf(envver, sizeof(envver), "SOIL_VERSION=%s", version ? version : "");
    snprintf(envdir, sizeof(envdir), "SOIL_APPDIR=%s", optdir);
    snprintf(envdata, sizeof(envdata), "SOIL_DATA=%s", datadir);
    setenv("SOIL_APP_ID", id, 1);
    setenv("SOIL_VERSION", version ? version : "", 1);
    setenv("SOIL_APPDIR", optdir, 1);
    setenv("SOIL_DATA", datadir, 1);
    return spawn_wait(args_arr, optdir, NULL);
}

static int cmd_install(const char *file)
{
    sol_header h;
    jval *m = NULL;
    verify_package(file, &h, &m, 0);

    const char *id = json_str_get(m, "id");
    const char *name = json_str_get(m, "name");
    const char *version = json_str_get(m, "version");
    const char *icon = json_str_get(m, "icon");
    const char *desc = json_str_get(m, "description");
    const char *home = home_dir();
    char base[PATH_MAX], pkgdir[PATH_MAX], icondir[PATH_MAX], optdir[PATH_MAX];
    char datadir[PATH_MAX], appsdir[PATH_MAX], deskdir[PATH_MAX];
    snprintf(base, sizeof(base), "%s/.local/share/soil", home);
    snprintf(pkgdir, sizeof(pkgdir), "%s/pkg", base);
    snprintf(icondir, sizeof(icondir), "%s/icons", base);
    snprintf(optdir, sizeof(optdir), "%s/opt/%s", base, id);
    snprintf(datadir, sizeof(datadir), "%s/data/%s", base, id);
    snprintf(appsdir, sizeof(appsdir), "%s/.local/share/applications", home);
    snprintf(deskdir, sizeof(deskdir), "%s/Desktop", home);
    mkdir_p(pkgdir, 0755);
    mkdir_p(icondir, 0755);
    mkdir_p(appsdir, 0755);
    mkdir_p(deskdir, 0755);

    char tmpdir[] = "/tmp/soil-inst-XXXXXX";
    if (!mkdtemp(tmpdir))
        die("cannot create temp dir");
    char mnt[PATH_MAX];
    snprintf(mnt, sizeof(mnt), "%s/mnt", tmpdir);
    mkdir(mnt, 0755);
    if (mount_pkg(file, mnt) != 0)
        die("failed to mount package");

    char iconpath[PATH_MAX];
    snprintf(iconpath, sizeof(iconpath), "%s/%s.png", icondir, id);
    if (icon && valid_rel(icon)) {
        char src[PATH_MAX];
        snprintf(src, sizeof(src), "%s/%s", mnt, icon);
        copy_file(src, iconpath, 0644);
    }

    int installer = (h.kind == 2);
    if (installer) {
        rm_rf(optdir);
        mkdir_p(optdir, 0755);
        char *cp[] = { "/bin/cp", "-R", NULL, optdir, NULL };
        char src[PATH_MAX];
        snprintf(src, sizeof(src), "%s/.", mnt);
        cp[2] = src;
        if (spawn_wait(cp, NULL, NULL) != 0)
            warn("payload extraction reported errors");
    }
    umount_pkg(mnt);
    rm_rf(tmpdir);

    char current[PATH_MAX];
    snprintf(current, sizeof(current), "%s/%s-%s.sol", pkgdir, id, version);
    if (copy_file(file, current, 0644) != 0)
        die("cannot copy package into %s", pkgdir);

    DIR *d = opendir(pkgdir);
    if (d) {
        struct dirent *de;
        while ((de = readdir(d)) != NULL) {
            char other[PATH_MAX];
            snprintf(other, sizeof(other), "%s/%s", pkgdir, de->d_name);
            if (strcmp(other, current) != 0 && strstr(de->d_name, id) == de->d_name)
                unlink(other);
        }
        closedir(d);
    }

    char safe_name[256];
    snprintf(safe_name, sizeof(safe_name), "%s", name ? name : id);
    desktop_escape(safe_name);

    char app_entry[PATH_MAX], desk_entry[PATH_MAX];
    snprintf(app_entry, sizeof(app_entry), "%s/soil-%s.desktop", appsdir, id);
    snprintf(desk_entry, sizeof(desk_entry), "%s/%s.desktop", deskdir, safe_name);
    write_desktop(app_entry, safe_name, desc, id, iconpath, 0);
    write_desktop(desk_entry, safe_name, desc, id, iconpath, 1);

    if (installer) {
        jval *inst = json_obj_get(m, "install");
        const char *cmdname = inst ? json_str_get(inst, "command") : NULL;
        jval *iargs = inst ? json_obj_get(inst, "args") : NULL;
        int as_root = inst ? json_bool_get(inst, "root", 0) : 0;
        if (!cmdname || !valid_rel(cmdname))
            die("installer manifest missing install.command");
        printf("soil: running install hook: %s%s\n", cmdname, as_root ? " (root)" : "");
        int rc = run_installer_hook(optdir, datadir, id, version, cmdname, iargs, as_root);
        if (rc != 0)
            warn("install hook exited with status %d", rc);
    }

    printf("soil: installed %s %s (%s)\n", safe_name, version ? version : "", id);
    if (!installer)
        printf("soil: launch with: soil launch %s\n", id);
    printf("soil: uninstall with: soil remove %s\n", id);
    free_manifest(m);
    return 0;
}

static int cmd_list(void)
{
    char pkgdir[PATH_MAX];
    snprintf(pkgdir, sizeof(pkgdir), "%s/.local/share/soil/pkg", home_dir());
    DIR *d = opendir(pkgdir);
    if (!d) {
        printf("no packages installed\n");
        return 0;
    }
    struct dirent *de;
    int count = 0;
    while ((de = readdir(d)) != NULL) {
        size_t n = strlen(de->d_name);
        if (n < 5 || strcmp(de->d_name + n - 4, ".sol") != 0)
            continue;
        char path[PATH_MAX];
        snprintf(path, sizeof(path), "%s/%s", pkgdir, de->d_name);
        sol_header h;
        char err[128];
        if (sol_read_header(path, &h, err, sizeof(err)) != 0)
            continue;
        char *raw = NULL;
        if (sol_read_manifest(path, &h, &raw, NULL) != 0)
            continue;
        jval *m = json_parse(raw, strlen(raw), NULL, 0);
        free(raw);
        if (!m) {
            json_free(m);
            continue;
        }
        printf("%-28s %-24s %-8s %-14s %6.1f KiB\n",
               json_str_get(m, "id") ? json_str_get(m, "id") : "?",
               json_str_get(m, "name") ? json_str_get(m, "name") : "?",
               json_str_get(m, "version") ? json_str_get(m, "version") : "?",
               sol_kind_name(h.kind),
               (double)h.payload_size / 1024.0 + (double)h.header_len / 1024.0);
        json_free(m);
        count++;
    }
    closedir(d);
    if (!count)
        printf("no packages installed\n");
    return 0;
}

static int cmd_launch(const char *id, int extra_argc, char **extra_argv)
{
    if (!valid_id(id))
        die("invalid app id: %s", id);
    char path[PATH_MAX];
    if (find_installed(id, path, sizeof(path)) != 0)
        die("%s is not installed (try: soil install FILE.sol)", id);
    return cmd_run(path, 0, getenv("SOIL_DEBUG") != NULL, extra_argc, extra_argv);
}

static int cmd_remove(const char *id)
{
    if (!valid_id(id))
        die("invalid app id: %s", id);
    char base[PATH_MAX], pkgdir[PATH_MAX];
    snprintf(base, sizeof(base), "%s/.local/share/soil", home_dir());
    snprintf(pkgdir, sizeof(pkgdir), "%s/pkg", base);

    char display[256];
    display[0] = 0;
    DIR *d = opendir(pkgdir);
    if (d) {
        char prefix[96];
        snprintf(prefix, sizeof(prefix), "%s-", id);
        struct dirent *de;
        while ((de = readdir(d)) != NULL) {
            if (strncmp(de->d_name, prefix, strlen(prefix)) != 0)
                continue;
            char path[PATH_MAX];
            snprintf(path, sizeof(path), "%s/%s", pkgdir, de->d_name);
            sol_header h;
            char err[128];
            if (sol_read_header(path, &h, err, sizeof(err)) == 0) {
                char *raw = NULL;
                if (sol_read_manifest(path, &h, &raw, NULL) == 0) {
                    jval *m = json_parse(raw, strlen(raw), NULL, 0);
                    free(raw);
                    const char *nm = m ? json_str_get(m, "name") : NULL;
                    if (nm && !display[0])
                        snprintf(display, sizeof(display), "%s", nm);
                    json_free(m);
                }
            }
            unlink(path);
        }
        closedir(d);
    }

    char path[PATH_MAX];
    snprintf(path, sizeof(path), "%s/opt/%s", base, id);
    rm_rf(path);
    snprintf(path, sizeof(path), "%s/data/%s", base, id);
    rm_rf(path);
    snprintf(path, sizeof(path), "%s/icons/%s.png", base, id);
    unlink(path);
    snprintf(path, sizeof(path), "%s/.local/share/applications/soil-%s.desktop",
             home_dir(), id);
    unlink(path);
    if (display[0]) {
        char safe[256];
        snprintf(safe, sizeof(safe), "%s", display);
        desktop_escape(safe);
        snprintf(path, sizeof(path), "%s/Desktop/%s.desktop", home_dir(), safe);
        unlink(path);
    }
    char deskdir[PATH_MAX];
    snprintf(deskdir, sizeof(deskdir), "%s/Desktop", home_dir());
    d = opendir(deskdir);
    if (d) {
        struct dirent *de;
        while ((de = readdir(d)) != NULL) {
            size_t n = strlen(de->d_name);
            if (n < 9 || strcmp(de->d_name + n - 8, ".desktop") != 0)
                continue;
            char dp[PATH_MAX];
            snprintf(dp, sizeof(dp), "%s/%s", deskdir, de->d_name);
            FILE *f = fopen(dp, "r");
            if (!f)
                continue;
            char content[2048];
            size_t got = fread(content, 1, sizeof(content) - 1, f);
            fclose(f);
            content[got] = 0;
            char needle[128];
            snprintf(needle, sizeof(needle), "soil launch %s", id);
            if (strstr(content, needle))
                unlink(dp);
        }
        closedir(d);
    }
    printf("soil: removed %s\n", id);
    return 0;
}

static int cmd_info(const char *file)
{
    char err[256];
    sol_header h;
    if (sol_read_header(file, &h, err, sizeof(err)) != 0)
        die("%s", err);
    printf("file:           %s\n", file);
    printf("format:         v%u\n", h.version);
    printf("kind:           %u (%s)\n", h.kind, sol_kind_name(h.kind));
    printf("header_len:     %u\n", h.header_len);
    printf("manifest_len:   %u\n", h.manifest_len);
    printf("payload_offset: %llu\n", (unsigned long long)h.payload_offset);
    printf("payload_size:   %llu\n", (unsigned long long)h.payload_size);
    printf("digest:         ");
    for (int i = 0; i < 32; i++)
        printf("%02x", h.digest[i]);
    printf("\n");
    static const uint8_t zero[64] = { 0 };
    printf("signature:      %s\n", memcmp(h.signature, zero, 64) ? "present" : "none");
    char *raw = NULL;
    if (sol_read_manifest(file, &h, &raw, NULL) == 0) {
        printf("manifest:\n%s\n", raw);
        free(raw);
    }
    return 0;
}

static int cmd_verify(const char *file)
{
    char err[256];
    sol_header h;
    if (sol_read_header(file, &h, err, sizeof(err)) != 0)
        die("%s", err);
    uint8_t digest[32];
    if (sol_compute_digest(file, &h, digest) != 0)
        die("cannot read package data");
    int ok = !memcmp(digest, h.digest, 32);
    printf("digest:    %s\n", ok ? "OK" : "MISMATCH");
    printf("sha256:    ");
    for (int i = 0; i < 32; i++)
        printf("%02x", digest[i]);
    printf("\n");
    char signer[128];
    int sig = sol_verify_signature(file, &h, signer, sizeof(signer));
    if (sig == SOL_SIG_NONE)
        printf("signature: none (unsigned)\n");
    else if (sig == SOL_SIG_TRUSTED)
        printf("signature: valid (trusted key: %s)\n", signer);
    else
        printf("signature: invalid or untrusted signer\n");
    if (!ok)
        return 2;
    return 0;
}

static int cmd_trust(const char *pem)
{
    char dir[PATH_MAX], dst[PATH_MAX];
    snprintf(dir, sizeof(dir), "%s/.config/soil/trust", home_dir());
    mkdir_p(dir, 0755);
    const char *base = strrchr(pem, '/');
    base = base ? base + 1 : pem;
    snprintf(dst, sizeof(dst), "%s/%s", dir, base);
    if (copy_file(pem, dst, 0644) != 0)
        die("cannot copy %s", pem);
    printf("soil: trusted key installed: %s\n", dst);
    return 0;
}

static int cmd_untrust(const char *name)
{
    char dst[PATH_MAX];
    snprintf(dst, sizeof(dst), "%s/.config/soil/trust/%s", home_dir(), name);
    if (unlink(dst) != 0)
        die("cannot remove %s", dst);
    printf("soil: trust removed: %s\n", name);
    return 0;
}

static int cmd_docs(void)
{
    printf("soil docs: %s\n", SOIL_DOCS);
    char *args[] = { "pcmanfm", SOIL_DOCS, NULL };
    spawn_detached(args);
    return 0;
}

static void usage(void)
{
    printf(
        "soil - Soil package manager and launcher\n"
        "\n"
        "usage:\n"
        "  soil FILE.sol [args]       run a package (also via double-click)\n"
        "  soil run FILE.sol          run a package\n"
        "  soil install FILE.sol      install a package for this session\n"
        "  soil launch ID [args]      launch an installed package\n"
        "  soil list                  list installed packages\n"
        "  soil remove ID             uninstall a package\n"
        "  soil info FILE.sol|ID      show header and manifest\n"
        "  soil verify FILE.sol       check digest and signature\n"
        "  soil trust KEY.pem         trust a publisher key\n"
        "  soil untrust NAME.pem      remove a trusted key\n"
        "  soil docs                  open the SDK documentation\n"
        "\n"
        "environment:\n"
        "  SOIL_STRICT=1              refuse unsigned/untrusted packages\n"
        "  SOIL_ALLOW_UNTRUSTED=1     silence the untrusted warning\n"
        "  SOIL_DEBUG=1               enable WebKit inspector/console output\n"
        "\n"
        "packages live in ~/.local/share/soil\n");
}

int main(int argc, char **argv)
{
    setvbuf(stdout, NULL, _IOLBF, 0);
    if (argc < 2) {
        usage();
        return 0;
    }

    if (sol_is_package_file(argv[1])) {
        int strict = 0;
        return cmd_run(argv[1], strict, getenv("SOIL_DEBUG") != NULL, argc - 2, argv + 2);
    }

    const char *cmd = argv[1];
    if (!strcmp(cmd, "help") || !strcmp(cmd, "--help") || !strcmp(cmd, "-h")) {
        usage();
        return 0;
    }
    if (!strcmp(cmd, "run")) {
        int strict = 0;
        int i = 2;
        for (; i < argc; i++) {
            if (!strcmp(argv[i], "--strict"))
                strict = 1;
            else if (!strcmp(argv[i], "--debug"))
                setenv("SOIL_DEBUG", "1", 1);
            else
                break;
        }
        if (i >= argc)
            die("usage: soil run FILE.sol");
        return cmd_run(argv[i], strict, getenv("SOIL_DEBUG") != NULL, argc - i - 1,
                       argv + i + 1);
    }
    if (!strcmp(cmd, "install")) {
        if (argc < 3)
            die("usage: soil install FILE.sol");
        return cmd_install(argv[2]);
    }
    if (!strcmp(cmd, "launch")) {
        if (argc < 3)
            die("usage: soil launch ID");
        return cmd_launch(argv[2], argc - 3, argv + 3);
    }
    if (!strcmp(cmd, "list"))
        return cmd_list();
    if (!strcmp(cmd, "remove")) {
        if (argc < 3)
            die("usage: soil remove ID");
        return cmd_remove(argv[2]);
    }
    if (!strcmp(cmd, "info")) {
        if (argc < 3)
            die("usage: soil info FILE.sol|ID");
        if (sol_is_package_file(argv[2]))
            return cmd_info(argv[2]);
        char path[PATH_MAX];
        if (find_installed(argv[2], path, sizeof(path)) != 0)
            die("%s is not installed", argv[2]);
        return cmd_info(path);
    }
    if (!strcmp(cmd, "verify")) {
        if (argc < 3)
            die("usage: soil verify FILE.sol");
        return cmd_verify(argv[2]);
    }
    if (!strcmp(cmd, "trust")) {
        if (argc < 3)
            die("usage: soil trust KEY.pem");
        return cmd_trust(argv[2]);
    }
    if (!strcmp(cmd, "untrust")) {
        if (argc < 3)
            die("usage: soil untrust NAME.pem");
        return cmd_untrust(argv[2]);
    }
    if (!strcmp(cmd, "docs"))
        return cmd_docs();
    if (!strcmp(cmd, "_mount") && geteuid() == 0) {
        if (argc < 4)
            return 1;
        return do_mount(argv[2], argv[3]);
    }
    if (!strcmp(cmd, "_umount") && geteuid() == 0) {
        if (argc < 3)
            return 1;
        return do_umount(argv[2]);
    }

    fprintf(stderr, "soil: unknown command: %s\n\n", cmd);
    usage();
    return 1;
}
