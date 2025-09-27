/* hsdwm — dock handling tightened + dwindle layout
 *
 * added:
 *  - dwindle layout (spiral split) alongside existing master layout
 *  - workspace_layout[] and DEFAULT_LAYOUT_NAME to pick default layout by name
 *  - set_workspace_layout() / set_layout_for_all() helpers
 *
 * usage: edit DEFAULT_LAYOUT_NAME near the top or call set_workspace_layout()
 */

#ifndef BORDER_PX_FOCUSED
#  define BORDER_PX_FOCUSED 12
#endif

#ifndef BORDER_PX_UNFOCUSED
#  define BORDER_PX_UNFOCUSED 12
#endif

#ifndef BORDER_COLOR_FOCUS
#  define BORDER_COLOR_FOCUS "dodgerblue"
#endif

#ifndef BORDER_COLOR_UNFOCUS
#  define BORDER_COLOR_UNFOCUS "black"
#endif

#ifndef MOD_MAIN
#  define MOD_MAIN Mod4Mask   /* Super by default; Alt (Mod1) is also accepted */
#endif

#ifndef DEFAULT_TAG_MODE
#  define DEFAULT_TAG_MODE 0   /* 0 = FLOATING, 1 = TILING */
#endif

#ifndef DEFAULT_MASTER_FACTOR
#  define DEFAULT_MASTER_FACTOR 60  /* percent width for master area */
#endif

#ifndef DEFAULT_LAYOUT_NAME
#  define DEFAULT_LAYOUT_NAME "dwindle"  /* master  or  dwindle */
#endif

#define _POSIX_C_SOURCE 200809L

#include <X11/Xlib.h>
#include <X11/Xatom.h>
#include <X11/keysym.h>
#include <X11/cursorfont.h>
#include <X11/Xutil.h>
#include <limits.h>
#include <sys/types.h>
#include <sys/stat.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <signal.h>
#include <sys/wait.h>

/* --- constants --- */
#define MOVE_CURSOR    XC_fleur
#define RESIZE_CURSOR  XC_sizing
#define MIN_WIN_W      32
#define MIN_WIN_H      24
#define MAX_WORKSPACES 9

static char *term_cmd[]  = { "xterm", NULL };
static char *dmenu_cmd[] = { "dmenu_run", NULL };

/* --- gap configuration (edit before compile) --- */
static int gap_outer = 24;   /* outer gap on both sides */
static int gap_inner = 8;   /* gap between master and stack */

/* --- modes --- */
enum { MODE_FLOATING = 0, MODE_TILING = 1 };

/* --- layouts --- */
enum { LAYOUT_MASTER = 0, LAYOUT_DWINDLE = 1 };
static int workspace_layout[MAX_WORKSPACES];
static const char *layout_names[] = { "master", "dwindle" };

/* --- client --- */
typedef struct Client {
    Window win;
    int x, y;
    unsigned int w, h;
    int workspace; /* -1 == global (docks) */
    int is_dock;
    /* primary 4 struts */
    unsigned long strut_left;
    unsigned long strut_right;
    unsigned long strut_top;
    unsigned long strut_bottom;
    /* extended partial fields (12-cardinals) */
    unsigned long strut_left_start_y;
    unsigned long strut_left_end_y;
    unsigned long strut_right_start_y;
    unsigned long strut_right_end_y;
    unsigned long strut_top_start_x;
    unsigned long strut_top_end_x;
    unsigned long strut_bottom_start_x;
    unsigned long strut_bottom_end_x;
    struct Client *next;
    struct Client *prev;
} Client;

/* --- globals --- */
static Display *dpy;
static int screen_num;
static Window root;

static Client *clients = NULL;
static Client *focused = NULL;
static Client *cycle_start = NULL;

static unsigned long border_focus_col;
static unsigned long border_unfocus_col;
static unsigned int border_focus_width;
static unsigned int border_unfocus_width;

static Atom ATOM_WM_PROTOCOLS;
static Atom ATOM_WM_DELETE_WINDOW;

/* ewmh atoms */
static Atom NET_WM_WINDOW_TYPE;
static Atom NET_WM_WINDOW_TYPE_DOCK;
static Atom NET_WM_STRUT_PARTIAL;
static Atom NET_WM_STATE;
static Atom NET_WM_STATE_ABOVE;

static int current_workspace = 0;
static int cycling = 0;

static int tag_mode[MAX_WORKSPACES];

/* reserved area computed from docks */
static int reserved_top = 0;
static int reserved_bottom = 0;
static int reserved_left = 0;
static int reserved_right = 0;

/* --- prototypes --- */
static void spawn_program(char *const argv[]);
static void send_wm_delete(Window w);
static void toggle_fullscreen(Client *c);
static void write_focused_workspace_file(int ws_index);
static void write_occupied_workspace_file(void);
static void update_borders(void);
static Client *find_toplevel_client_from_window(Window w);
static void focus_client_proper(Client *c);
static void focus_window_at_pointer(void);
static void move_focused_to_workspace(int ws);
static void start_cycle(void);
static void cycle_focus(int forward);
static void stop_cycle(void);

static void tile_workspace(int ws);
static void set_workspace_mode(int ws, int mode);
static void set_mode_for_all(int mode);
static void focus_in_direction(int dir);

static void sigchld_handler(int sig) {
    (void)sig;
    while (waitpid(-1, NULL, WNOHANG) > 0);
}

/* new: improved neighbor finder + swap */
static Client *find_neighbor_in_direction(Client *cur, int dir);
static void swap_clients(Client *a, Client *b);
static void swap_clients_keep_focus(Client *a, Client *b); /* new helper */

/* dock helpers */
static void get_window_type_and_strut(Window w, Client *c);
static void update_global_struts(void);
static void set_dock_above_property(Window w);
static void restack_docks(void);
static void apply_dock_geometry(Client *c); /* compute & enforce geometry from strut */
static void handle_propertynotify(XEvent *ev);

/* dwindle helpers */
static void dwindle_tile(Client **arr, int start, int n, int x, int y, int w, int h, int horiz, int b, int inner_gap);
static Client **collect_workspace_clients(int ws, int *out_count);

/* --- helpers --- */
static void die(const char *msg) {
    fprintf(stderr, "wm: %s\n", msg);
    exit(EXIT_FAILURE);
}

static int xerror_handler(Display *d, XErrorEvent *ev) {
    char buf[128];
    XGetErrorText(d, ev->error_code, buf, sizeof(buf));
    fprintf(stderr, "X error: request %d, error %d: %s\n", ev->request_code, ev->error_code, buf);
    fflush(stderr);
    return 0;
}

static unsigned long alloc_color(const char *name) {
    XColor col, dummy;
    Colormap cmap = DefaultColormap(dpy, screen_num);
    if (!XAllocNamedColor(dpy, cmap, name, &col, &dummy)) {
        return BlackPixel(dpy, screen_num);
    }
    return col.pixel;
}

static void ensure_wm_dir(void) {
    const char *home = getenv("HOME");
    if (!home) return;
    char path[PATH_MAX];
    snprintf(path, sizeof(path), "%s/.wm", home);
    struct stat st;
    if (stat(path, &st) == -1) mkdir(path, 0700);
}

static void write_focused_workspace_file(int ws_index) {
    const char *home = getenv("HOME");
    if (!home) return;
    ensure_wm_dir();
    char path[PATH_MAX];
    snprintf(path, sizeof(path), "%s/.wm/focused.workspace", home);
    FILE *f = fopen(path, "w");
    if (!f) return;
    fprintf(f, "%d\n", ws_index + 1);
    fclose(f);
}

