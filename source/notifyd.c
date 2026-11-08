/* ccos-notifyd - the notification server for apps (org.freedesktop.Notifications)
 *
 * Apps like Firefox, Discord or Thunderbird send "Notify" over D-Bus. We hand
 * each one to the ccos desktop, which shows it as a toast in the corner:
 *   /run/ccos/toast       we write   "N <id>\t<app id>\t<title>\t<body>\n"
 *                                    "C <id>\n"  (the app took it back)
 *   /run/ccos/toast-act   we read    "<id>\n"    (someone clicked it)
 * A click becomes ActionInvoked(id, "default"), which brings the app forward.
 * D-Bus starts this program the first time an app wants it.
 *
 * Build: gcc -O2 -o ccos-notifyd notifyd.c $(pkg-config --cflags --libs gio-2.0 gio-unix-2.0)
 */
#include <gio/gio.h>
#include <gio/gunixinputstream.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>

#define TOAST_FIFO "/run/ccos/toast"
#define ACT_FIFO   "/run/ccos/toast-act"

static const char *xml =
    "<node><interface name='org.freedesktop.Notifications'>"
    "<method name='Notify'><arg type='s' direction='in'/><arg type='u' direction='in'/><arg type='s' direction='in'/>"
    "<arg type='s' direction='in'/><arg type='s' direction='in'/><arg type='as' direction='in'/><arg type='a{sv}' direction='in'/>"
    "<arg type='i' direction='in'/><arg type='u' direction='out'/></method>"
    "<method name='CloseNotification'><arg type='u' direction='in'/></method>"
    "<method name='GetCapabilities'><arg type='as' direction='out'/></method>"
    "<method name='GetServerInformation'><arg type='s' direction='out'/><arg type='s' direction='out'/>"
    "<arg type='s' direction='out'/><arg type='s' direction='out'/></method>"
    "<signal name='NotificationClosed'><arg type='u'/><arg type='u'/></signal>"
    "<signal name='ActionInvoked'><arg type='u'/><arg type='s'/></signal>"
    "<signal name='ActivationToken'><arg type='u'/><arg type='s'/></signal>"
    "</interface></node>";

static GDBusConnection *bus;
static guint32 next_id = 1;

/* tabs and newlines would break our one-line messages; markup like <b> too */
static void clean(const char *in, char *out, size_t n) {
    size_t o = 0; int tag = 0;
    for (const char *p = in ? in : ""; *p && o + 1 < n; p++) {
        if (*p == '<') { tag = 1; continue; }
        if (tag) { if (*p == '>') tag = 0; continue; }
        if (*p == '&') {                                   /* the common entities */
            if (!strncmp(p, "&amp;", 5)) { out[o++] = '&'; p += 4; continue; }
            if (!strncmp(p, "&lt;", 4)) { out[o++] = '<'; p += 3; continue; }
            if (!strncmp(p, "&gt;", 4)) { out[o++] = '>'; p += 3; continue; }
            if (!strncmp(p, "&quot;", 6)) { out[o++] = '"'; p += 5; continue; }
            if (!strncmp(p, "&apos;", 6)) { out[o++] = '\''; p += 5; continue; }
        }
        out[o++] = (*p == '\t' || *p == '\n' || *p == '\r') ? ' ' : *p;
    }
    out[o] = 0;
}

static void to_desktop(const char *line) {
    int fd = open(TOAST_FIFO, O_WRONLY | O_NONBLOCK | O_CLOEXEC);   /* no desktop: drop it */
    if (fd < 0) return;
    if (write(fd, line, strlen(line)) < 0) { /* full: drop it */ }
    close(fd);
}

