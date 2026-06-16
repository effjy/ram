/**
 * RAM Visualizer - A modern, high-performance GTK3 & Cairo RAM usage analyzer.
 * Developed in C for Ubuntu-Mate (and other GTK3 desktops).
 * 
 * Compiles cleanly with zero warnings: gcc -Wall -Wextra -O2 main.c `pkg-config --cflags --libs gtk+-3.0` -lm -o ram-visualizer
 * 
 * Features:
 * - Left side: GTK TreeView of top-RAM consuming operations (PID, Icon, Name, RAM in MB).
 * - Right side: Visual charts drawn with Cairo custom rendering (RAM ring gauge & horizontal bar charts).
 * - Real-time process memory updates via periodic timeout.
 * - Search filter for process list.
 * - Direct "Process Termination" (Kill Signal) for taking down RAM hogs.
 */

#include <gtk/gtk.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <ctype.h>
#include <unistd.h>
#include <signal.h>
#include <errno.h>
#include <math.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

// Columns for our process GtkListStore
enum {
    COL_ICON = 0,
    COL_PID,
    COL_NAME,
    COL_RAM_MB,
    COL_RAM_STR,
    NUM_COLS
};

// Structure holding single process information
typedef struct {
    gint pid;
    gchar *name;
    gdouble rss_mb;
    gchar *icon_name;
} ProcessInfo;

// Main Context holding all state for the GTK Application
typedef struct {
    GList *process_list;          // List of current ProcessInfo*
    gdouble total_ram_gb;        // Total System memory in GB
    gdouble used_ram_gb;         // Used System memory in GB
    gdouble used_ram_percent;    // Percentage of total RAM currently used
    gint selected_pid;           // PID selected in TreeView
    gchar *selected_name;        // Name of selected process
    gchar *search_query;         // Current filter search query

    GtkListStore *list_store;    // List model
    GtkWidget *tree_view;        // TreeView widget
    GtkWidget *scrolled_window;  // Scroll container for the TreeView
    gdouble saved_scroll;        // Vertical scroll position kept across refreshes
    gboolean scroll_restore_pending; // Guard so only one restore idle is queued
    GtkWidget *drawing_area;     // Custom drawing widget
    GtkWidget *stats_label;      // RAM stats text banner
    GtkWidget *status_bar;       // Information status bar
    GtkWidget *search_entry;     // Search input
    GtkWidget *kill_btn;         // Kill selected action button
} AppContext;

// Deallocate process list structures
static void clear_process_list(AppContext *ctx) {
    if (ctx->process_list) {
        GList *l;
        for (l = ctx->process_list; l != NULL; l = l->next) {
            ProcessInfo *p = (ProcessInfo *)l->data;
            g_free(p->name);
            g_free(p->icon_name);
            g_slice_free(ProcessInfo, p);
        }
        g_list_free(ctx->process_list);
        ctx->process_list = NULL;
    }
}

// Replace the status bar message (pop first so the message stack can't grow
// unbounded over a long-running session).
static void set_status(AppContext *ctx, const gchar *msg) {
    if (!ctx->status_bar) return;
    gtk_statusbar_pop(GTK_STATUSBAR(ctx->status_bar), 1);
    gtk_statusbar_push(GTK_STATUSBAR(ctx->status_bar), 1, msg);
}

// Map process names and common binaries to corresponding GTK system icons
static gchar *guess_icon_for_proc(const gchar *name) {
    if (!name) return g_strdup("system-run");
    gchar *lower = g_ascii_strdown(name, -1);
    gchar *icon = "system-run";

    if (strstr(lower, "firefox")) icon = "firefox";
    else if (strstr(lower, "chrome") || strstr(lower, "chromium")) icon = "google-chrome";
    else if (strstr(lower, "thunderbird")) icon = "thunderbird";
    else if (strstr(lower, "terminal") || strstr(lower, "bash") || strstr(lower, "zsh") || strstr(lower, "sh")) icon = "utilities-terminal";
    else if (strstr(lower, "code") || strstr(lower, "vscode") || strstr(lower, "vim") || strstr(lower, "nano") || strstr(lower, "gedit")) icon = "accessories-text-editor";
    else if (strstr(lower, "gimp")) icon = "gimp";
    else if (strstr(lower, "slack")) icon = "slack";
    else if (strstr(lower, "discord")) icon = "discord";
    else if (strstr(lower, "spotify")) icon = "spotify";
    else if (strstr(lower, "steam")) icon = "steam";
    else if (strstr(lower, "vlc")) icon = "vlc";
    else if (strstr(lower, "python") || strstr(lower, "node") || strstr(lower, "perl") || strstr(lower, "java")) icon = "text-x-script";
    else if (strstr(lower, "systemd") || strstr(lower, "dbus") || strstr(lower, "udev") || strstr(lower, "kernel")) icon = "preferences-system";
    else if (strstr(lower, "explorer") || strstr(lower, "nautilus") || strstr(lower, "caja") || strstr(lower, "thunar")) icon = "system-file-manager";
    else if (strstr(lower, "image") || strstr(lower, "view") || strstr(lower, "shot")) icon = "image-x-generic";
    else if (strstr(lower, "audio") || strstr(lower, "music") || strstr(lower, "volume") || strstr(lower, "pulse")) icon = "audio-card";
    
    g_free(lower);
    return g_strdup(icon);
}

