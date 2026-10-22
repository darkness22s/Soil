#define _GNU_SOURCE
#include <gtk/gtk.h>
#include <webkit2/webkit2.h>
#include <jsc/jsc.h>
#include <libnotify/notify.h>

#include <errno.h>
#include <limits.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include "json.h"

static GtkWidget *g_win;
static WebKitWebView *g_view;
static char *g_root;
static char *g_id;
static char *g_data_dir;
static char *g_app_name;
static char *g_app_version;
static jval *g_manifest;

static void die(const char *msg)
{
    fprintf(stderr, "soil-app: %s\n", msg);
    exit(2);
}

static int perm_ok(const char *perm)
{
    jval *p = json_obj_get(g_manifest, "permissions");
    if (!p || p->type != JSON_ARR)
        return 0;
    for (size_t i = 0; i < p->n; i++)
        if (p->items[i]->type == JSON_STR && !strcmp(p->items[i]->str, perm))
            return 1;
    return 0;
}

static int safe_rel(const char *p)
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

static void js_escape_single(jbuf *b, const char *s)
{
    for (const char *p = s; *p; p++) {
        if (*p == '\\' || *p == '\'')
            jbuf_putc(b, '\\');
        if (*p == '\n') {
            jbuf_puts(b, "\\n");
            continue;
        }
        if (*p == '\r') {
            jbuf_puts(b, "\\r");
            continue;
        }
        jbuf_putc(b, *p);
    }
}

static void send_reply(long id, int ok, const char *payload)
{
    jbuf js;
    jbuf_init(&js);
    jbuf_printf(&js, "__soilReply(%ld,%s,'", id, ok ? "true" : "false");
    js_escape_single(&js, payload ? payload : "null");
    jbuf_puts(&js, "');");
    webkit_web_view_evaluate_javascript(g_view, js.p, -1, NULL, NULL, NULL, NULL, NULL);
    jbuf_free(&js);
}

static void reply_json(long id, jbuf *b)
{
    send_reply(id, 1, b->p ? b->p : "null");
}

static void reply_null(long id)
{
    send_reply(id, 1, "null");
}

static void reply_error(long id, const char *fmt, ...)
{
    char msg[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);
    send_reply(id, 0, msg);
}

static void reply_error_perm(long id, const char *perm)
{
    reply_error(id, "permission denied: %s", perm);
}

static const char *mime_for(const char *path)
{
    const char *ext = strrchr(path, '.');
    if (!ext)
        return "application/octet-stream";
    if (!g_ascii_strcasecmp(ext, ".html") || !g_ascii_strcasecmp(ext, ".htm"))
        return "text/html";
    if (!g_ascii_strcasecmp(ext, ".js") || !g_ascii_strcasecmp(ext, ".mjs"))
        return "text/javascript";
    if (!g_ascii_strcasecmp(ext, ".css"))
        return "text/css";
    if (!g_ascii_strcasecmp(ext, ".json"))
        return "application/json";
    if (!g_ascii_strcasecmp(ext, ".png"))
        return "image/png";
    if (!g_ascii_strcasecmp(ext, ".jpg") || !g_ascii_strcasecmp(ext, ".jpeg"))
        return "image/jpeg";
    if (!g_ascii_strcasecmp(ext, ".gif"))
        return "image/gif";
    if (!g_ascii_strcasecmp(ext, ".svg"))
        return "image/svg+xml";
    if (!g_ascii_strcasecmp(ext, ".webp"))
        return "image/webp";
    if (!g_ascii_strcasecmp(ext, ".ico"))
        return "image/x-icon";
    if (!g_ascii_strcasecmp(ext, ".woff"))
        return "font/woff";
    if (!g_ascii_strcasecmp(ext, ".woff2"))
        return "font/woff2";
    if (!g_ascii_strcasecmp(ext, ".ttf"))
        return "font/ttf";
    if (!g_ascii_strcasecmp(ext, ".wasm"))
        return "application/wasm";
    if (!g_ascii_strcasecmp(ext, ".txt") || !g_ascii_strcasecmp(ext, ".md"))
        return "text/plain";
    if (!g_ascii_strcasecmp(ext, ".xml"))
        return "application/xml";
    if (!g_ascii_strcasecmp(ext, ".mp3"))
        return "audio/mpeg";
    if (!g_ascii_strcasecmp(ext, ".mp4"))
        return "video/mp4";
    return "application/octet-stream";
}