static void method_call(GDBusConnection *c, const gchar *sender, const gchar *path, const gchar *iface,
                        const gchar *method, GVariant *p, GDBusMethodInvocation *inv, gpointer ud) {
    (void)c; (void)sender; (void)path; (void)iface; (void)ud;
    if (!strcmp(method, "Notify")) {
        const gchar *app, *icon, *summary, *body; guint32 replaces; GVariant *actions, *hints; gint32 timeout;
        g_variant_get(p, "(&su&s&s&s@as@a{sv}i)", &app, &replaces, &icon, &summary, &body, &actions, &hints, &timeout);
        guint32 id = replaces ? replaces : next_id++;
        const gchar *entry = NULL;                         /* which app: "org.mozilla.firefox" */
        g_variant_lookup(hints, "desktop-entry", "&s", &entry);
        char a[128], t[200], b[400], line[800];
        clean(entry && *entry ? entry : app, a, sizeof a);
        clean(summary, t, sizeof t);
        clean(body, b, sizeof b);
        snprintf(line, sizeof line, "N %u\t%s\t%s\t%s\n", id, a, t, b);
        to_desktop(line);
        g_variant_unref(actions); g_variant_unref(hints);
        g_dbus_method_invocation_return_value(inv, g_variant_new("(u)", id));
    } else if (!strcmp(method, "CloseNotification")) {
        guint32 id; g_variant_get(p, "(u)", &id);
        char line[32]; snprintf(line, sizeof line, "C %u\n", id);
        to_desktop(line);
        g_dbus_connection_emit_signal(bus, NULL, "/org/freedesktop/Notifications", "org.freedesktop.Notifications",
                                      "NotificationClosed", g_variant_new("(uu)", id, 3), NULL);
        g_dbus_method_invocation_return_value(inv, NULL);
    } else if (!strcmp(method, "GetCapabilities")) {
        const gchar *caps[] = { "body", "actions", NULL };
        g_dbus_method_invocation_return_value(inv, g_variant_new("(^as)", caps));
    } else if (!strcmp(method, "GetServerInformation")) {
        g_dbus_method_invocation_return_value(inv, g_variant_new("(ssss)", "ccos", "ccos", "1.0", "1.2"));
    } else {
        g_dbus_method_invocation_return_error(inv, G_DBUS_ERROR, G_DBUS_ERROR_UNKNOWN_METHOD, "no such method");
    }
}

/* clicks on toasts come back through the second pipe */
static gboolean act_ready(GIOChannel *ch, GIOCondition cond, gpointer ud) {
    (void)cond; (void)ud;
    gchar *line = NULL; gsize len;
    GIOStatus st;
    while ((st = g_io_channel_read_line(ch, &line, &len, NULL, NULL)) == G_IO_STATUS_NORMAL) {
        guint32 id = (guint32)strtoul(line, NULL, 10);
        g_free(line);
        if (!id) continue;
        g_dbus_connection_emit_signal(bus, NULL, "/org/freedesktop/Notifications", "org.freedesktop.Notifications",
                                      "ActionInvoked", g_variant_new("(us)", id, "default"), NULL);
        g_dbus_connection_emit_signal(bus, NULL, "/org/freedesktop/Notifications", "org.freedesktop.Notifications",
                                      "NotificationClosed", g_variant_new("(uu)", id, 2), NULL);
    }
    return st != G_IO_STATUS_EOF && st != G_IO_STATUS_ERROR;
}

static void open_act(void);
static gboolean reopen(gpointer ud) { (void)ud; open_act(); return G_SOURCE_REMOVE; }
static gboolean act_hup(GIOChannel *ch, GIOCondition cond, gpointer ud) {
    gboolean keep = act_ready(ch, cond, ud);
    if (!keep || (cond & G_IO_HUP)) { g_timeout_add(500, reopen, NULL); return G_SOURCE_REMOVE; }
    return G_SOURCE_CONTINUE;
}
static void open_act(void) {
    mkdir("/run/ccos", 0755);
    if (access(ACT_FIFO, F_OK) != 0) mkfifo(ACT_FIFO, 0600);
    int fd = open(ACT_FIFO, O_RDWR | O_NONBLOCK | O_CLOEXEC);   /* RDWR: never sees end-of-file */
    if (fd < 0) return;
    GIOChannel *ch = g_io_channel_unix_new(fd);
    g_io_channel_set_close_on_unref(ch, TRUE);
    g_io_channel_set_flags(ch, G_IO_FLAG_NONBLOCK, NULL);
    g_io_add_watch(ch, G_IO_IN | G_IO_HUP | G_IO_ERR, act_hup, NULL);
}

static const GDBusInterfaceVTable vt = { method_call, NULL, NULL, { 0 } };
static void name_lost(GDBusConnection *c, const gchar *name, gpointer loop) { (void)c; (void)name; g_main_loop_quit(loop); }

int main(void) {
    GError *err = NULL;
    bus = g_bus_get_sync(G_BUS_TYPE_SESSION, NULL, &err);
    if (!bus) { fprintf(stderr, "ccos-notifyd: %s\n", err->message); return 1; }
    GDBusNodeInfo *info = g_dbus_node_info_new_for_xml(xml, NULL);
    g_dbus_connection_register_object(bus, "/org/freedesktop/Notifications", info->interfaces[0], &vt, NULL, NULL, NULL);
    GMainLoop *loop = g_main_loop_new(NULL, FALSE);
    g_bus_own_name_on_connection(bus, "org.freedesktop.Notifications", G_BUS_NAME_OWNER_FLAGS_NONE, NULL, name_lost, loop, NULL);
    open_act();
    g_main_loop_run(loop);
    return 0;
}