// Safe parsing of /proc/<pid>/status & cmdline for memories and names
static void parse_proc_pid(gint pid, gchar **name, gdouble *rss_mb) {
    gchar path[256];
    snprintf(path, sizeof(path), "/proc/%d/status", pid);
    
    FILE *f = fopen(path, "r");
    *rss_mb = 0.0;
    *name = NULL;
    if (!f) return;

    gchar line[256];
    gchar proc_name[256] = "";
    glong rss_kb = 0;
    gboolean has_rss = FALSE;

    while (fgets(line, sizeof(line), f)) {
        if (strncmp(line, "Name:", 5) == 0) {
            gchar *p = line + 5;
            while (*p && isspace((unsigned char)*p)) p++;
            gchar *end = p;
            while (*end && *end != '\n') end++;
            *end = '\0';
            strncpy(proc_name, p, sizeof(proc_name) - 1);
        } else if (strncmp(line, "VmRSS:", 6) == 0) {
            gchar *p = line + 6;
            while (*p && isspace((unsigned char)*p)) p++;
            rss_kb = strtol(p, NULL, 10);
            has_rss = TRUE;
        }
    }
    fclose(f);

    // If RSS was not found, we don't want to show it (e.g. idle kernel threads)
    if (!has_rss || rss_kb <= 0) {
        return;
    }

    // Attempt to parse actual program command line argument for precise naming
    snprintf(path, sizeof(path), "/proc/%d/cmdline", pid);
    FILE *fcmd = fopen(path, "rb");
    if (fcmd) {
        gchar cmd[512];
        size_t n = fread(cmd, 1, sizeof(cmd) - 1, fcmd);
        if (n > 0) {
            cmd[n] = '\0';
            gchar *base = cmd;
            gchar *slash = strrchr(cmd, '/');
            if (slash) base = slash + 1;
            
            // Clean non-printable bytes common in argument vectors
            gint len = strlen(base);
            if (len > 0) {
                gboolean valid = TRUE;
                for (gint i = 0; i < len; i++) {
                    if (!isprint((unsigned char)base[i])) { valid = FALSE; break; }
                }
                if (valid) {
                    strncpy(proc_name, base, sizeof(proc_name) - 1);
                }
            }
        }
        fclose(fcmd);
    }

    if (strlen(proc_name) > 0) {
        *name = g_strdup(proc_name);
        *rss_mb = (gdouble)rss_kb / 1024.0;
    }
}

// Read Linux System overall RAM specifications
static void get_system_ram(gdouble *total_gb, gdouble *used_gb, gdouble *percent) {
    FILE *f = fopen("/proc/meminfo", "r");
    *total_gb = 8.0;  // Default fallback limits
    *used_gb = 4.0;
    *percent = 50.0;
    if (!f) return;

    gchar line[256];
    glong total_kb = 0;
    glong avail_kb = 0;

    while (fgets(line, sizeof(line), f)) {
        if (strncmp(line, "MemTotal:", 9) == 0) {
            gchar *p = line + 9;
            while (*p && isspace((unsigned char)*p)) p++;
            total_kb = strtol(p, NULL, 10);
        } else if (strncmp(line, "MemAvailable:", 13) == 0) {
            gchar *p = line + 13;
            while (*p && isspace((unsigned char)*p)) p++;
            avail_kb = strtol(p, NULL, 10);
        }
    }
    fclose(f);

    if (total_kb > 0) {
        glong used_kb = total_kb - avail_kb;
        if (used_kb < 0) used_kb = 0;
        *total_gb = (gdouble)total_kb / (1024.0 * 1024.0);
        *used_gb = (gdouble)used_kb / (1024.0 * 1024.0);
        *percent = (*used_gb / *total_gb) * 100.0;
    }
}

// Compare process RSS desc for sorting
static gint compare_processes(gconstpointer a, gconstpointer b) {
    const ProcessInfo *pa = (const ProcessInfo *)a;
    const ProcessInfo *pb = (const ProcessInfo *)b;
    if (pa->rss_mb > pb->rss_mb) return -1;
    if (pa->rss_mb < pb->rss_mb) return 1;
    return 0;
}

// Restore the vertical scroll position after a refresh repopulates the list.
// Runs on idle so the TreeView has recomputed its scroll range first.
static gboolean restore_scroll_position(gpointer data) {
    AppContext *ctx = (AppContext *)data;
    ctx->scroll_restore_pending = FALSE;
    if (ctx->scrolled_window) {
        GtkAdjustment *vadj = gtk_scrolled_window_get_vadjustment(
            GTK_SCROLLED_WINDOW(ctx->scrolled_window));
        if (vadj) gtk_adjustment_set_value(vadj, ctx->saved_scroll);
    }
    return G_SOURCE_REMOVE; // one-shot
}