static void percent_decode(char *s)
{
    char *w = s;
    for (char *p = s; *p; p++) {
        if (*p == '%' && p[1] && p[2]) {
            int hi = g_ascii_xdigit_value(p[1]);
            int lo = g_ascii_xdigit_value(p[2]);
            if (hi >= 0 && lo >= 0) {
                *w++ = (char)((hi << 4) | lo);
                p += 2;
                continue;
            }
        }
        *w++ = *p;
    }
    *w = 0;
}

static void uri_request_cb(WebKitURISchemeRequest *req, gpointer user_data)
{
    (void)user_data;
    const char *uri = webkit_uri_scheme_request_get_uri(req);
    const char *p = uri;
    if (!strncmp(p, "soil://", 7))
        p += 7;
    while (*p && *p != '/' && *p != '?')
        p++;
    if (*p == '/')
        p++;
    char rel[PATH_MAX];
    snprintf(rel, sizeof(rel), "%s", p);
    char *q = strchr(rel, '?');
    if (q)
        *q = 0;
    q = strchr(rel, '#');
    if (q)
        *q = 0;
    percent_decode(rel);

    if (!rel[0]) {
        const char *entry = json_str_get(g_manifest, "entry");
        if (!entry)
            entry = "index.html";
        snprintf(rel, sizeof(rel), "%s", entry);
    }
    if (!safe_rel(rel)) {
        GError *e = g_error_new_literal(G_IO_ERROR, G_IO_ERROR_NOT_FOUND, "not found");
        webkit_uri_scheme_request_finish_error(req, e);
        g_error_free(e);
        return;
    }

    char full[PATH_MAX * 2];
    snprintf(full, sizeof(full), "%s/%s", g_root, rel);
    char resolved[PATH_MAX];
    if (!realpath(full, resolved) || strncmp(resolved, g_root, strlen(g_root)) != 0) {
        GError *e = g_error_new_literal(G_IO_ERROR, G_IO_ERROR_NOT_FOUND, "not found");
        webkit_uri_scheme_request_finish_error(req, e);
        g_error_free(e);
        return;
    }

    gchar *data = NULL;
    gsize len = 0;
    if (!g_file_get_contents(resolved, &data, &len, NULL)) {
        GError *e = g_error_new_literal(G_IO_ERROR, G_IO_ERROR_NOT_FOUND, "not found");
        webkit_uri_scheme_request_finish_error(req, e);
        g_error_free(e);
        return;
    }
    GInputStream *stream = g_memory_input_stream_new_from_data(data, (gssize)len, g_free);
    webkit_uri_scheme_request_finish(req, stream, (gssize)len, mime_for(resolved));
    g_object_unref(stream);
}

/* ---------- store ---------- */

static void store_path(char *out, size_t n)
{
    snprintf(out, n, "%s/store.json", g_data_dir);
}

static jval *store_load(void)
{
    char path[PATH_MAX];
    store_path(path, sizeof(path));
    gchar *data = NULL;
    gsize len = 0;
    if (!g_file_get_contents(path, &data, &len, NULL)) {
        jval *empty = json_parse("{}", 2, NULL, 0);
        return empty;
    }
    jval *v = json_parse(data, len, NULL, 0);
    g_free(data);
    if (!v || v->type != JSON_OBJ) {
        json_free(v);
        return json_parse("{}", 2, NULL, 0);
    }
    return v;
}