static void write_occupied_workspace_file(void) {
    const char *home = getenv("HOME");
    if (!home) return;
    ensure_wm_dir();
    char path[PATH_MAX];
    snprintf(path, sizeof(path), "%s/.wm/occupied.workspace", home);
    FILE *f = fopen(path, "w");
    if (!f) return;

    int first = 1;
    for (int w = 0; w < MAX_WORKSPACES; ++w) {
        for (Client *c = clients; c; c = c->next) {
            if (c->workspace == w) {
                if (!first) fprintf(f, ",");
                fprintf(f, "%d", w + 1);
                first = 0;
                break;
            }
        }
    }
    fprintf(f, "\n");
    fclose(f);
}

/* --- client list helpers --- */
static Client *find_client(Window w) {
    for (Client *c = clients; c; c = c->next)
        if (c->win == w) return c;
    return NULL;
}

static Client *find_toplevel_client_from_window(Window w) {
    if (!w) return NULL;
    Client *c = find_client(w);
    if (c) return c;

    Window root_ret, parent, *children = NULL;
    unsigned int nchildren = 0;
    Window cur = w;
    while (1) {
        if (!XQueryTree(dpy, cur, &root_ret, &parent, &children, &nchildren)) break;
        if (children) { XFree(children); children = NULL; }
        if (parent == 0 || parent == root) break;
        c = find_client(parent);
        if (c) return c;
        cur = parent;
    }
    return NULL;
}

static void add_client_to_list(Client *c) {
    c->next = clients;
    c->prev = NULL;
    if (clients) clients->prev = c;
    clients = c;
}

static void remove_client_from_list(Client *c) {
    if (!c) return;
    if (c->prev) c->prev->next = c->next;
    if (c->next) c->next->prev = c->prev;
    if (clients == c) clients = c->next;
}

/* --- geometry helpers --- */
static void clamp_size(unsigned int *w, unsigned int *h) {
    int sw = DisplayWidth(dpy, screen_num);
    int sh = DisplayHeight(dpy, screen_num);
    unsigned int maxw = (unsigned int)(sw * 0.95);
    unsigned int maxh = (unsigned int)(sh * 0.95);
    if (*w < MIN_WIN_W) *w = MIN_WIN_W;
    if (*h < MIN_WIN_H) *h = MIN_WIN_H;
    if (*w > maxw) *w = maxw;
    if (*h > maxh) *h = maxh;
}

/* --- dock helpers --- */

/* Read window type and full strut partial (12 cardinals).
 * Fills extended strut fields; sets c->is_dock if appropriate.
 */
static void get_window_type_and_strut(Window w, Client *c) {
    if (!c) return;
    c->is_dock = 0;
    c->strut_left = c->strut_right = c->strut_top = c->strut_bottom = 0;
    c->strut_left_start_y = c->strut_left_end_y = 0;
    c->strut_right_start_y = c->strut_right_end_y = 0;
    c->strut_top_start_x = c->strut_top_end_x = 0;
    c->strut_bottom_start_x = c->strut_bottom_end_x = 0;

    Atom actual_type = None;
    int actual_format = 0;
    unsigned long nitems = 0, bytes_after = 0;
    unsigned char *prop = NULL;

    /* read _NET_WM_WINDOW_TYPE */
    if (NET_WM_WINDOW_TYPE != None) {
        if (XGetWindowProperty(dpy, w, NET_WM_WINDOW_TYPE, 0, 64, False,
                               XA_ATOM, &actual_type, &actual_format,
                               &nitems, &bytes_after, &prop) == Success && prop) {
            Atom *atoms = (Atom *)prop;
            for (unsigned long i = 0; i < nitems; ++i) {
                if (atoms[i] == NET_WM_WINDOW_TYPE_DOCK) { c->is_dock = 1; break; }
            }
            XFree(prop); prop = NULL;
        }
    }

    /* read _NET_WM_STRUT_PARTIAL (12 cardinals) */
    if (NET_WM_STRUT_PARTIAL != None) {
        if (XGetWindowProperty(dpy, w, NET_WM_STRUT_PARTIAL, 0, 12, False,
                               XA_CARDINAL, &actual_type, &actual_format,
                               &nitems, &bytes_after, &prop) == Success && prop) {
            unsigned long *vals = (unsigned long *)prop;
            if (nitems >= 4) {
                c->strut_left   = vals[0];
                c->strut_right  = vals[1];
                c->strut_top    = vals[2];
                c->strut_bottom = vals[3];
            }
            if (nitems >= 12) {
                c->strut_left_start_y   = vals[4];
                c->strut_left_end_y     = vals[5];
                c->strut_right_start_y  = vals[6];
                c->strut_right_end_y    = vals[7];
                c->strut_top_start_x    = vals[8];
                c->strut_top_end_x      = vals[9];
                c->strut_bottom_start_x = vals[10];
                c->strut_bottom_end_x   = vals[11];
                /* if any non-zero, treat as dock */
                if (c->strut_left || c->strut_right || c->strut_top || c->strut_bottom) c->is_dock = 1;
            }
            XFree(prop); prop = NULL;
        }
    }
}

/* Compute reserved areas from all docks (keep existing semantics: max per side) */
static void update_global_struts(void) {
    reserved_top = reserved_bottom = reserved_left = reserved_right = 0;
    for (Client *c = clients; c; c = c->next) {
        if (!c->is_dock) continue;
        if ((int)c->strut_top > reserved_top) reserved_top = (int)c->strut_top;
        if ((int)c->strut_bottom > reserved_bottom) reserved_bottom = (int)c->strut_bottom;
        if ((int)c->strut_left > reserved_left) reserved_left = (int)c->strut_left;
        if ((int)c->strut_right > reserved_right) reserved_right = (int)c->strut_right;
    }
}

/* set _NET_WM_STATE _ABOVE for compositors */
static void set_dock_above_property(Window w) {
    if (NET_WM_STATE == None || NET_WM_STATE_ABOVE == None) return;
    Atom atoms[1] = { NET_WM_STATE_ABOVE };
    XChangeProperty(dpy, w, NET_WM_STATE, XA_ATOM, 32, PropModeReplace, (unsigned char *)atoms, 1);
}

/* raise all docks last so they remain on top */
static void restack_docks(void) {
    for (Client *c = clients; c; c = c->next) {
        if (c->is_dock) {
            XMapWindow(dpy, c->win);    /* ensure mapped */
            XRaiseWindow(dpy, c->win);  /* raise dock last */
        }
    }
}

/* Enforce the dock geometry derived from struts.
 * If partial fields are present, use them to size and position the dock exactly.
 * Otherwise, fallback to sizing using primary strut sizes.
 */