// Main polling loop to refresh processes and redraw graphics
static void refresh_system_stats(AppContext *ctx) {
    clear_process_list(ctx);
    get_system_ram(&(ctx->total_ram_gb), &(ctx->used_ram_gb), &(ctx->used_ram_percent));

    // Open /proc sandbox directory
    DIR *dir = opendir("/proc");
    if (!dir) {
        set_status(ctx, "Error: Unable to open /proc directory!");
        return;
    }

    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL) {
        // We only parse numbered processes folders
        gboolean is_pid = TRUE;
        for (gsize i = 0; entry->d_name[i] != '\0'; i++) {
            if (!isdigit((unsigned char)entry->d_name[i])) {
                is_pid = FALSE;
                break;
            }
        }

        if (is_pid && entry->d_name[0] != '\0') {
            gint pid = atoi(entry->d_name);
            gchar *name = NULL;
            gdouble rss_mb = 0.0;
            parse_proc_pid(pid, &name, &rss_mb);

            if (name && rss_mb > 0.5) { // Track processes consuming > 0.5MB
                ProcessInfo *p = g_slice_new(ProcessInfo);
                p->pid = pid;
                p->name = name;
                p->rss_mb = rss_mb;
                p->icon_name = guess_icon_for_proc(name);
                ctx->process_list = g_list_append(ctx->process_list, p);
            } else if (name) {
                g_free(name);
            }
        }
    }
    closedir(dir);

    // Sort heavy processes to the top
    ctx->process_list = g_list_sort(ctx->process_list, compare_processes);

    // Remember where the user has scrolled so the refresh doesn't yank them
    // back to the top of the process list.
    if (ctx->scrolled_window) {
        GtkAdjustment *vadj = gtk_scrolled_window_get_vadjustment(
            GTK_SCROLLED_WINDOW(ctx->scrolled_window));
        if (vadj) ctx->saved_scroll = gtk_adjustment_get_value(vadj);
    }

    // Remember the selected PID. Clearing the model below drops the selection
    // (firing on_selection_changed, which resets selected_pid to -1), so stash
    // it first and re-select the matching row after the reload.
    gint reselect_pid = ctx->selected_pid;

    // Filter, clean & reload the GtkTreeView ListStore model
    GtkIconTheme *icon_theme = gtk_icon_theme_get_default();
    gtk_list_store_clear(ctx->list_store);

    GList *l;
    gint counter = 0;
    for (l = ctx->process_list; l != NULL; l = l->next) {
        ProcessInfo *p = (ProcessInfo *)l->data;

        // Perform text matching if searching
        if (ctx->search_query && strlen(ctx->search_query) > 0) {
            gchar *lower_name = g_ascii_strdown(p->name, -1);
            gchar *lower_query = g_ascii_strdown(ctx->search_query, -1);
            gboolean matches = (strstr(lower_name, lower_query) != NULL);
            g_free(lower_name);
            g_free(lower_query);
            if (!matches) continue;
        }

        // Retrieve process icon, or fallback gracefully
        GdkPixbuf *pixbuf = NULL;
        GError *error = NULL;
        if (p->icon_name) {
            pixbuf = gtk_icon_theme_load_icon(icon_theme, p->icon_name, 24, GTK_ICON_LOOKUP_FORCE_SIZE, &error);
            if (error) {
                g_error_free(error);
                error = NULL;
                pixbuf = gtk_icon_theme_load_icon(icon_theme, "system-run", 24, GTK_ICON_LOOKUP_FORCE_SIZE, &error);
                if (error) g_error_free(error);
            }
        }

        GtkTreeIter iter;
        gtk_list_store_append(ctx->list_store, &iter);
        
        gchar ram_str[32];
        if (p->rss_mb > 1024.0) {
            snprintf(ram_str, sizeof(ram_str), "%.2f GB", p->rss_mb / 1024.0);
        } else {
            snprintf(ram_str, sizeof(ram_str), "%.1f MB", p->rss_mb);
        }

        gtk_list_store_set(ctx->list_store, &iter,
                           COL_ICON, pixbuf,
                           COL_PID, p->pid,
                           COL_NAME, p->name,
                           COL_RAM_MB, p->rss_mb,
                           COL_RAM_STR, ram_str,
                           -1);

        if (pixbuf) g_object_unref(pixbuf);
        
        counter++;
        if (counter >= 150) break; // Display top 150 matching processes for maximum performance
    }

    // Re-select the previously selected process if it still exists, so the
    // selection and the Kill button survive the periodic refresh.
    if (reselect_pid > 0) {
        GtkTreeIter iter;
        gboolean valid = gtk_tree_model_get_iter_first(GTK_TREE_MODEL(ctx->list_store), &iter);
        while (valid) {
            gint row_pid = 0;
            gtk_tree_model_get(GTK_TREE_MODEL(ctx->list_store), &iter, COL_PID, &row_pid, -1);
            if (row_pid == reselect_pid) {
                GtkTreeSelection *sel = gtk_tree_view_get_selection(GTK_TREE_VIEW(ctx->tree_view));
                gtk_tree_selection_select_iter(sel, &iter);
                break;
            }
            valid = gtk_tree_model_iter_next(GTK_TREE_MODEL(ctx->list_store), &iter);
        }
    }

    // Refresh memory label
    gchar label_text[256];
    snprintf(label_text, sizeof(label_text), 
             "<span font_desc='Sans Bold 12' color='#ffffff'>System Memory: %.2f GB Used / %.2f GB Total (%.1f%%)</span>", 
             ctx->used_ram_gb, ctx->total_ram_gb, ctx->used_ram_percent);
    gtk_label_set_markup(GTK_LABEL(ctx->stats_label), label_text);

    // Re-draw right panel visuals
    gtk_widget_queue_draw(ctx->drawing_area);

    // The TreeView updates its scroll range lazily after rows are added, so
    // restore the saved scroll position on idle to avoid it being clamped.
    if (ctx->scrolled_window && !ctx->scroll_restore_pending) {
        ctx->scroll_restore_pending = TRUE;
        g_idle_add(restore_scroll_position, ctx);
    }
}

// 15s Timer trigger
static gboolean on_timeout_tick(gpointer data) {
    AppContext *ctx = (AppContext *)data;
    refresh_system_stats(ctx);
    return TRUE; // keep running
}