static int store_save(jval *obj)
{
    char path[PATH_MAX];
    store_path(path, sizeof(path));
    jbuf b;
    jbuf_init(&b);
    json_write(&b, obj);
    FILE *f = fopen(path, "w");
    if (!f) {
        jbuf_free(&b);
        return -1;
    }
    fputs(b.p, f);
    fclose(f);
    jbuf_free(&b);
    return 0;
}

static jval *store_lookup(jval *obj, const char *key)
{
    return json_obj_get(obj, key);
}

static void store_set(jval *obj, const char *key, const char *value)
{
    for (size_t i = 0; i < obj->n; i++) {
        if (!strcmp(obj->keys[i], key)) {
            json_free(obj->vals[i]);
            jval *v = calloc(1, sizeof(*v));
            v->type = JSON_STR;
            v->str = strdup(value);
            obj->vals[i] = v;
            return;
        }
    }
    obj->keys = realloc(obj->keys, (obj->n + 1) * sizeof(char *));
    obj->vals = realloc(obj->vals, (obj->n + 1) * sizeof(jval *));
    obj->keys[obj->n] = strdup(key);
    jval *v = calloc(1, sizeof(*v));
    v->type = JSON_STR;
    v->str = strdup(value);
    obj->vals[obj->n] = v;
    obj->n++;
}

static void store_del(jval *obj, const char *key)
{
    for (size_t i = 0; i < obj->n; i++) {
        if (!strcmp(obj->keys[i], key)) {
            free(obj->keys[i]);
            json_free(obj->vals[i]);
            memmove(obj->keys + i, obj->keys + i + 1, (obj->n - i - 1) * sizeof(char *));
            memmove(obj->vals + i, obj->vals + i + 1, (obj->n - i - 1) * sizeof(jval *));
            obj->n--;
            return;
        }
    }
}

/* ---------- dialogs ---------- */

static void add_filters(GtkFileChooser *fc, jval *filters)
{
    if (!filters || filters->type != JSON_ARR)
        return;
    for (size_t i = 0; i < filters->n; i++) {
        jval *f = filters->items[i];
        if (f->type != JSON_OBJ)
            continue;
        const char *name = json_str_get(f, "name");
        GtkFileFilter *ff = gtk_file_filter_new();
        gtk_file_filter_set_name(ff, name ? name : "Files");
        jval *pats = json_obj_get(f, "patterns");
        if (pats && pats->type == JSON_ARR) {
            for (size_t k = 0; k < pats->n; k++)
                if (pats->items[k]->type == JSON_STR)
                    gtk_file_filter_add_pattern(ff, pats->items[k]->str);
        } else {
            gtk_file_filter_add_pattern(ff, "*");
        }
        gtk_file_chooser_add_filter(fc, ff);
    }
}

static void do_dialog(long id, jval *params, int save)
{
    const char *title = params ? json_str_get(params, "title") : NULL;
    GtkFileChooserAction action = save ? GTK_FILE_CHOOSER_ACTION_SAVE
                                       : GTK_FILE_CHOOSER_ACTION_OPEN;
    const char *accept = save ? "_Save" : "_Open";
    GtkWidget *dlg = gtk_file_chooser_dialog_new(title ? title : "Select File",
                                                 GTK_WINDOW(g_win), action,
                                                 "_Cancel", GTK_RESPONSE_CANCEL,
                                                 accept, GTK_RESPONSE_ACCEPT, NULL);
    add_filters(GTK_FILE_CHOOSER(dlg), params ? json_obj_get(params, "filters") : NULL);
    if (save) {
        const char *def = params ? json_str_get(params, "defaultName") : NULL;
        if (def)
            gtk_file_chooser_set_current_name(GTK_FILE_CHOOSER(dlg), def);
    }
    gint resp = gtk_dialog_run(GTK_DIALOG(dlg));
    if (resp == GTK_RESPONSE_ACCEPT) {
        char *filename = gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(dlg));
        jbuf b;
        jbuf_init(&b);
        jbuf_puts(&b, "{\"path\":");
        json_escape(&b, filename ? filename : "");
        jbuf_putc(&b, '}');
        reply_json(id, &b);
        jbuf_free(&b);
        g_free(filename);
    } else {
        reply_null(id);
    }
    gtk_widget_destroy(dlg);
}