static void apply_dock_geometry(Client *c) {
    if (!c || !c->is_dock) return;

    int sw = DisplayWidth(dpy, screen_num);
    int sh = DisplayHeight(dpy, screen_num);

    int new_x = 0, new_y = 0, new_w = (int)c->w, new_h = (int)c->h;

    if (c->strut_top > 0) {
        new_y = 0;
        /* try top partial start/end */
        if (c->strut_top_end_x > c->strut_top_start_x) {
            new_x = (int)c->strut_top_start_x;
            new_w = (int)(c->strut_top_end_x - c->strut_top_start_x + 1);
        } else {
            /* fallback span full width minus left/right reserves */
            new_x = 0;
            new_w = sw - reserved_left - reserved_right;
        }
        new_h = (int)c->strut_top;
    } else if (c->strut_bottom > 0) {
        new_h = (int)c->strut_bottom;
        new_y = sh - new_h;
        if (c->strut_bottom_end_x > c->strut_bottom_start_x) {
            new_x = (int)c->strut_bottom_start_x;
            new_w = (int)(c->strut_bottom_end_x - c->strut_bottom_start_x + 1);
        } else {
            new_x = 0;
            new_w = sw - reserved_left - reserved_right;
        }
    } else if (c->strut_left > 0) {
        new_x = 0;
        new_w = (int)c->strut_left;
        if (c->strut_left_end_y > c->strut_left_start_y) {
            new_y = (int)c->strut_left_start_y;
            new_h = (int)(c->strut_left_end_y - c->strut_left_start_y + 1);
        } else {
            new_y = 0;
            new_h = sh - reserved_top - reserved_bottom;
        }
    } else if (c->strut_right > 0) {
        new_w = (int)c->strut_right;
        new_x = sw - new_w;
        if (c->strut_right_end_y > c->strut_right_start_y) {
            new_y = (int)c->strut_right_start_y;
            new_h = (int)(c->strut_right_end_y - c->strut_right_start_y + 1);
        } else {
            new_y = 0;
            new_h = sh - reserved_top - reserved_bottom;
        }
    } else {
        /* Not a real strut; keep current geometry (but ensure mapped/global) */
        new_x = c->x;
        new_y = c->y;
        new_w = c->w;
        new_h = c->h;
    }

    /* clamp / sanity */
    if (new_x < 0) new_x = 0;
    if (new_y < 0) new_y = 0;
    if (new_w < 1) new_w = 1;
    if (new_h < 1) new_h = 1;
    if (new_x + new_w > sw) new_w = sw - new_x;
    if (new_y + new_h > sh) new_h = sh - new_y;

    /* apply and remember */
    c->x = new_x;
    c->y = new_y;
    c->w = (unsigned int)new_w;
    c->h = (unsigned int)new_h;

    XMoveResizeWindow(dpy, c->win, c->x, c->y, c->w, c->h);
}

/* --- borders --- */
static void update_borders(void) {
    /* first, apply borders / raise focused normal windows */
    for (Client *c = clients; c; c = c->next) {
        if (c->is_dock) continue; /* skip docks for normal border logic */

        if (c->workspace != current_workspace) {
            XSetWindowBorderWidth(dpy, c->win, 0);
            continue;
        }
        if (focused && c == focused) {
            XSetWindowBorderWidth(dpy, c->win, border_focus_width);
            XSetWindowBorder(dpy, c->win, border_focus_col);
            XRaiseWindow(dpy, c->win);
        } else {
            XSetWindowBorderWidth(dpy, c->win, border_unfocus_width);
            XSetWindowBorder(dpy, c->win, border_unfocus_col);
        }
    }

    /* now ensure docks are mapped + on top */
    restack_docks();
}

/* --- manage / unmanage --- */
static void manage(Window w) {
    XWindowAttributes wa;
    if (!XGetWindowAttributes(dpy, w, &wa)) return;
    if (w == root) return;

    Client *c = calloc(1, sizeof(Client));
    if (!c) return;
    c->win = w;
    c->workspace = current_workspace;

    /* detect dock/panel + read strut (fills partials too) */
    get_window_type_and_strut(w, c);

    /* if override-redirect and not a dock -> ignore (like many tooltips) */
    if (wa.override_redirect && !c->is_dock) {
        free(c);
        return;
    }

    if (XGetWindowAttributes(dpy, w, &wa)) {
        c->w = wa.width;
        c->h = wa.height;
    } else {
        c->w = 400;
        c->h = 300;
    }

    clamp_size(&c->w, &c->h);

    int sw = DisplayWidth(dpy, screen_num);
    int sh = DisplayHeight(dpy, screen_num);
    int cx = (sw - (int)c->w) / 2;
    int cy = (sh - (int)c->h) / 2;
    c->x = cx; c->y = cy;

    XSetWindowBorderWidth(dpy, c->win, 0);
    XSetWindowBorder(dpy, c->win, border_unfocus_col);

    XMoveResizeWindow(dpy, c->win, c->x, c->y, c->w, c->h);

    /* docks get minimal events to avoid focus on Enter/PointerMotion, but we do want PropertyChange */
    if (c->is_dock) {
        XSelectInput(dpy, c->win, ExposureMask | StructureNotifyMask | PropertyChangeMask);
    } else {
        XSelectInput(dpy, c->win, EnterWindowMask | FocusChangeMask | PropertyChangeMask | StructureNotifyMask | ButtonPressMask);
    }

    add_client_to_list(c);

    XSetWMProtocols(dpy, c->win, &ATOM_WM_DELETE_WINDOW, 1);

    if (c->is_dock) {
        /* docks: visible on all workspaces, don't tile, don't take focus */
        c->workspace = -1; /* mark as global */

        /* enforce geometry derived from struts (important) */
        apply_dock_geometry(c);

        /* set above state for compositors */
        set_dock_above_property(c->win);

        /* map and raise dock */
        XMapWindow(dpy, c->win);
        XRaiseWindow(dpy, c->win);

        update_global_struts();
        update_borders();
        write_occupied_workspace_file();
        return;
    }

    if (c->workspace == current_workspace) XMapWindow(dpy, c->win);

    write_occupied_workspace_file();

    focused = c;
    XRaiseWindow(dpy, c->win);
    XSetInputFocus(dpy, c->win, RevertToPointerRoot, CurrentTime);
    update_borders();
    write_focused_workspace_file(current_workspace);

    if (tag_mode[c->workspace] == MODE_TILING) tile_workspace(c->workspace);
}

static void unmanage(Window w) {
    Client *c = find_client(w);
    if (!c) return;
    int ws = c->workspace;
    remove_client_from_list(c);
    free(c);
    write_occupied_workspace_file();

    /* recompute reserved areas if a dock was removed */
    update_global_struts();

    if (focused && !find_client(focused->win)) {
        focused = NULL;
        for (Client *cc = clients; cc; cc = cc->next) {
            if (cc->workspace == current_workspace) { focused = cc; break; }
        }
        update_borders();
        if (focused) {
            XRaiseWindow(dpy, focused->win);
            XSetInputFocus(dpy, focused->win, RevertToPointerRoot, CurrentTime);
            write_focused_workspace_file(current_workspace);
        } else {
            write_focused_workspace_file(current_workspace);
        }
    }

    if (ws >= 0 && ws < MAX_WORKSPACES && tag_mode[ws] == MODE_TILING) tile_workspace(ws);
}

/* --- move / resize --- */
static void move_client(Client *c, int start_root_x, int start_root_y, int start_x, int start_y) {
    if (!c) return;
    /* defensive: don't allow moving docks */
    if (c->is_dock) return;

    XEvent ev;
    Cursor cur = XCreateFontCursor(dpy, MOVE_CURSOR);
    XGrabPointer(dpy, root, False,
                 PointerMotionMask | ButtonReleaseMask,
                 GrabModeAsync, GrabModeAsync,
                 None, cur, CurrentTime);

    while (1) {
        XMaskEvent(dpy, PointerMotionMask | ButtonReleaseMask, &ev);
        if (ev.type == MotionNotify) {
            int nx = start_x + (ev.xmotion.x_root - start_root_x);
            int ny = start_y + (ev.xmotion.y_root - start_root_y);
            c->x = nx; c->y = ny;
            XMoveWindow(dpy, c->win, c->x, c->y);
        } else if (ev.type == ButtonRelease) break;
    }

    XUngrabPointer(dpy, CurrentTime);
    XFreeCursor(dpy, cur);
}