// Tree view row click mapping
static void on_row_activated(GtkTreeView *tree_view, GtkTreePath *path, GtkTreeViewColumn *column, gpointer data) {
    (void)tree_view; (void)column;
    AppContext *ctx = (AppContext *)data;
    GtkTreeIter iter;
    if (gtk_tree_model_get_iter(GTK_TREE_MODEL(ctx->list_store), &iter, path)) {
        gint pid;
        gchar *name;
        gtk_tree_model_get(GTK_TREE_MODEL(ctx->list_store), &iter, COL_PID, &pid, COL_NAME, &name, -1);
        ctx->selected_pid = pid;
        if (ctx->selected_name) g_free(ctx->selected_name);
        ctx->selected_name = g_strdup(name);
        
        gtk_widget_set_sensitive(ctx->kill_btn, TRUE);

        gchar status[128];
        snprintf(status, sizeof(status), "Selected: %s (PID: %d)", name, pid);
        set_status(ctx, status);
        g_free(name);
    }
}

static void on_selection_changed(GtkTreeSelection *selection, gpointer data) {
    AppContext *ctx = (AppContext *)data;
    GtkTreeIter iter;
    if (gtk_tree_selection_get_selected(selection, NULL, &iter)) {
        gint pid;
        gchar *name;
        gtk_tree_model_get(GTK_TREE_MODEL(ctx->list_store), &iter, COL_PID, &pid, COL_NAME, &name, -1);
        ctx->selected_pid = pid;
        if (ctx->selected_name) g_free(ctx->selected_name);
        ctx->selected_name = g_strdup(name);
        
        gtk_widget_set_sensitive(ctx->kill_btn, TRUE);

        gchar status[128];
        snprintf(status, sizeof(status), "Selected: %s (PID: %d)", name, pid);
        set_status(ctx, status);
        g_free(name);
    } else {
        ctx->selected_pid = -1;
        if (ctx->selected_name) {
            g_free(ctx->selected_name);
            ctx->selected_name = NULL;
        }
        gtk_widget_set_sensitive(ctx->kill_btn, FALSE);
    }
}

// Instant Filter/Search input
static void on_search_changed(GtkSearchEntry *entry, gpointer data) {
    AppContext *ctx = (AppContext *)data;
    const gchar *text = gtk_entry_get_text(GTK_ENTRY(entry));
    if (ctx->search_query) g_free(ctx->search_query);
    ctx->search_query = g_strdup(text);
    refresh_system_stats(ctx);
}

// True if the process still exists. kill(pid, 0) sends no signal; it only
// performs the existence/permission check (ESRCH means it is gone).
static gboolean process_alive(gint pid) {
    return (kill(pid, 0) == 0) || (errno == EPERM);
}

// Send a signal, then poll briefly to confirm the process actually exits.
// Returns TRUE once the process is gone. Blocks up to ~timeout_ms.
static gboolean signal_and_confirm(gint pid, int sig, int timeout_ms) {
    if (kill(pid, sig) != 0) return FALSE;
    for (int waited = 0; waited < timeout_ms; waited += 50) {
        if (!process_alive(pid)) return TRUE;
        g_usleep(50 * 1000); // 50ms
    }
    return !process_alive(pid);
}

// Kill selected RAM hog
static void on_kill_clicked(GtkButton *button, gpointer data) {
    (void)button;
    AppContext *ctx = (AppContext *)data;
    if (ctx->selected_pid <= 0 || !ctx->selected_name) return;

    gint target_pid = ctx->selected_pid;
    gchar *target_name = g_strdup(ctx->selected_name);

    // Direct desktop dialog check
    GtkWidget *parent = gtk_widget_get_toplevel(ctx->tree_view);
    GtkWidget *dialog = gtk_message_dialog_new(GTK_WINDOW(parent),
                                 GTK_DIALOG_MODAL | GTK_DIALOG_DESTROY_WITH_PARENT,
                                 GTK_MESSAGE_WARNING,
                                 GTK_BUTTONS_YES_NO,
                                 "Are you sure you want to terminate '%s' (PID: %d)?",
                                 target_name, target_pid);
    
    gtk_window_set_title(GTK_WINDOW(dialog), "Terminate Process");
    gint response = gtk_dialog_run(GTK_DIALOG(dialog));
    gtk_widget_destroy(dialog);

    if (response == GTK_RESPONSE_YES) {
        if (target_pid <= 0) {
            g_free(target_name);
            return;
        }
        // Ask politely first, then verify the process actually exits — a
        // process can catch or ignore SIGTERM, so a delivered signal is not
        // proof of death.
        errno = 0;
        gboolean killed = signal_and_confirm(target_pid, SIGTERM, 1500);
        gint term_errno = errno;

        // Still alive (and we have the rights to signal it): offer a forceful
        // SIGKILL, which cannot be caught or ignored.
        if (!killed && term_errno != EPERM) {
            GtkWidget *force_dialog = gtk_message_dialog_new(GTK_WINDOW(parent),
                                         GTK_DIALOG_MODAL | GTK_DIALOG_DESTROY_WITH_PARENT,
                                         GTK_MESSAGE_QUESTION,
                                         GTK_BUTTONS_YES_NO,
                                         "'%s' (PID: %d) did not respond to a graceful quit.\n"
                                         "Force kill it with SIGKILL?",
                                         target_name, target_pid);
            gtk_window_set_title(GTK_WINDOW(force_dialog), "Force Kill Process");
            gint force_resp = gtk_dialog_run(GTK_DIALOG(force_dialog));
            gtk_widget_destroy(force_dialog);
            if (force_resp == GTK_RESPONSE_YES) {
                errno = 0;
                killed = signal_and_confirm(target_pid, SIGKILL, 1500);
                term_errno = errno;
            } else {
                g_free(target_name);
                return;
            }
        }

        errno = term_errno; // preserve for the error-message branch below
        if (killed) {
            gchar msg[128];
            snprintf(msg, sizeof(msg), "Successfully terminated %s (PID: %d)", target_name, target_pid);
            set_status(ctx, msg);

            // Instantly clear selection and refresh stats
            if (ctx->selected_pid == target_pid) {
                ctx->selected_pid = -1;
                if (ctx->selected_name) {
                    g_free(ctx->selected_name);
                    ctx->selected_name = NULL;
                }
                gtk_widget_set_sensitive(ctx->kill_btn, FALSE);
            }
            refresh_system_stats(ctx);
        } else {
            const gchar *detail = (errno == EPERM)
                ? "You may need administrative privileges to kill this process."
                : (errno == ESRCH)
                    ? "The process no longer exists."
                    : "The process could not be terminated.";
            GtkWidget *err_dialog = gtk_message_dialog_new(GTK_WINDOW(parent),
                                         GTK_DIALOG_MODAL,
                                         GTK_MESSAGE_ERROR,
                                         GTK_BUTTONS_OK,
                                         "Failed to terminate process %d.\n%s",
                                         target_pid, detail);
            gtk_dialog_run(GTK_DIALOG(err_dialog));
            gtk_widget_destroy(err_dialog);
        }
    }
    g_free(target_name);
}