/* ---------- exec ---------- */

static int exec_allowed(const char *path)
{
    jval *ex = json_obj_get(g_manifest, "exec");
    if (!ex || ex->type != JSON_ARR)
        return 0;
    for (size_t i = 0; i < ex->n; i++)
        if (ex->items[i]->type == JSON_STR && !strcmp(ex->items[i]->str, path))
            return 1;
    return 0;
}

static void do_exec(long id, jval *params)
{
    if (!perm_ok("exec")) {
        reply_error_perm(id, "exec");
        return;
    }
    const char *path = params ? json_str_get(params, "path") : NULL;
    if (!path || !safe_rel(path) || !exec_allowed(path)) {
        reply_error(id, "exec path not allowed");
        return;
    }
    char full[PATH_MAX * 2];
    snprintf(full, sizeof(full), "%s/%s", g_root, path);
    char resolved[PATH_MAX];
    if (!realpath(full, resolved) || strncmp(resolved, g_root, strlen(g_root)) != 0 ||
        access(resolved, X_OK) != 0) {
        reply_error(id, "cannot execute %s", path);
        return;
    }
    char *argv[64];
    int argc = 0;
    argv[argc++] = resolved;
    jval *args = params ? json_obj_get(params, "args") : NULL;
    if (args && args->type == JSON_ARR) {
        for (size_t i = 0; i < args->n && argc < 62; i++)
            argv[argc++] = (args->items[i]->type == JSON_STR) ? args->items[i]->str : "";
    }
    argv[argc] = NULL;

    char **envp = g_get_environ();
    envp = g_environ_setenv(envp, "SOIL_APP_ID", g_id, TRUE);
    envp = g_environ_setenv(envp, "SOIL_VERSION", g_app_version ? g_app_version : "", TRUE);
    envp = g_environ_setenv(envp, "SOIL_APPDIR", g_root, TRUE);
    envp = g_environ_setenv(envp, "SOIL_DATA", g_data_dir, TRUE);
    GPid pid = 0;
    GError *err = NULL;
    gboolean ok = g_spawn_async(g_root, argv, envp, G_SPAWN_DO_NOT_REAP_CHILD,
                                NULL, NULL, &pid, &err);
    g_strfreev(envp);
    if (!ok) {
        reply_error(id, "exec failed: %s", err ? err->message : "unknown");
        if (err) g_error_free(err);
        return;
    }
    jbuf b;
    jbuf_init(&b);
    jbuf_printf(&b, "{\"pid\":%d}", (int)pid);
    reply_json(id, &b);
    jbuf_free(&b);
}

/* ---------- dispatch ---------- */