static void resize_client(Client *c, int start_root_x, int start_root_y, unsigned int start_w, unsigned int start_h) {
    if (!c) return;
    /* defensive: don't allow resizing docks */
    if (c->is_dock) return;

    XEvent ev;
    Cursor cur = XCreateFontCursor(dpy, RESIZE_CURSOR);
    XGrabPointer(dpy, root, False,
                 PointerMotionMask | ButtonReleaseMask,
                 GrabModeAsync, GrabModeAsync,
                 None, cur, CurrentTime);

    while (1) {
        XMaskEvent(dpy, PointerMotionMask | ButtonReleaseMask, &ev);
        if (ev.type == MotionNotify) {
            int nw = (int)start_w + (ev.xmotion.x_root - start_root_x);
            int nh = (int)start_h + (ev.xmotion.y_root - start_root_y);
            if (nw < MIN_WIN_W) nw = MIN_WIN_W;
            if (nh < MIN_WIN_H) nh = MIN_WIN_H;
            c->w = (unsigned int)nw; c->h = (unsigned int)nh;
            XResizeWindow(dpy, c->win, c->w, c->h);
        } else if (ev.type == ButtonRelease) break;
    }

    XUngrabPointer(dpy, CurrentTime);
    XFreeCursor(dpy, cur);
}

/* --- tiling --- */
static void tile_workspace(int ws) {
    if (ws < 0 || ws >= MAX_WORKSPACES) return;
    int count = 0;
    for (Client *c = clients; c; c = c->next) if (c->workspace == ws) ++count;
    if (count == 0) return;

    int rw = DisplayWidth(dpy, screen_num);
    int rh = DisplayHeight(dpy, screen_num);

    int outer_gap = gap_outer;
    int inner_gap = gap_inner;

    int b = (int)border_unfocus_width;

    /* account for reserved dock areas */
    int top_reserve = reserved_top;
    int bottom_reserve = reserved_bottom;
    int left_reserve = reserved_left;
    int right_reserve = reserved_right;

    /* effective outer gap includes border thickness to avoid overlap */
    int effective_outer = outer_gap + b;

    int avail_w = rw - 2 * effective_outer - left_reserve - right_reserve;
    int avail_h = rh - 2 * effective_outer - top_reserve - bottom_reserve;
    if (avail_w < MIN_WIN_W) avail_w = MIN_WIN_W;
    if (avail_h < MIN_WIN_H) avail_h = MIN_WIN_H;

    int origin_x = effective_outer + left_reserve;
    int origin_y = effective_outer + top_reserve;

    /* if a single client just fill area */
    if (count == 1) {
        for (Client *c = clients; c; c = c->next) if (c->workspace == ws) {
            c->x = origin_x;
            c->y = origin_y;
            c->w = (unsigned int)(avail_w - 2 * b);
            c->h = (unsigned int)(avail_h - 2 * b);
            clamp_size(&c->w, &c->h);
            XMoveResizeWindow(dpy, c->win, c->x, c->y, c->w, c->h);
        }
        return;
    }

    /* choose layout for this workspace */
    int layout = workspace_layout[ws];
    if (layout == LAYOUT_MASTER) {
        int master_w = (avail_w * DEFAULT_MASTER_FACTOR) / 100;
        if (master_w < MIN_WIN_W) master_w = MIN_WIN_W;

        int stack_w = avail_w - master_w - inner_gap;
        if (stack_w < MIN_WIN_W) stack_w = MIN_WIN_W;

        if (stack_w < MIN_WIN_W) stack_w = MIN_WIN_W;

        int idx = 0;
        int stack_count = count - 1;
        int stack_idx = 0;

        int total_stack_gap = (stack_count > 0) ? (stack_count - 1) * inner_gap : 0;
        int stack_each_h = (stack_count > 0) ? (avail_h - total_stack_gap) / stack_count : avail_h;

        for (Client *c = clients; c; c = c->next) {
            if (c->workspace != ws) continue;
            if (idx == 0) {
                c->x = origin_x;
                c->y = origin_y;
                c->w = (unsigned int)(master_w - 2 * b);
                c->h = (unsigned int)(avail_h - 2 * b);
                clamp_size(&c->w, &c->h);
                XMoveResizeWindow(dpy, c->win, c->x, c->y, c->w, c->h);
            } else {
                int ny = origin_y + stack_idx * (stack_each_h + inner_gap);
                int nh = (stack_idx == stack_count - 1) ? (avail_h - (stack_each_h + inner_gap) * (stack_count - 1)) : stack_each_h;

                c->x = origin_x + master_w + inner_gap;
                c->y = ny;
                c->w = (unsigned int)(stack_w - 2 * b);
                c->h = (unsigned int)(nh - 2 * b);
                clamp_size(&c->w, &c->h);
                XMoveResizeWindow(dpy, c->win, c->x, c->y, c->w, c->h);
                ++stack_idx;
            }
            ++idx;
        }
        return;
    }

    /* dwindle layout: spiral like alternating splits
     * we tile clients in client-list order using dwindle_tile()
     */
    int cnt = 0;
    Client **arr = collect_workspace_clients(ws, &cnt);
    if (!arr || cnt == 0) { free(arr); return; }

    dwindle_tile(arr, 0, cnt, origin_x, origin_y, avail_w, avail_h, 0, b, inner_gap);

    free(arr);
}

/* set workspace layout by index (LAYOUT_MASTER or LAYOUT_DWINDLE) */
static void set_workspace_layout(int ws, int layout) {
    if (ws < 0 || ws >= MAX_WORKSPACES) return;
    workspace_layout[ws] = (layout == LAYOUT_DWINDLE) ? LAYOUT_DWINDLE : LAYOUT_MASTER;
    if (tag_mode[ws] == MODE_TILING) tile_workspace(ws);
}

static void set_layout_for_all(int layout) {
    for (int w = 0; w < MAX_WORKSPACES; ++w) {
        workspace_layout[w] = (layout == LAYOUT_DWINDLE) ? LAYOUT_DWINDLE : LAYOUT_MASTER;
        if (tag_mode[w] == MODE_TILING) tile_workspace(w);
    }
}

/* dwindle recursive tiler
 * arr[start] is placed first in the available rect then we recurse on remaining
 * horiz == 0 -> split vertically (place first on left)
 * horiz == 1 -> split horizontally (place first on top)
 */
static void dwindle_tile(Client **arr, int start, int n, int x, int y, int w, int h, int horiz, int b, int inner_gap) {
    if (n <= 0) return;
    if (n == 1) {
        Client *c = arr[start];
        c->x = x;
        c->y = y;
        int ww = w - 2 * b;
        int hh = h - 2 * b;
        if (ww < 1) ww = 1;
        if (hh < 1) hh = 1;
        c->w = (unsigned int)ww;
        c->h = (unsigned int)hh;
        clamp_size(&c->w, &c->h);
        XMoveResizeWindow(dpy, c->win, c->x, c->y, c->w, c->h);
        return;
    }

    /* compute split amount based on DEFAULT_MASTER_FACTOR */
    if (horiz == 0) {
        int amount = (w * DEFAULT_MASTER_FACTOR) / 100;
        if (amount < MIN_WIN_W) amount = MIN_WIN_W;
        if (amount > w - (MIN_WIN_W + inner_gap)) amount = w - (MIN_WIN_W + inner_gap);
        if (amount < 1) amount = 1;

        /* place first client on left */
        Client *c = arr[start];
        c->x = x;
        c->y = y;
        int ww = amount - 2 * b;
        int hh = h - 2 * b;
        if (ww < 1) ww = 1;
        if (hh < 1) hh = 1;
        c->w = (unsigned int)ww;
        c->h = (unsigned int)hh;
        clamp_size(&c->w, &c->h);
        XMoveResizeWindow(dpy, c->win, c->x, c->y, c->w, c->h);

        /* recurse on remaining to the right, flip orientation */
        int nx = x + amount + inner_gap;
        int nw = w - amount - inner_gap;
        if (nw < MIN_WIN_W) nw = MIN_WIN_W;
        dwindle_tile(arr, start + 1, n - 1, nx, y, nw, h, 1, b, inner_gap);
    } else {
        int amount = (h * DEFAULT_MASTER_FACTOR) / 100;
        if (amount < MIN_WIN_H) amount = MIN_WIN_H;
        if (amount > h - (MIN_WIN_H + inner_gap)) amount = h - (MIN_WIN_H + inner_gap);
        if (amount < 1) amount = 1;

        /* place first client on top */
        Client *c = arr[start];
        c->x = x;
        c->y = y;
        int ww = w - 2 * b;
        int hh = amount - 2 * b;
        if (ww < 1) ww = 1;
        if (hh < 1) hh = 1;
        c->w = (unsigned int)ww;
        c->h = (unsigned int)hh;
        clamp_size(&c->w, &c->h);
        XMoveResizeWindow(dpy, c->win, c->x, c->y, c->w, c->h);

        /* recurse on remaining below, flip orientation */
        int ny = y + amount + inner_gap;
        int nh = h - amount - inner_gap;
        if (nh < MIN_WIN_H) nh = MIN_WIN_H;
        dwindle_tile(arr, start + 1, n - 1, x, ny, w, nh, 0, b, inner_gap);
    }
}