// Drawing helper rounded rectangles in Cairo
static void draw_rounded_rectangle(cairo_t *cr, double x, double y, double w, double h, double r) {
    cairo_new_sub_path(cr);
    cairo_arc(cr, x + w - r, y + r, r, -M_PI / 2.0, 0);
    cairo_arc(cr, x + w - r, y + h - r, r, 0, M_PI / 2.0);
    cairo_arc(cr, x + r, y + h - r, r, M_PI / 2.0, M_PI);
    cairo_arc(cr, x + r, y + r, r, M_PI, 3.0 * M_PI / 2.0);
    cairo_close_path(cr);
}

// Custom modern Cairo Draw Engine for high-intel Memory graphs
static gboolean on_draw_cairo(GtkWidget *widget, cairo_t *cr, gpointer data) {
    AppContext *ctx = (AppContext *)data;
    gint w = gtk_widget_get_allocated_width(widget);
    gint h = gtk_widget_get_allocated_height(widget);

    // 1. Sleek metallic background fill
    cairo_set_source_rgb(cr, 0.08, 0.10, 0.15); // Deep slate background
    cairo_paint(cr);

    // Subtle dark visual grid overlay
    cairo_set_source_rgba(cr, 1.0, 1.0, 1.0, 0.02);
    cairo_set_line_width(cr, 1.0);
    for (int x = 20; x < w; x += 40) {
        cairo_move_to(cr, x, 0);
        cairo_line_to(cr, x, h);
        cairo_stroke(cr);
    }
    for (int y = 20; y < h; y += 40) {
        cairo_move_to(cr, 0, y);
        cairo_line_to(cr, w, y);
        cairo_stroke(cr);
    }

    // 2. RAM gauge circle drawn in the center-top
    double gauge_cx = w / 2.0;
    double gauge_cy = 130.0;
    double gauge_radius = 80.0;

    // Base track ring
    cairo_set_source_rgb(cr, 0.15, 0.18, 0.25);
    cairo_set_line_width(cr, 14.0);
    cairo_arc(cr, gauge_cx, gauge_cy, gauge_radius, 0, 2 * M_PI);
    cairo_stroke(cr);

    // Memory used arc (active colored overlay with smooth vector gradient)
    cairo_pattern_t *ring_gradient = cairo_pattern_create_linear(gauge_cx - 80, gauge_cy - 80, gauge_cx + 80, gauge_cy + 80);
    if (ctx->used_ram_percent < 60.0) {
        cairo_pattern_add_color_stop_rgb(ring_gradient, 0.0, 0.1, 0.8, 0.9); // Cyan / Blue glow
        cairo_pattern_add_color_stop_rgb(ring_gradient, 1.0, 0.3, 0.9, 0.6);
    } else if (ctx->used_ram_percent < 85.0) {
        cairo_pattern_add_color_stop_rgb(ring_gradient, 0.0, 0.9, 0.7, 0.1); // Amber warming
        cairo_pattern_add_color_stop_rgb(ring_gradient, 1.0, 0.9, 0.4, 0.1);
    } else {
        cairo_pattern_add_color_stop_rgb(ring_gradient, 0.0, 0.9, 0.1, 0.1); // High critical RED
        cairo_pattern_add_color_stop_rgb(ring_gradient, 1.0, 0.9, 0.4, 0.1);
    }
    cairo_set_source(cr, ring_gradient);
    cairo_set_line_width(cr, 14.0);
    cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);
    
    // Draw arc based on system RAM used bounds
    double angle_start = -M_PI / 2.0;
    double angle_end = angle_start + (ctx->used_ram_percent / 100.0) * (2.0 * M_PI);
    cairo_arc(cr, gauge_cx, gauge_cy, gauge_radius, angle_start, angle_end);
    cairo_stroke(cr);
    cairo_pattern_destroy(ring_gradient);

    // Overall value string overlay
    cairo_set_source_rgb(cr, 1.0, 1.0, 1.0);
    cairo_select_font_face(cr, "Ubuntu, Sans", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_BOLD);
    cairo_set_font_size(cr, 24.0);
    gchar percentage_str[32];
    snprintf(percentage_str, sizeof(percentage_str), "%.1f%%", ctx->used_ram_percent);
    cairo_text_extents_t extents;
    cairo_text_extents(cr, percentage_str, &extents);
    cairo_move_to(cr, gauge_cx - extents.width / 2.0, gauge_cy + 4.0);
    cairo_show_text(cr, percentage_str);

    // "USED" minor caption
    cairo_set_source_rgb(cr, 0.6, 0.7, 0.8);
    cairo_set_font_size(cr, 11.0);
    cairo_text_extents(cr, "RAM COMMITTED", &extents);
    cairo_move_to(cr, gauge_cx - extents.width / 2.0, gauge_cy + 22.0);
    cairo_show_text(cr, "RAM COMMITTED");

    // 3. Top Processes Section (Clean, modern horizontal gradient bar representations)
    double bar_start_y = 260.0;
    double bar_spacing = 42.0;
    gint num_bars = 6;

    cairo_set_source_rgb(cr, 1.0, 1.0, 1.0);
    cairo_set_font_size(cr, 14.0);
    cairo_select_font_face(cr, "Ubuntu, Sans", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_BOLD);
    cairo_move_to(cr, 30.0, bar_start_y - 20.0);
    cairo_show_text(cr, "Top Memory Consumers (Actual RAM Use)");

    cairo_set_source_rgb(cr, 0.6, 0.7, 0.8);
    cairo_set_font_size(cr, 11.0);
    cairo_select_font_face(cr, "Ubuntu, Sans", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_NORMAL);

    // Retrieve global max RSS from the top process to scale bars
    gdouble max_rss = 1.0;
    if (ctx->process_list) {
        ProcessInfo *top = (ProcessInfo *)ctx->process_list->data;
        if (top->rss_mb > max_rss) {
            max_rss = top->rss_mb;
        }
    }

    GList *l = ctx->process_list;
    for (gint i = 0; i < num_bars && l != NULL; i++, l = l->next) {
        ProcessInfo *p = (ProcessInfo *)l->data;
        double y = bar_start_y + (i * bar_spacing);

        // Draw process designation text (Name + PID)
        cairo_set_source_rgb(cr, 0.9, 0.95, 1.0);
        cairo_set_font_size(cr, 11.0);
        cairo_select_font_face(cr, "Ubuntu, Sans", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_BOLD);
        cairo_move_to(cr, 30.0, y - 4.0);
        gchar label_proc[128];
        snprintf(label_proc, sizeof(label_proc), "%s [PID %d]", p->name, p->pid);
        cairo_show_text(cr, label_proc);

        // Draw formatted RAM readout
        cairo_set_source_rgb(cr, 0.6, 0.7, 0.8);
        cairo_set_font_size(cr, 11.0);
        cairo_select_font_face(cr, "Ubuntu, Sans", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_NORMAL);
        gchar label_ram_size[64];
        if (p->rss_mb > 1024.0) {
            snprintf(label_ram_size, sizeof(label_ram_size), "%.2f GB", p->rss_mb / 1024.0);
        } else {
            snprintf(label_ram_size, sizeof(label_ram_size), "%.1f MB", p->rss_mb);
        }
        cairo_text_extents(cr, label_ram_size, &extents);
        cairo_move_to(cr, w - 30.0 - extents.width, y - 4.0);
        cairo_show_text(cr, label_ram_size);

        // Gray background channel container
        double track_w = w - 60.0;
        cairo_set_source_rgb(cr, 0.15, 0.18, 0.25);
        draw_rounded_rectangle(cr, 30.0, y, track_w, 10.0, 5.0);
        cairo_fill(cr);

        // High resolution memory weight bar
        double fill_percent = p->rss_mb / max_rss;
        if (fill_percent > 1.0) fill_percent = 1.0;
        if (fill_percent < 0.02) fill_percent = 0.02; // Small sliver to remain active
        double fill_w = track_w * fill_percent;

        // Custom premium bar color gradient based on sequence
        cairo_pattern_t *bar_gradient = cairo_pattern_create_linear(30.0, y, 30.0 + fill_w, y);
        if (i == 0) {
            cairo_pattern_add_color_stop_rgb(bar_gradient, 0.0, 1.0, 0.3, 0.3); // Heavy Crimson Warning
            cairo_pattern_add_color_stop_rgb(bar_gradient, 1.0, 0.9, 0.5, 0.1);
        } else if (i == 1) {
            cairo_pattern_add_color_stop_rgb(bar_gradient, 0.0, 0.9, 0.5, 0.1); // Warm Amber
            cairo_pattern_add_color_stop_rgb(bar_gradient, 1.0, 0.9, 0.7, 0.1);
        } else {
            cairo_pattern_add_color_stop_rgb(bar_gradient, 0.0, 0.1, 0.8, 0.9); // Normal Cyan/Teal
            cairo_pattern_add_color_stop_rgb(bar_gradient, 1.0, 0.1, 0.9, 0.6);
        }

        cairo_set_source(cr, bar_gradient);
        draw_rounded_rectangle(cr, 30.0, y, fill_w, 10.0, 5.0);
        cairo_fill(cr);
        cairo_pattern_destroy(bar_gradient);
    }

    // High fidelity HUD footer stats
    cairo_set_source_rgb(cr, 0.4, 0.5, 0.6);
    cairo_select_font_face(cr, "Ubuntu, Sans", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_BOLD);
    cairo_set_font_size(cr, 11.0);
    cairo_move_to(cr, 30.0, h - 20.0);
    cairo_show_text(cr, "SYSTEM METRIC MONITOR LIVE (5S REFRESH)");

    // Static horizontal visual division line
    cairo_set_source_rgba(cr, 1.0, 1.0, 1.0, 0.08);
    cairo_set_line_width(cr, 1.0);
    cairo_move_to(cr, 30.0, h - 35.0);
    cairo_line_to(cr, w - 30.0, h - 35.0);
    cairo_stroke(cr);

    return TRUE;
}