static void handle_message(WebKitUserContentManager *ucm, WebKitJavascriptResult *res,
                           gpointer user_data)
{
    (void)ucm;
    (void)user_data;
    JSCValue *val = webkit_javascript_result_get_js_value(res);
    char *text = jsc_value_to_string(val);
    if (!text)
        return;
    char err[128];
    jval *msg = json_parse(text, strlen(text), err, sizeof(err));
    g_free(text);
    if (!msg || msg->type != JSON_OBJ) {
        json_free(msg);
        return;
    }
    jval *jid = json_obj_get(msg, "id");
    const char *method = json_str_get(msg, "method");
    jval *params = json_obj_get(msg, "params");
    long id = (jid && jid->type == JSON_NUM) ? (long)jid->num : 0;
    if (!method) {
        reply_error(id, "bad request");
        json_free(msg);
        return;
    }

    if (!strcmp(method, "app.info")) {
        jbuf b;
        jbuf_init(&b);
        jbuf_puts(&b, "{\"id\":");
        json_escape(&b, g_id);
        jbuf_puts(&b, ",\"name\":");
        json_escape(&b, g_app_name ? g_app_name : g_id);
        jbuf_puts(&b, ",\"version\":");
        json_escape(&b, g_app_version ? g_app_version : "");
        jbuf_puts(&b, ",\"kind\":");
        json_escape(&b, json_str_get(g_manifest, "kind") ?
                    json_str_get(g_manifest, "kind") : "webapp");
        jbuf_puts(&b, ",\"dataDir\":");
        json_escape(&b, g_data_dir);
        jbuf_putc(&b, '}');
        reply_json(id, &b);
        jbuf_free(&b);
    } else if (!strcmp(method, "window.setTitle")) {
        const char *t = params ? json_str_get(params, "title") : NULL;
        gtk_window_set_title(GTK_WINDOW(g_win), t ? t : (g_app_name ? g_app_name : ""));
        reply_null(id);
    } else if (!strcmp(method, "window.close")) {
        gtk_window_close(GTK_WINDOW(g_win));
        reply_null(id);
    } else if (!strcmp(method, "window.setSize")) {
        int w = params ? json_int_get(params, "width", 900) : 900;
        int h = params ? json_int_get(params, "height", 640) : 640;
        if (w < 320) w = 320;
        if (h < 240) h = 240;
        gtk_window_resize(GTK_WINDOW(g_win), w, h);
        reply_null(id);
    } else if (!strcmp(method, "window.minimize")) {
        gtk_window_iconify(GTK_WINDOW(g_win));
        reply_null(id);
    } else if (!strcmp(method, "dialog.openFile")) {
        if (!perm_ok("dialogs")) { reply_error_perm(id, "dialogs"); }
        else do_dialog(id, params, 0);
    } else if (!strcmp(method, "dialog.saveFile")) {
        if (!perm_ok("dialogs")) { reply_error_perm(id, "dialogs"); }
        else do_dialog(id, params, 1);
    } else if (!strcmp(method, "clipboard.read")) {
        if (!perm_ok("clipboard")) {
            reply_error_perm(id, "clipboard");
        } else {
            GtkClipboard *cb = gtk_clipboard_get(GDK_SELECTION_CLIPBOARD);
            char *t = gtk_clipboard_wait_for_text(cb);
            jbuf b;
            jbuf_init(&b);
            jbuf_puts(&b, "{\"text\":");
            json_escape(&b, t ? t : "");
            jbuf_putc(&b, '}');
            reply_json(id, &b);
            jbuf_free(&b);
            g_free(t);
        }
    } else if (!strcmp(method, "clipboard.write")) {
        if (!perm_ok("clipboard")) {
            reply_error_perm(id, "clipboard");
        } else {
            const char *t = params ? json_str_get(params, "text") : "";
            GtkClipboard *cb = gtk_clipboard_get(GDK_SELECTION_CLIPBOARD);
            gtk_clipboard_set_text(cb, t ? t : "", -1);
            gtk_clipboard_store(cb);
            reply_null(id);
        }
    } else if (!strcmp(method, "shell.openExternal")) {
        if (!perm_ok("openExternal")) {
            reply_error_perm(id, "openExternal");
        } else {
            const char *url = params ? json_str_get(params, "url") : NULL;
            if (!url || (strncmp(url, "http://", 7) && strncmp(url, "https://", 8) &&
                         strncmp(url, "mailto:", 7))) {
                reply_error(id, "unsupported URL");
            } else {
                char *args[] = { "/usr/local/bin/soil-browser", (char *)url, NULL };
                g_spawn_async(NULL, args, NULL, G_SPAWN_DO_NOT_REAP_CHILD, NULL, NULL,
                              NULL, NULL);
                reply_null(id);
            }
        }
    } else if (!strcmp(method, "notify")) {
        if (!perm_ok("notifications")) {
            reply_error_perm(id, "notifications");
        } else {
            const char *title = params ? json_str_get(params, "title") : "Soil";
            const char *body = params ? json_str_get(params, "body") : "";
            NotifyNotification *n = notify_notification_new(title ? title : "Soil",
                                                            body ? body : "", NULL);
            GError *e = NULL;
            if (!n || !notify_notification_show(n, &e)) {
                reply_error(id, "notification failed: %s", e ? e->message : "unknown");
                if (e) g_error_free(e);
            } else {
                reply_null(id);
            }
            if (n) g_object_unref(n);
        }
    } else if (!strcmp(method, "store.get")) {
        const char *key = params ? json_str_get(params, "key") : NULL;
        if (!key) {
            reply_error(id, "store.get: missing key");
        } else {
            jval *obj = store_load();
            jval *v = store_lookup(obj, key);
            jbuf b;
            jbuf_init(&b);
            jbuf_puts(&b, "{\"value\":");
            if (v)
                json_write(&b, v);
            else
                jbuf_puts(&b, "null");
            jbuf_putc(&b, '}');
            reply_json(id, &b);
            jbuf_free(&b);
            json_free(obj);
        }
    } else if (!strcmp(method, "store.set")) {
        const char *key = params ? json_str_get(params, "key") : NULL;
        const char *value = params ? json_str_get(params, "value") : NULL;
        if (!key || !value) {
            reply_error(id, "store.set: missing key/value");
        } else {
            jval *obj = store_load();
            store_set(obj, key, value);
            store_save(obj);
            json_free(obj);
            reply_null(id);
        }
    } else if (!strcmp(method, "store.delete")) {
        const char *key = params ? json_str_get(params, "key") : NULL;
        if (!key) {
            reply_error(id, "store.delete: missing key");
        } else {
            jval *obj = store_load();
            store_del(obj, key);
            store_save(obj);
            json_free(obj);
            reply_null(id);
        }
    } else if (!strcmp(method, "store.keys")) {
        jval *obj = store_load();
        jbuf b;
        jbuf_init(&b);
        jbuf_puts(&b, "{\"keys\":[");
        for (size_t i = 0; i < obj->n; i++) {
            if (i) jbuf_putc(&b, ',');
            json_escape(&b, obj->keys[i]);
        }
        jbuf_puts(&b, "]}");
        reply_json(id, &b);
        jbuf_free(&b);
        json_free(obj);
    } else if (!strcmp(method, "exec.run")) {
        do_exec(id, params);
    } else {
        reply_error(id, "unknown method: %s", method);
    }
    json_free(msg);
}