/* --- workspace / focus helpers (master/stack rules kept) --- */

static void set_workspace_mode(int ws, int mode) {
    if (ws < 0 || ws >= MAX_WORKSPACES) return;
    tag_mode[ws] = (mode == MODE_TILING) ? MODE_TILING : MODE_FLOATING;
    if (tag_mode[ws] == MODE_TILING) tile_workspace(ws);
}

static void set_mode_for_all(int mode) {
    for (int w = 0; w < MAX_WORKSPACES; ++w) {
        tag_mode[w] = (mode == MODE_TILING) ? MODE_TILING : MODE_FLOATING;
        if (tag_mode[w] == MODE_TILING) tile_workspace(w);
    }
}

static void switch_workspace(int ws) {
    if (ws < 0 || ws >= MAX_WORKSPACES) return;
    if (ws == current_workspace) return;
    current_workspace = ws;

    for (Client *c = clients; c; c = c->next) {
        if (c->workspace == current_workspace || c->workspace == -1) XMapWindow(dpy, c->win);
        else XUnmapWindow(dpy, c->win);
    }

    focused = NULL;
    for (Client *c = clients; c; c = c->next) {
        if (c->workspace == current_workspace) { focused = c; break; }
    }
    if (focused) {
        XRaiseWindow(dpy, focused->win);
        XSetInputFocus(dpy, focused->win, RevertToPointerRoot, CurrentTime);
    }

    if (tag_mode[current_workspace] == MODE_TILING) tile_workspace(current_workspace);

    update_borders();
    write_focused_workspace_file(current_workspace);
    write_occupied_workspace_file();
}

static void move_focused_to_workspace(int ws) {
    if (!focused) return;
    if (ws < 0 || ws >= MAX_WORKSPACES) return;
    focused->workspace = ws;
    if (focused->workspace != current_workspace) XUnmapWindow(dpy, focused->win);
    write_occupied_workspace_file();

    if (tag_mode[ws] == MODE_TILING) tile_workspace(ws);
    if (tag_mode[current_workspace] == MODE_TILING) tile_workspace(current_workspace);
}

/* --- Alt-Tab --- */
static void start_cycle(void) {
    if (!clients) return;
    cycling = 1;
    cycle_start = focused;
}

static void cycle_focus(int forward) {
    if (!clients || !cycling) return;

    Client *start = focused ? focused : clients;
    Client *c = start;

    do {
        c = forward ? (c->next ? c->next : clients) : (c->prev ? c->prev : NULL);
        if (!c) c = clients;
        while (c && c->workspace != current_workspace) {
            c = forward ? (c->next ? c->next : clients) : (c->prev ? c->prev : NULL);
            if (!c) c = clients;
            if (c == start) break;
        }
        if (c && c->workspace == current_workspace) break;
    } while (c && c != start);

    if (c && c != focused) {
        focused = c;
        XRaiseWindow(dpy, focused->win);
        XSetInputFocus(dpy, focused->win, RevertToPointerRoot, CurrentTime);
        update_borders();
    }
}

static void stop_cycle(void) {
    cycling = 0;
    cycle_start = NULL;
}

/* --- key grabbing --- */
static void grab_keycode_for_keysym_for_mods(KeySym keysym) {
    unsigned int bases[2] = { MOD_MAIN, Mod1Mask };
    for (int b = 0; b < 2; ++b) {
        KeyCode kc = XKeysymToKeycode(dpy, keysym);
        if (kc == 0) continue;
        unsigned int masks[] = {
            0,
            LockMask,
            Mod2Mask,
            LockMask | Mod2Mask,
            ShiftMask,
            ShiftMask | LockMask,
            ShiftMask | Mod2Mask,
            ShiftMask | LockMask | Mod2Mask
        };
        for (size_t i = 0; i < sizeof(masks)/sizeof(masks[0]); ++i)
            XGrabKey(dpy, kc, bases[b] | masks[i], root, True, GrabModeAsync, GrabModeAsync);
    }
}

static void grab_keys_and_buttons(void) {
    grab_keycode_for_keysym_for_mods(XK_Return);
    grab_keycode_for_keysym_for_mods(XK_d);
    grab_keycode_for_keysym_for_mods(XK_f);
    grab_keycode_for_keysym_for_mods(XK_Tab);
    grab_keycode_for_keysym_for_mods(XK_t);

    grab_keycode_for_keysym_for_mods(XK_h);
    grab_keycode_for_keysym_for_mods(XK_j);
    grab_keycode_for_keysym_for_mods(XK_k);
    grab_keycode_for_keysym_for_mods(XK_l);

    /* arrows too */
    grab_keycode_for_keysym_for_mods(XK_Left);
    grab_keycode_for_keysym_for_mods(XK_Down);
    grab_keycode_for_keysym_for_mods(XK_Up);
    grab_keycode_for_keysym_for_mods(XK_Right);

    for (int i = 0; i < MAX_WORKSPACES; ++i) {
        grab_keycode_for_keysym_for_mods(XK_1 + i);
    }

    KeySym french_top[MAX_WORKSPACES] = {
        XK_ampersand, XK_eacute, XK_quotedbl, XK_apostrophe, XK_parenleft,
        XK_minus, XK_egrave, XK_underscore, XK_ccedilla
    };
    for (int i = 0; i < MAX_WORKSPACES; ++i) grab_keycode_for_keysym_for_mods(french_top[i]);

    grab_keycode_for_keysym_for_mods(XK_q);
    grab_keycode_for_keysym_for_mods(XK_a);

    grab_keycode_for_keysym_for_mods(XK_e);

    unsigned int bases[2] = { MOD_MAIN, Mod1Mask };
    for (int b = 0; b < 2; ++b) {
        unsigned int base = bases[b];
        XGrabButton(dpy, Button1, base, root, True, ButtonPressMask, GrabModeAsync, GrabModeAsync, None, None);
        XGrabButton(dpy, Button3, base, root, True, ButtonPressMask, GrabModeAsync, GrabModeAsync, None, None);
    }
}

/* --- key -> workspace --- */
static int keysym_to_workspace(KeySym ks) {
    if (ks >= XK_1 && ks <= XK_9) return (int)(ks - XK_1);
    if (ks == XK_ampersand)    return 0;
    if (ks == XK_eacute)      return 1;
    if (ks == XK_quotedbl)    return 2;
    if (ks == XK_apostrophe)  return 3;
    if (ks == XK_parenleft)   return 4;
    if (ks == XK_minus)       return 5;
    if (ks == XK_egrave)      return 6;
    if (ks == XK_underscore)  return 7;
    if (ks == XK_ccedilla)    return 8;
    return -1;
}

