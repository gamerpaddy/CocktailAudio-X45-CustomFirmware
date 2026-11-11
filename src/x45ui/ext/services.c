#define _GNU_SOURCE
#include "ext.h"
#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#define ROOT "/home/root/x45new"
#define SSH_OFF "/etc/ssh/sshd_not_to_be_run"
#define INSTALL_STATUS "/tmp/x45radio-install.status"
extern const struct ui_ext ext_radioinstall, ext_ssh;
static pid_t installer, ssh_job;
static int installed, checked;
static long last_check, last_full;
static char install_note[180], ssh_note[120];

static int radio_complete(void)
{
    char line[256], path[512];
    struct stat st;
    FILE *f = fopen(ROOT "/lib/radio-files.txt", "r");
    int n = 0, ok = 1;
    if (!f) return 0;
    while (fgets(line, sizeof line, f)) {
        line[strcspn(line, "\r\n")] = 0;
        if (!line[0]) continue;
        n++;
        snprintf(path, sizeof path, ROOT "/%s", line);
        if (stat(path, &st) || !S_ISREG(st.st_mode) || st.st_size <= 0 ||
            (!strncmp(line, "bin/", 4) && access(path, X_OK))) { ok = 0; break; }
    }
    fclose(f);
    return ok && n > 0;
}

static int install_visible(void)
{
    if (!checked) { installed = radio_complete(); checked = 1; }
    return !installed;
}

static pid_t launch(const char *program, const char *arg, const char *log)
{
    pid_t pid = fork();
    if (!pid) {
        int f = open(log, O_WRONLY | O_CREAT | O_TRUNC, 0600), fd;
        if (f >= 0) { dup2(f, 1); dup2(f, 2); if (f > 2) close(f); }
        for (fd = 3; fd < 128; fd++) close(fd);   /* the panel's sockets, pipes and device files stay with the panel */
        execl(program, program, arg, (char *)NULL);
        _exit(127);
    }
    return pid;
}

static void install_ok(struct ui *u)
{
    if (installer) return;
    installer = launch(ROOT "/bin/x45radio-install", "install", "/tmp/x45radio-install.log");
    if (installer < 0) { installer = 0; snprintf(install_note, sizeof install_note, "Could not start installer"); }
    else snprintf(install_note, sizeof install_note, "Checking components...");
    u->dirty = 1;
}

static void ssh_set(struct ui *u, int on)
{
    if (ssh_job) return;
    ssh_job = launch(ROOT "/bin/x45ssh", on ? "on" : "off", "/tmp/x45ssh.log");
    if (ssh_job < 0) { ssh_job = 0; snprintf(ssh_note, sizeof ssh_note, "Could not change SSH"); }
    else snprintf(ssh_note, sizeof ssh_note, "%s SSH...", on ? "Enabling" : "Disabling");
    u->dirty = 1;
}

static void ssh_ok(struct ui *u) { ssh_set(u, access(SSH_OFF, F_OK) == 0); }
static void ssh_nav(struct ui *u, int dx, int dy) { (void)dy; if (dx) ssh_set(u, dx > 0); }

static void poll_services(struct ui *u)
{
    long now = ext_now_ms();
    int status;
    if (ext_since(now, last_check) < 500) return;
    last_check = now;
    if (installer) {
        FILE *f = fopen(INSTALL_STATUS, "r");
        if (f) { if (fgets(install_note, sizeof install_note, f)) install_note[strcspn(install_note, "\r\n")] = 0; fclose(f); }
        if (waitpid(installer, &status, WNOHANG) == installer) {
            installer = 0;
            if (!WIFEXITED(status) || WEXITSTATUS(status)) {
                if (!install_note[0] || !strcmp(install_note, "Checking components..."))
                    snprintf(install_note, sizeof install_note, "Installation failed; press OK to retry");
            }
        }
        u->dirty = 1;
    }
    if (ssh_job && waitpid(ssh_job, &status, WNOHANG) == ssh_job) {
        ssh_job = 0;
        snprintf(ssh_note, sizeof ssh_note, "%s", WIFEXITED(status) && !WEXITSTATUS(status) ?
                 "Saved; this setting also applies after reboot" : "SSH change failed; check the service configuration");
        u->dirty = 1;
    }
    /* the component list is re-read (a stat of about 25 files) twice a second only while an install runs or its
     * screen is open; otherwise every 10 s is plenty */
    if (installer || (u->screen == SCR_EXT && u->ext == &ext_radioinstall) || ext_since(now, last_full) >= 10000) {
        int complete = radio_complete();
        last_full = now;
        if (!checked || complete != installed) { installed = complete; checked = 1; u->dirty = 1; }
    }
    if (u->screen == SCR_OPTIONS && u->sel >= 4 + ui_ext_menu_count()) u->sel = 3 + ui_ext_menu_count();
}

static void install_draw(struct ui *u)
{
    struct x45_surface *s = &u->g.back;
    x45_text(s, 24, 90, installed ? "WiFi and Bluetooth installed" : "Install WiFi and Bluetooth", 3, u->t.text);
    ext_para(u, 24, 155, s->w - 48, install_note[0] ? install_note :
             "Press OK to install missing components. If no kit is bundled, insert a USB drive containing the x45-radio-kit folder.", 3, u->t.dim);
    draw_hints(u, installer ? "RETURN|back" : "OK|install;RETURN|back");
}

static void ssh_draw(struct ui *u)
{
    struct x45_surface *s = &u->g.back;
    int on = access(SSH_OFF, F_OK) != 0;
    x45_text(s, 24, 90, on ? "SSH: enabled" : "SSH: disabled", 4, u->t.accent);
    ext_para(u, 24, 165, s->w - 48, "Remote shell access from local IP addresses. Disabling SSH closes remote shell sessions. You can enable it again here.", 3, u->t.text);
    if (ssh_note[0]) ext_para(u, 24, 320, s->w - 48, ssh_note, 2, u->t.dim);
    draw_hints(u, ssh_job ? "RETURN|back" : "OK|toggle;LEFT|disable;RIGHT|enable;RETURN|back");
}

/* No web/API command for SSH: turning the remote shell on or off is done on the panel only (a web page or a script
 * that could switch it off would also cut off the way back in). */
static int install_cmd(struct ui *u, const char *arg)
{
    if (strcmp(arg, "install")) return 0;
    install_ok(u); return 1;
}
static void ssh_json(struct ui *u, FILE *f)
{
    (void)u;
    fprintf(f, "{\"enabled\":%s,\"busy\":%s}", access(SSH_OFF, F_OK) ? "true" : "false", ssh_job ? "true" : "false");
}
static void install_json(struct ui *u, FILE *f)
{
    (void)u;
    fprintf(f, "{\"installed\":%s,\"busy\":%s}", installed ? "true" : "false", installer ? "true" : "false");
}
const struct ui_ext ext_radioinstall = {
    .name = "radioinstall", .title = "WIFI / BLUETOOTH INSTALL", .menu = "Install WiFi / Bluetooth",
    .menu_visible = install_visible, .draw = install_draw, .ok = install_ok,
    .poll = poll_services, .cmd = install_cmd, .json = install_json,
};
const struct ui_ext ext_ssh = {
    .name = "ssh", .title = "SSH ACCESS", .menu = "SSH access",
    .draw = ssh_draw, .ok = ssh_ok, .nav = ssh_nav, .json = ssh_json,
};