static const char *BOOTSTRAP_JS =
    "(function(){"
    "if(window.soil&&window.soil.__v===1)return;"
    "var pending={},seq=0;"
    "function call(method,params){return new Promise(function(resolve,reject){"
    "var id=++seq;pending[id]={resolve:resolve,reject:reject};"
    "try{window.webkit.messageHandlers.soil.postMessage(JSON.stringify({id:id,method:method,params:params||{}}));}"
    "catch(e){delete pending[id];reject(e);}});}"
    "window.__soilReply=function(id,ok,payload){var p=pending[id];if(!p)return;delete pending[id];"
    "if(ok){try{p.resolve(JSON.parse(payload));}catch(e){p.reject(e);}}"
    "else{p.reject(new Error(payload||'soil error'));}};"
    "window.soil={__v:1,"
    "app:{info:function(){return call('app.info');}},"
    "window:{setTitle:function(t){return call('window.setTitle',{title:String(t)});},"
    "close:function(){return call('window.close');},"
    "setSize:function(w,h){return call('window.setSize',{width:w,height:h});},"
    "minimize:function(){return call('window.minimize');}},"
    "dialog:{openFile:function(o){return call('dialog.openFile',o||{});},"
    "saveFile:function(o){return call('dialog.saveFile',o||{});}},"
    "clipboard:{read:function(){return call('clipboard.read');},"
    "write:function(t){return call('clipboard.write',{text:String(t)});}},"
    "shell:{openExternal:function(u){return call('shell.openExternal',{url:String(u)});}},"
    "notify:function(t,b){return call('notify',{title:String(t),body:String(b||'')});},"
    "store:{get:function(k){return call('store.get',{key:String(k)});},"
    "set:function(k,v){return call('store.set',{key:String(k),value:String(v)});},"
    "delete:function(k){return call('store.delete',{key:String(k)});},"
    "keys:function(){return call('store.keys');}},"
    "exec:{run:function(p,a){return call('exec.run',{path:String(p),args:a||[]});}}"
    "};})();";