/* --- focus helpers --- */
static void focus_client_proper(Client *c) {
    if (!c) return;
    if (c->workspace != current_workspace) return;
    if (focused && focused == c) return;
    focused = c;
    XRaiseWindow(dpy, c->win);
    XSetInputFocus(dpy, c->win, RevertToPointerRoot, CurrentTime);
    update_borders(); /* will restack docks after raising focused window */
    write_focused_workspace_file(current_workspace);
}

/* bring a window to "priority" - raise it, focus it and ensure borders */
static void make_priority(Client *c) {
    if (!c) return;
    if (c->workspace != current_workspace && c->workspace != -1) {
        /* don't automatically switch workspace here; just ignore */
    }
    focused = c;
    XMapWindow(dpy, c->win);
    XRaiseWindow(dpy, c->win);
    XSetInputFocus(dpy, c->win, RevertToPointerRoot, CurrentTime);
    update_borders();
    write_focused_workspace_file(current_workspace);
}

static void focus_window_at_pointer(void) {
    Window ret_root, ret_child;
    int root_x, root_y, win_x, win_y;
    unsigned int mask;
    if (!XQueryPointer(dpy, root, &ret_root, &ret_child, &root_x, &root_y, &win_x, &win_y, &mask)) return;
    Client *c = find_toplevel_client_from_window(ret_child ? ret_child : ret_root);
    if (!c) return;
    /* don't focus docks on pointer enter */
    if (c->is_dock) return;
    if (c && c->workspace == current_workspace) make_priority(c);
}

/* helper: compute overlap length between [a1,a2) and [b1,b2) */
static int overlap_len(int a1, int a2, int b1, int b2) {
    int lo = a1 > b1 ? a1 : b1;
    int hi = a2 < b2 ? a2 : b2;
    return (hi > lo) ? (hi - lo) : 0;
}

/* --- improved directional finder (sway/i3-like) --- */
static Client *find_neighbor_in_direction(Client *cur, int dir) {
    if (!clients) return NULL;

    Client *start = cur;
    if (!start || start->workspace != current_workspace) {
        for (start = clients; start && start->workspace != current_workspace; start = start->next);
        if (!start) return NULL;
    }
    cur = start;

    int cx1 = cur->x;
    int cy1 = cur->y;
    int cx2 = cur->x + (int)cur->w;
    int cy2 = cur->y + (int)cur->h;
    int ccx = cx1 + (int)cur->w / 2;
    int ccy = cy1 + (int)cur->h / 2;

    Client *best = NULL;
    long long best_score = LLONG_MAX;
    int found_in_dir = 0;

    for (Client *c = clients; c; c = c->next) {
        if (c->workspace != current_workspace) continue;
        if (c == cur) continue;
        if (c->is_dock) continue;

        int ax1 = c->x;
        int ay1 = c->y;
        int ax2 = c->x + (int)c->w;
        int ay2 = c->y + (int)c->h;
        int acx = ax1 + (int)c->w / 2;
        int acy = ay1 + (int)c->h / 2;

        int overlap_perp = 0;
        int in_dir = 0;
        long long primary = 0;
        long long secondary = 0;

        if (dir == 0) {
            overlap_perp = overlap_len(ay1, ay2, cy1, cy2);
            if (ax2 <= cx1) {
                in_dir = 1;
                primary = (long long)(cx1 - ax2);
            } else if (overlap_perp > 0 && ax1 < cx1) {
                in_dir = 1;
                primary = 0;
            } else {
                in_dir = 0;
                primary = (long long)llabs((long long)acx - (long long)ccx);
            }
            secondary = overlap_perp > 0 ? 0 : (long long)llabs((long long)acy - (long long)ccy);
        } else if (dir == 3) {
            overlap_perp = overlap_len(ay1, ay2, cy1, cy2);
            if (ax1 >= cx2) {
                in_dir = 1;
                primary = (long long)(ax1 - cx2);
            } else if (overlap_perp > 0 && ax2 > cx2) {
                in_dir = 1;
                primary = 0;
            } else {
                in_dir = 0;
                primary = (long long)llabs((long long)acx - (long long)ccx);
            }
            secondary = overlap_perp > 0 ? 0 : (long long)llabs((long long)acy - (long long)ccy);
        } else if (dir == 2) {
            overlap_perp = overlap_len(ax1, ax2, cx1, cx2);
            if (ay2 <= cy1) {
                in_dir = 1;
                primary = (long long)(cy1 - ay2);
            } else if (overlap_perp > 0 && ay1 < cy1) {
                in_dir = 1;
                primary = 0;
            } else {
                in_dir = 0;
                primary = (long long)llabs((long long)acy - (long long)ccy);
            }
            secondary = overlap_perp > 0 ? 0 : (long long)llabs((long long)acx - (long long)ccx);
        } else {
            overlap_perp = overlap_len(ax1, ax2, cx1, cx2);
            if (ay1 >= cy2) {
                in_dir = 1;
                primary = (long long)(ay1 - cy2);
            } else if (overlap_perp > 0 && ay2 > cy2) {
                in_dir = 1;
                primary = 0;
            } else {
                in_dir = 0;
                primary = (long long)llabs((long long)acy - (long long)ccy);
            }
            secondary = overlap_perp > 0 ? 0 : (long long)llabs((long long)acx - (long long)ccx);
        }

        long long score = primary * 100000LL + secondary * 100LL;

        if (in_dir) {
            score -= 1000000000LL;
            found_in_dir = 1;
            if (overlap_perp > 0) score -= 500000000LL;
        }

        if (!found_in_dir) {
            long long dx = (long long)(acx - ccx);
            long long dy = (long long)(acy - ccy);
            long long center_dist = dx * dx + dy * dy;
            score = center_dist;
        }

        if (score < best_score) {
            best_score = score;
            best = c;
        }
    }

    return best;
}

/* swap two nodes... (no focus side-effect here: caller decides focus) */
static void swap_clients(Client *a, Client *b) {
    if (!a || !b || a == b) return;
    if (a->workspace != b->workspace) return;

    Client *a_prev = a->prev;
    Client *a_next = a->next;
    Client *b_prev = b->prev;
    Client *b_next = b->next;

    int a_was_head = (clients == a);
    int b_was_head = (clients == b);

    if (a_next == b) {
        /* a immediately before b */
        if (a_prev) a_prev->next = b;
        b->prev = a_prev;

        a->next = b_next;
        if (b_next) b_next->prev = a;

        b->next = a;
        a->prev = b;
    } else if (b_next == a) {
        /* b immediately before a */
        if (b_prev) b_prev->next = a;
        a->prev = b_prev;

        b->next = a_next;
        if (a_next) a_next->prev = b;

        a->next = b;
        b->prev = a;
    } else {
        /* non-adjacent */
        if (a_prev) a_prev->next = b;
        if (a_next) a_next->prev = b;
        if (b_prev) b_prev->next = a;
        if (b_next) b_next->prev = a;

        a->prev = b_prev;
        a->next = b_next;
        b->prev = a_prev;
        b->next = a_next;
    }

    if (a_was_head) clients = b;
    else if (b_was_head) clients = a;

    /* NO focus change here. Caller must call make_priority() or focus_client_proper()
     * on the client that should remain focused after the swap.
     */
}

/* swap helper that grabs server, retile, update borders, then focuses the moved client
 * This reduces race conditions where focus ends up on the wrong window.
 */
static void swap_clients_keep_focus(Client *a, Client *b) {
    if (!a || !b) return;
    if (a == b) return;
    if (a->workspace != b->workspace) return;
    if (a->is_dock || b->is_dock) return;

    /* we want focus to stay on 'a' (the client the user moved). store it. */
    Client *moved = a;

    XGrabServer(dpy);

    swap_clients(a, b);

    if (tag_mode[current_workspace] == MODE_TILING) tile_workspace(current_workspace);

    update_global_struts();
    update_borders();
    write_occupied_workspace_file();

    XSync(dpy, False);

    focus_client_proper(moved);

    XSync(dpy, False);
    XUngrabServer(dpy);
}