int main(gint argc, gchar *argv[]) {
    // Standard GTK3 initialization
    gtk_init(&argc, &argv);

    // Configure Application context
    AppContext *ctx = g_slice_new0(AppContext);
    ctx->selected_pid = -1;
    ctx->selected_name = NULL;
    ctx->search_query = NULL;

    // Load initial processes list standard parameters
    get_system_ram(&(ctx->total_ram_gb), &(ctx->used_ram_gb), &(ctx->used_ram_percent));

    // 1. Create main window fitting in limits, centered on launching
    GtkWidget *window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    gtk_window_set_title(GTK_WINDOW(window), "RAM Visualizer");
    gtk_window_set_default_size(GTK_WINDOW(window), 1100, 650);
    gtk_window_set_position(GTK_WINDOW(window), GTK_WIN_POS_CENTER);

    // Apply custom desktop icon
    if (gtk_icon_theme_has_icon(gtk_icon_theme_get_default(), "ram-visualizer")) {
        gtk_window_set_icon_name(GTK_WINDOW(window), "ram-visualizer");
    } else {
        GdkPixbuf *app_icon = NULL;
        GError *icon_err = NULL;
        // Attempt local SVG load, then local PNG fallback
        app_icon = gdk_pixbuf_new_from_file("./ram-visualizer.svg", &icon_err);
        if (icon_err) {
            g_error_free(icon_err);
            icon_err = NULL;
            app_icon = gdk_pixbuf_new_from_file("./ram_visualizer_icon.png", &icon_err);
            if (icon_err) g_error_free(icon_err);
        }
        if (app_icon) {
            gtk_window_set_icon(GTK_WINDOW(window), app_icon);
            g_object_unref(app_icon);
        }
    }

    // Connect standard window exit event
    g_signal_connect(window, "destroy", G_CALLBACK(gtk_main_quit), NULL);

    // 2. High-level layout boxes (Main horizontal box split: left processes / right visuals)
    GtkWidget *main_hbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
    gtk_container_set_border_width(GTK_CONTAINER(main_hbox), 12);
    gtk_container_add(GTK_CONTAINER(window), main_hbox);

    // LEFT DIVISION - Process tables operations
    GtkWidget *left_vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    gtk_box_pack_start(GTK_BOX(main_hbox), left_vbox, TRUE, TRUE, 0);

    // Section title
    GtkWidget *left_title_label = gtk_label_new(NULL);
    gtk_label_set_markup(GTK_LABEL(left_title_label), "<span font_desc='Sans Bold 12' color='#ffffff'>Processes Consuming RAM</span>");
    gtk_label_set_xalign(GTK_LABEL(left_title_label), 0.0);
    gtk_box_pack_start(GTK_BOX(left_vbox), left_title_label, FALSE, FALSE, 0);

    // Live search section
    GtkWidget *search_hbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    gtk_box_pack_start(GTK_BOX(left_vbox), search_hbox, FALSE, FALSE, 0);

    ctx->search_entry = gtk_search_entry_new();
    gtk_entry_set_placeholder_text(GTK_ENTRY(ctx->search_entry), "Search processes...");
    g_signal_connect(ctx->search_entry, "search-changed", G_CALLBACK(on_search_changed), ctx);
    gtk_box_pack_start(GTK_BOX(search_hbox), ctx->search_entry, TRUE, TRUE, 0);

    // GTK Scrolled windows to maintain smooth list browsing
    GtkWidget *scrolled_window = gtk_scrolled_window_new(NULL, NULL);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scrolled_window), GTK_POLICY_AUTOMATIC, GTK_POLICY_AUTOMATIC);
    gtk_scrolled_window_set_shadow_type(GTK_SCROLLED_WINDOW(scrolled_window), GTK_SHADOW_IN);
    gtk_box_pack_start(GTK_BOX(left_vbox), scrolled_window, TRUE, TRUE, 0);
    ctx->scrolled_window = scrolled_window;

    // Process TreeView Columns setup
    ctx->list_store = gtk_list_store_new(NUM_COLS,
                                         GDK_TYPE_PIXBUF,  // icon
                                         G_TYPE_INT,       // pid
                                         G_TYPE_STRING,    // process name
                                         G_TYPE_DOUBLE,    // rss numerical memory
                                         G_TYPE_STRING);   // formatted size (e.g. "126 MB")

    ctx->tree_view = gtk_tree_view_new_with_model(GTK_TREE_MODEL(ctx->list_store));
    g_object_unref(ctx->list_store); // let TreeView claim ownership
    gtk_tree_view_set_grid_lines(GTK_TREE_VIEW(ctx->tree_view), GTK_TREE_VIEW_GRID_LINES_HORIZONTAL);
    gtk_container_add(GTK_CONTAINER(scrolled_window), ctx->tree_view);

    // Connect selection mappings
    GtkTreeSelection *selection = gtk_tree_view_get_selection(GTK_TREE_VIEW(ctx->tree_view));
    gtk_tree_selection_set_mode(selection, GTK_SELECTION_SINGLE);
    g_signal_connect(selection, "changed", G_CALLBACK(on_selection_changed), ctx);
    g_signal_connect(ctx->tree_view, "row-activated", G_CALLBACK(on_row_activated), ctx);

    // Column 1: Icon + Name combo
    GtkTreeViewColumn *col_name = gtk_tree_view_column_new();
    gtk_tree_view_column_set_title(col_name, "Process Name");
    gtk_tree_view_column_set_resizable(col_name, TRUE);
    gtk_tree_view_column_set_expand(col_name, TRUE);

    GtkCellRenderer *renderer_icon = gtk_cell_renderer_pixbuf_new();
    gtk_tree_view_column_pack_start(col_name, renderer_icon, FALSE);
    gtk_tree_view_column_set_attributes(col_name, renderer_icon, "pixbuf", COL_ICON, NULL);

    GtkCellRenderer *renderer_name = gtk_cell_renderer_text_new();
    gtk_tree_view_column_pack_start(col_name, renderer_name, TRUE);
    gtk_tree_view_column_set_attributes(col_name, renderer_name, "text", COL_NAME, NULL);
    
    gtk_tree_view_append_column(GTK_TREE_VIEW(ctx->tree_view), col_name);

    // Column 2: PID number
    GtkCellRenderer *renderer_pid = gtk_cell_renderer_text_new();
    GtkTreeViewColumn *col_pid = gtk_tree_view_column_new_with_attributes("PID", renderer_pid, "text", COL_PID, NULL);
    gtk_tree_view_column_set_resizable(col_pid, TRUE);
    gtk_tree_view_column_set_min_width(col_pid, 75);
    gtk_tree_view_append_column(GTK_TREE_VIEW(ctx->tree_view), col_pid);

    // Column 3: RAM MB string formatter
    GtkCellRenderer *renderer_mem = gtk_cell_renderer_text_new();
    g_object_set(renderer_mem, "xalign", 1.0, NULL);
    GtkTreeViewColumn *col_mem = gtk_tree_view_column_new_with_attributes("Memory Usage", renderer_mem, "text", COL_RAM_STR, NULL);
    gtk_tree_view_column_set_resizable(col_mem, TRUE);
    gtk_tree_view_column_set_min_width(col_mem, 120);
    gtk_tree_view_append_column(GTK_TREE_VIEW(ctx->tree_view), col_mem);

    // Left bottom utilities action buttons (Kill Process trigger)
    GtkWidget *left_action_hbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_box_pack_start(GTK_BOX(left_vbox), left_action_hbox, FALSE, FALSE, 0);

    ctx->kill_btn = gtk_button_new_with_label("Force Quit Process (Kill)");
    GtkWidget *kill_icon = gtk_image_new_from_icon_name("process-stop", GTK_ICON_SIZE_BUTTON);
    gtk_button_set_image(GTK_BUTTON(ctx->kill_btn), kill_icon);
    gtk_widget_set_sensitive(ctx->kill_btn, FALSE);
    g_signal_connect(ctx->kill_btn, "clicked", G_CALLBACK(on_kill_clicked), ctx);
    gtk_box_pack_end(GTK_BOX(left_action_hbox), ctx->kill_btn, FALSE, FALSE, 0);

    // RIGHT DIVISION - Graphics and gauges
    GtkWidget *right_vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    gtk_widget_set_size_request(right_vbox, 480, -1);
    gtk_box_pack_start(GTK_BOX(main_hbox), right_vbox, FALSE, TRUE, 0);

    // Status / System RAM Summary Banner
    ctx->stats_label = gtk_label_new(NULL);
    gtk_label_set_use_markup(GTK_LABEL(ctx->stats_label), TRUE);
    gtk_label_set_xalign(GTK_LABEL(ctx->stats_label), 0.0);
    gtk_box_pack_start(GTK_BOX(right_vbox), ctx->stats_label, FALSE, FALSE, 0);

    // Drawing area plate for customizable charts
    ctx->drawing_area = gtk_drawing_area_new();
    gtk_widget_set_size_request(ctx->drawing_area, -1, 480);
    g_signal_connect(ctx->drawing_area, "draw", G_CALLBACK(on_draw_cairo), ctx);
    gtk_box_pack_start(GTK_BOX(right_vbox), ctx->drawing_area, TRUE, TRUE, 0);

    // 4. Global window footer status bar info
    ctx->status_bar = gtk_statusbar_new();
    gtk_box_pack_start(GTK_BOX(left_vbox), ctx->status_bar, FALSE, FALSE, 0);
    set_status(ctx, "Select a heavy process to force quit or search by keyword.");

    // Dynamic styling tweak: apply elegant dark visual theme styling constraints globally to widgets
    GtkCssProvider *css_provider = gtk_css_provider_new();
    const gchar *dark_style_css = 
        "window, dialog { background-color: #121824; color: #f1f5f9; }\n"
        "label { color: #f1f5f9; }\n"
        "treeview { background-color: #1a2233; color: #e2e8f0; }\n"
        "treeview:selected { background-color: #2563eb; color: #ffffff; }\n"
        "entry { background-color: #1a2233; color: #ffffff; border: 1px solid #2e3d59; border-radius: 4px; padding: 6px; }\n"
        "button { background: linear-gradient(180deg, #3b82f6 0%, #2563eb 100%); color: #ffffff; border-radius: 4px; padding: 6px 12px; font-weight: bold; border: none; }\n"
        "button:hover { background: #3b82f6; }\n"
        "button:disabled { background: #1e293b; color: #64748b; }\n"
        "scrollbar { background-color: #121824; }\n";
    
    gtk_css_provider_load_from_data(css_provider, dark_style_css, -1, NULL);
    gtk_style_context_add_provider_for_screen(gdk_screen_get_default(),
                                              GTK_STYLE_PROVIDER(css_provider),
                                              GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
    g_object_unref(css_provider);

    // Force first poll loop update
    refresh_system_stats(ctx);

    // Setup periodic polling interrupt callback (5000 milliseconds)
    g_timeout_add(5000, on_timeout_tick, ctx);

    // GTK main launch
    gtk_widget_show_all(window);
    gtk_main();

    // Loop exiting - safe structures clearout
    clear_process_list(ctx);
    if (ctx->selected_name) g_free(ctx->selected_name);
    if (ctx->search_query) g_free(ctx->search_query);
    g_slice_free(AppContext, ctx);

    return 0;
}