static void on_destroy(GtkWidget *w, gpointer data)
{
    (void)w;
    (void)data;
    gtk_main_quit();
}

static void on_title_changed(GObject *obj, GParamSpec *pspec, gpointer data)
{
    (void)pspec;
    (void)data;
    const char *t = webkit_web_view_get_title(WEBKIT_WEB_VIEW(obj));
    if (t && *t)
        gtk_window_set_title(GTK_WINDOW(g_win), t);
}

int main(int argc, char **argv)
{
    const char *root = NULL, *manifest_path = NULL, *id = NULL, *data_dir = NULL;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--root") && i + 1 < argc)
            root = argv[++i];
        else if (!strcmp(argv[i], "--manifest") && i + 1 < argc)
            manifest_path = argv[++i];
        else if (!strcmp(argv[i], "--id") && i + 1 < argc)
            id = argv[++i];
        else if (!strcmp(argv[i], "--data") && i + 1 < argc)
            data_dir = argv[++i];
    }
    if (!root || !manifest_path || !id || !data_dir)
        die("usage: soil-app --root DIR --manifest FILE --id ID --data DIR");

    if (!getenv("WEBKIT_DISABLE_SANDBOX_THIS_IS_DANGEROUS"))
        setenv("WEBKIT_DISABLE_SANDBOX_THIS_IS_DANGEROUS", "1", 0);
    if (!getenv("WEBKIT_DISABLE_COMPOSITING_MODE"))
        setenv("WEBKIT_DISABLE_COMPOSITING_MODE", "1", 0);
    if (!getenv("WEBKIT_DISABLE_DMABUF_RENDERER"))
        setenv("WEBKIT_DISABLE_DMABUF_RENDERER", "1", 0);
    if (!getenv("WEBKIT_SKIA_ENABLE_CPU_RENDERING"))
        setenv("WEBKIT_SKIA_ENABLE_CPU_RENDERING", "1", 0);
    if (!getenv("JSC_useJIT"))
        setenv("JSC_useJIT", "0", 0);

    gchar *manifest_raw = NULL;
    gsize manifest_len = 0;
    if (!g_file_get_contents(manifest_path, &manifest_raw, &manifest_len, NULL))
        die("cannot read manifest");
    char err[128];
    g_manifest = json_parse(manifest_raw, manifest_len, err, sizeof(err));
    if (!g_manifest)
        die(err);
    g_free(manifest_raw);

    char resolved[PATH_MAX];
    if (!realpath(root, resolved))
        die("cannot resolve app root");
    g_root = g_strdup(resolved);
    g_id = g_strdup(id);
    g_data_dir = g_strdup(data_dir);
    const char *name = json_str_get(g_manifest, "name");
    g_app_name = g_strdup(name ? name : id);
    const char *ver = json_str_get(g_manifest, "version");
    g_app_version = g_strdup(ver ? ver : "");
    mkdir(g_data_dir, 0755);

    gtk_init(&argc, &argv);
    g_set_prgname("soil-app");
    g_set_application_name(g_app_name);

    g_win = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    jval *win = json_obj_get(g_manifest, "window");
    int w = win ? json_int_get(win, "width", 900) : 900;
    int h = win ? json_int_get(win, "height", 640) : 640;
    int resizable = win ? json_bool_get(win, "resizable", 1) : 1;
    gtk_window_set_default_size(GTK_WINDOW(g_win), w, h);
    gtk_window_set_resizable(GTK_WINDOW(g_win), resizable);
    gtk_window_set_title(GTK_WINDOW(g_win), g_app_name);
    gtk_window_set_role(GTK_WINDOW(g_win), "soil-app");
    const char *icon = json_str_get(g_manifest, "icon");
    if (icon && safe_rel(icon)) {
        char ip[PATH_MAX * 2];
        snprintf(ip, sizeof(ip), "%s/%s", g_root, icon);
        gtk_window_set_icon_from_file(GTK_WINDOW(g_win), ip, NULL);
    }
    g_signal_connect(g_win, "destroy", G_CALLBACK(on_destroy), NULL);

    WebKitWebContext *ctx = webkit_web_context_get_default();
    webkit_web_context_set_cache_model(ctx, WEBKIT_CACHE_MODEL_DOCUMENT_VIEWER);
    webkit_web_context_set_sandbox_enabled(ctx, FALSE);
    WebKitSecurityManager *sm = webkit_web_context_get_security_manager(ctx);
    webkit_security_manager_register_uri_scheme_as_secure(sm, "soil");
    webkit_security_manager_register_uri_scheme_as_cors_enabled(sm, "soil");
    webkit_web_context_register_uri_scheme(ctx, "soil", uri_request_cb, NULL, NULL);

    WebKitUserContentManager *ucm = webkit_user_content_manager_new();
    webkit_user_content_manager_register_script_message_handler(ucm, "soil");
    g_signal_connect(ucm, "script-message-received::soil",
                     G_CALLBACK(handle_message), NULL);
    WebKitUserScript *us = webkit_user_script_new(
        BOOTSTRAP_JS, WEBKIT_USER_CONTENT_INJECT_ALL_FRAMES,
        WEBKIT_USER_SCRIPT_INJECT_AT_DOCUMENT_START, NULL, NULL);
    webkit_user_content_manager_add_script(ucm, us);
    webkit_user_script_unref(us);

    g_view = WEBKIT_WEB_VIEW(webkit_web_view_new_with_user_content_manager(ucm));
    gtk_container_add(GTK_CONTAINER(g_win), GTK_WIDGET(g_view));
    g_signal_connect(g_view, "notify::title", G_CALLBACK(on_title_changed), NULL);

    WebKitSettings *st = webkit_web_view_get_settings(g_view);
    webkit_settings_set_enable_javascript(st, TRUE);
    webkit_settings_set_enable_webgl(st, FALSE);
    webkit_settings_set_enable_webaudio(st, FALSE);
    webkit_settings_set_enable_mediasource(st, FALSE);
    webkit_settings_set_enable_media(st, FALSE);
    webkit_settings_set_enable_smooth_scrolling(st, TRUE);
    webkit_settings_set_javascript_can_open_windows_automatically(st, FALSE);
    webkit_settings_set_enable_page_cache(st, FALSE);
    webkit_settings_set_enable_accelerated_2d_canvas(st, FALSE);
    webkit_settings_set_hardware_acceleration_policy(st, WEBKIT_HARDWARE_ACCELERATION_POLICY_NEVER);
    webkit_settings_set_enable_write_console_messages_to_stdout(st, TRUE);
    webkit_settings_set_enable_developer_extras(st, getenv("SOIL_DEBUG") ? TRUE : FALSE);

    notify_init("soil-app");

    gtk_widget_show_all(g_win);
    gtk_widget_grab_focus(GTK_WIDGET(g_view));

    const char *entry = json_str_get(g_manifest, "entry");
    if (!entry)
        entry = "index.html";
    char *escaped = g_uri_escape_string(entry, "/", TRUE);
    char uri[PATH_MAX * 2];
    snprintf(uri, sizeof(uri), "soil://app/%s", escaped);
    g_free(escaped);
    webkit_web_view_load_uri(g_view, uri);

    gtk_main();
    notify_uninit();
    return 0;
}