/* helper: collect clients for a workspace into an array */
static Client **collect_workspace_clients(int ws, int *out_count) {
    int cnt = 0;
    for (Client *c = clients; c; c = c->next) if (c->workspace == ws) ++cnt;
    if (out_count) *out_count = cnt;
    if (cnt == 0) return NULL;
    Client **arr = calloc(cnt, sizeof(Client*));
    int i = 0;
    for (Client *c = clients; c; c = c->next) if (c->workspace == ws) arr[i++] = c;
    return arr;
}

/* focus_in_direction (reworked)
 * Use the geometric neighbor finder for all modes. The previous master/stack
 * special-casing caused buggy behavior; now we always pick the best geometric
 * neighbor in the requested direction. If no window is focused we pick the
 * "extreme" window on the current workspace (rightmost, leftmost, topmost,
 * bottommost) which matches the requested direction.
 */
static void focus_in_direction(int dir) {
    /* helper: pick extreme when nothing is focused */
    if (!focused) {
        Client *best = NULL;
        for (Client *c = clients; c; c = c->next) {
            if (c->workspace != current_workspace) continue;
            if (c->is_dock) continue;
            if (!best) { best = c; continue; }
            int bcx = best->x + (int)best->w / 2;
            int bcy = best->y + (int)best->h / 2;
            int ccx = c->x + (int)c->w / 2;
            int ccy = c->y + (int)c->h / 2;
            if (dir == 3) { /* right: pick max center x */
                if (ccx > bcx) best = c;
            } else if (dir == 0) { /* left: pick min center x */
                if (ccx < bcx) best = c;
            } else if (dir == 2) { /* up: pick min center y */
                if (ccy < bcy) best = c;
            } else { /* down: pick max center y */
                if (ccy > bcy) best = c;
            }
        }
        if (best) focus_client_proper(best);
        return;
    }

    /* normally, use geometric neighbor finder */
    Client *cand = find_neighbor_in_direction(focused, dir);
    if (cand && cand->workspace == current_workspace && !cand->is_dock) {
        focus_client_proper(cand);
    }
}

/* --- event handlers --- */
static void handle_maprequest(XEvent *ev) { manage(ev->xmaprequest.window); }
static void handle_destroynotify(XEvent *ev) { unmanage(ev->xdestroywindow.window); }
static void handle_unmapnotify(XEvent *ev) { (void)ev; }

static void handle_configurerequest(XEvent *ev) {
    XConfigureRequestEvent *e = &ev->xconfigurerequest;
    Client *c = find_client(e->window);

    /* If it's a dock: ignore external configure requests — the WM enforces dock geometry.
     * Otherwise allow normal configure request handling.
     */
    if (c && c->is_dock) {
        /* Re-read strut in case it changed and reapply geometry */
        get_window_type_and_strut(e->window, c);
        apply_dock_geometry(c);
        update_global_struts();
        update_borders();
        write_occupied_workspace_file();
        return;
    }

    XWindowChanges changes;
    changes.x = e->x; changes.y = e->y;
    changes.width = e->width; changes.height = e->height;
    changes.border_width = e->border_width;
    changes.sibling = e->above; changes.stack_mode = e->detail;
    XConfigureWindow(dpy, e->window, e->value_mask, &changes);

    if (c) {
        XWindowAttributes wa;
        if (XGetWindowAttributes(dpy, e->window, &wa)) {
            c->x = wa.x; c->y = wa.y; c->w = wa.width; c->h = wa.height;
            clamp_size(&c->w, &c->h);
        }
    }
}

static void handle_enternotify(XEvent *ev) {
    Window w = ev->xcrossing.window;
    Client *c = find_toplevel_client_from_window(w);
    if (c && c->workspace == current_workspace && !c->is_dock) make_priority(c);
}

static void handle_motionnotify(XEvent *ev) {
    (void)ev;
    focus_window_at_pointer();
}

static void handle_buttonpress(XEvent *ev) {
    XButtonEvent *be = &ev->xbutton;
    Window clicked = be->subwindow ? be->subwindow : be->window;
    if (!clicked) return;
    Client *c = find_toplevel_client_from_window(clicked);
    if (!c) return;

    /* For docks: ignore interactions that would move/resize/focus them.
     * If you want click-to-activate bar buttons, change this to only
     * ignore Button1/3 move/resize behavior. For now we simply ignore.
     */
    if (c->is_dock) {
        return;
    }

    make_priority(c);

    if (be->button == Button1) move_client(c, be->x_root, be->y_root, c->x, c->y);
    else if (be->button == Button3) resize_client(c, be->x_root, be->y_root, c->w, c->h);
}

static void handle_keypress(XEvent *ev) {
    XKeyEvent *ke = &ev->xkey;
    KeySym ks = XLookupKeysym(ke, 0);
    unsigned int state = ke->state & ~(LockMask | Mod2Mask);
    unsigned int mod_accept = MOD_MAIN | Mod1Mask;

    if ((state & mod_accept) && (ks == XK_q || ks == XK_a)) {
        if (focused) { send_wm_delete(focused->win); }
        return;
    }

    if ((state & mod_accept) && (ke->state & ShiftMask)) {
        int ws = keysym_to_workspace(ks);
        if (ws >= 0 && ws < MAX_WORKSPACES) { move_focused_to_workspace(ws); return; }
    }

    if ((state & mod_accept) && ks == XK_Tab) {
        if (!cycling) start_cycle();
        cycle_focus(!(ke->state & ShiftMask));
        return;
    }

    if ((state & mod_accept) && ks == XK_t) {
        if (ke->state & ShiftMask) {
            int newmode = (tag_mode[0] == MODE_TILING) ? MODE_FLOATING : MODE_TILING;
            set_mode_for_all(newmode);
        } else {
            int ws = current_workspace;
            int newmode = (tag_mode[ws] == MODE_TILING) ? MODE_FLOATING : MODE_TILING;
            set_workspace_mode(ws, newmode);
        }
        return;
    }

    /* map arrows to hjkl behavior */
    int dir = -1;
    if (ks == XK_h || ks == XK_Left) dir = 0;
    else if (ks == XK_j || ks == XK_Down) dir = 1;
    else if (ks == XK_k || ks == XK_Up) dir = 2;
    else if (ks == XK_l || ks == XK_Right) dir = 3;

    if ((state & mod_accept) && dir != -1) {
        if (ke->state & ShiftMask) {
            if (!focused) return;
            Client *cand = find_neighbor_in_direction(focused, dir);
            if (cand && cand->workspace == current_workspace && !cand->is_dock) {
                swap_clients_keep_focus(focused, cand);
            }
            return;
        } else {
            focus_in_direction(dir);
            return;
        }
    }

    if (state & mod_accept) {
        if (ks == XK_Return) { spawn_program(term_cmd); return; }
        if (ks == XK_d) { spawn_program(dmenu_cmd); return; }
        if (ks == XK_f) { if (focused) toggle_fullscreen(focused); return; }
        if ((ke->state & ShiftMask) && ks == XK_e) { XCloseDisplay(dpy); exit(EXIT_SUCCESS); }
        int ws = keysym_to_workspace(ks);
        if (ws >= 0 && ws < MAX_WORKSPACES) { switch_workspace(ws); return; }
    }
}

static void handle_keyrelease(XEvent *ev) {
    XKeyEvent *ke = &ev->xkey;
    KeySym ks = XLookupKeysym(ke, 0);
    unsigned int state = ke->state & ~(LockMask | Mod2Mask);
    unsigned int mod_accept = MOD_MAIN | Mod1Mask;

    if ((state & mod_accept) && ks == XK_Tab) {
        stop_cycle();
    }
}

static void handle_clientmessage(XEvent *ev) {
    XClientMessageEvent *cm = &ev->xclient;
    if (cm->message_type == ATOM_WM_PROTOCOLS && (Atom)cm->data.l[0] == ATOM_WM_DELETE_WINDOW) {
        unmanage(cm->window);
    }
}

/* PropertyNotify handler to detect when a dock changes its strut or type. */
static void handle_propertynotify(XEvent *ev) {
    XPropertyEvent *pe = &ev->xproperty;
    if (!pe) return;
    Client *c = find_client(pe->window);
    if (!c) return;
    if (!c->is_dock) return;

    if (pe->atom == NET_WM_STRUT_PARTIAL || pe->atom == NET_WM_WINDOW_TYPE) {
        /* re-read strut and reapply geometry */
        get_window_type_and_strut(c->win, c);
        apply_dock_geometry(c);
        update_global_struts();
        /* re-tile visible workspace(s) */
        for (int i = 0; i < MAX_WORKSPACES; ++i)
            if (tag_mode[i] == MODE_TILING) tile_workspace(i);
        update_borders();
    }
}

/* --- autolaunch / scan / loop --- */
static void run_autolaunch(void) {
    const char *home = getenv("HOME");
    if (!home) return;
    char path[PATH_MAX];
    snprintf(path, sizeof(path), "%s/.local/bin/autolaunch.sh", home);
    if (access(path, X_OK) == 0) {
        pid_t pid = fork();
        if (pid == 0) {
            if (dpy) close(ConnectionNumber(dpy));
            setsid();
            execl(path, path, (char*)NULL);
            _exit(EXIT_FAILURE);
        }
    }
}

static void scan_existing_windows(void) {
    Window root_ret, parent;
    Window *children = NULL;
    unsigned int nchildren = 0;
    if (XQueryTree(dpy, root, &root_ret, &parent, &children, &nchildren)) {
        for (unsigned int i = 0; i < nchildren; ++i) {
            Window w = children[i];
            XWindowAttributes wa;
            if (XGetWindowAttributes(dpy, w, &wa)) manage(w);
        }
        if (children) XFree(children);
    }
}

static void run_loop(void) {
    XEvent ev;
    while (1) {
        XNextEvent(dpy, &ev);
        switch (ev.type) {
            case MapRequest:       handle_maprequest(&ev); break;
            case DestroyNotify:    handle_destroynotify(&ev); break;
            case UnmapNotify:      handle_unmapnotify(&ev); break;
            case ConfigureRequest: handle_configurerequest(&ev); break;
            case EnterNotify:      handle_enternotify(&ev); break;
            case MotionNotify:     handle_motionnotify(&ev); break;
            case ButtonPress:      handle_buttonpress(&ev); break;
            case KeyPress:         handle_keypress(&ev); break;
            case KeyRelease:       handle_keyrelease(&ev); break;
            case ClientMessage:    handle_clientmessage(&ev); break;
            case PropertyNotify:   handle_propertynotify(&ev); break;
            default:               break;
        }
    }
}

/* --- core helpers --- */
static void spawn_program(char *const argv[]) {
    pid_t pid = fork();
    if (pid == 0) {
        if (dpy) close(ConnectionNumber(dpy));
        setsid();
        execvp(argv[0], argv);
        _exit(EXIT_FAILURE);
    } else if (pid < 0) {
        fprintf(stderr, "wm: spawn failed: %s\n", strerror(errno));
    }
}

static void send_wm_delete(Window w) {
    XEvent ev;
    memset(&ev, 0, sizeof(ev));
    ev.type = ClientMessage;
    ev.xclient.window = w;
    ev.xclient.message_type = ATOM_WM_PROTOCOLS;
    ev.xclient.format = 32;
    ev.xclient.data.l[0] = ATOM_WM_DELETE_WINDOW;
    ev.xclient.data.l[1] = CurrentTime;
    XSendEvent(dpy, w, False, NoEventMask, &ev);
}

static void toggle_fullscreen(Client *c) {
    if (!c) return;
    int rw = DisplayWidth(dpy, screen_num);
    int rh = DisplayHeight(dpy, screen_num);
    if (c->x == 0 && c->y == 0 && (int)c->w == rw && (int)c->h == rh) {
        int nw = rw * 2 / 3;
        int nh = rh * 2 / 3;
        int nx = (rw - nw) / 2;
        int ny = (rh - nh) / 2;
        c->x = nx; c->y = ny; c->w = nw; c->h = nh;
        XMoveResizeWindow(dpy, c->win, c->x, c->y, c->w, c->h);
    } else {
        c->x = 0; c->y = 0; c->w = rw; c->h = rh;
        XMoveResizeWindow(dpy, c->win, c->x, c->y, c->w, c->h);
    }
}

/* --- startup --- */
int main(void) {
    struct sigaction sa;
    sa.sa_handler = sigchld_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_RESTART | SA_NOCLDSTOP;
    sigaction(SIGCHLD, &sa, NULL);

    dpy = XOpenDisplay(NULL);
    if (!dpy) die("cannot open display");
    XSetErrorHandler(xerror_handler);

    screen_num = DefaultScreen(dpy);
    root = RootWindow(dpy, screen_num);

    ATOM_WM_PROTOCOLS      = XInternAtom(dpy, "WM_PROTOCOLS", False);
    ATOM_WM_DELETE_WINDOW  = XInternAtom(dpy, "WM_DELETE_WINDOW", False);

    /* intern atoms for docks/struts */
    NET_WM_WINDOW_TYPE      = XInternAtom(dpy, "_NET_WM_WINDOW_TYPE", False);
    NET_WM_WINDOW_TYPE_DOCK = XInternAtom(dpy, "_NET_WM_WINDOW_TYPE_DOCK", False);
    NET_WM_STRUT_PARTIAL    = XInternAtom(dpy, "_NET_WM_STRUT_PARTIAL", False);
    NET_WM_STATE            = XInternAtom(dpy, "_NET_WM_STATE", False);
    NET_WM_STATE_ABOVE      = XInternAtom(dpy, "_NET_WM_STATE_ABOVE", False);

    border_focus_col   = alloc_color(BORDER_COLOR_FOCUS);
    border_unfocus_col = alloc_color(BORDER_COLOR_UNFOCUS);
    border_focus_width = BORDER_PX_FOCUSED;
    border_unfocus_width = BORDER_PX_UNFOCUSED;

    for (int i = 0; i < MAX_WORKSPACES; ++i) tag_mode[i] = (DEFAULT_TAG_MODE ? MODE_TILING : MODE_FLOATING);

    /* init workspace layout from DEFAULT_LAYOUT_NAME */
    for (int i = 0; i < MAX_WORKSPACES; ++i) {
        if (strcmp(DEFAULT_LAYOUT_NAME, "dwindle") == 0) workspace_layout[i] = LAYOUT_DWINDLE;
        else workspace_layout[i] = LAYOUT_MASTER;
    }

    if (XSelectInput(dpy, root,
                     SubstructureRedirectMask | SubstructureNotifyMask |
                     ButtonPressMask | EnterWindowMask | PointerMotionMask | KeyReleaseMask) == BadAccess) {
        die("another window manager is running");
    }

    grab_keys_and_buttons();

    run_autolaunch();

    scan_existing_windows();

    /* compute reserved areas from any existing bars */
    update_global_struts();

    for (int i = 0; i < MAX_WORKSPACES; ++i) if (tag_mode[i] == MODE_TILING) tile_workspace(i);

    write_focused_workspace_file(current_workspace);
    write_occupied_workspace_file();

    run_loop();

    XCloseDisplay(dpy);
    return 0;
}

