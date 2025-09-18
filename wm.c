/*
 * hsdwm - floating + per-tag tiling with master layout
 *
 * Behavior:
 *   mod  +  h j k l  or  arrows       -> focus left / down / up / right
 *   mod  +  shift  +  h j k l  or arrows -> swap focused window with neighbor (sway/i3 style)
 *
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
#  define BORDER_COLOR_UNFOCUS "gray"
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

#ifndef DEFAULT_GAP
#  define DEFAULT_GAP 8
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

/* --- constants --- */
#define MOVE_CURSOR    XC_fleur
#define RESIZE_CURSOR  XC_sizing
#define MIN_WIN_W      32
#define MIN_WIN_H      24
#define MAX_WORKSPACES 9

static char *term_cmd[]  = { "xterm", NULL };
static char *dmenu_cmd[] = { "dmenu_run", NULL };

/* --- modes --- */
enum { MODE_FLOATING = 0, MODE_TILING = 1 };

/* --- client --- */
typedef struct Client {
    Window win;
    int x, y;
    unsigned int w, h;
    int workspace;
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

static int current_workspace = 0;
static int cycling = 0;

static int tag_mode[MAX_WORKSPACES];

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

/* new: improved neighbor finder + swap */
static Client *find_neighbor_in_direction(Client *cur, int dir);
static void swap_clients(Client *a, Client *b);

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

/* --- borders --- */
static void update_borders(void) {
    for (Client *c = clients; c; c = c->next) {
        if (c->workspace != current_workspace) {
            XSetWindowBorderWidth(dpy, c->win, 0);
            continue;
        }
        if (focused && c->win == focused->win) {
            XSetWindowBorderWidth(dpy, c->win, border_focus_width);
            XSetWindowBorder(dpy, c->win, border_focus_col);
        } else {
            XSetWindowBorderWidth(dpy, c->win, border_unfocus_width);
            XSetWindowBorder(dpy, c->win, border_unfocus_col);
        }
    }
}

/* --- manage / unmanage --- */
static void manage(Window w) {
    XWindowAttributes wa;
    if (!XGetWindowAttributes(dpy, w, &wa)) return;
    if (wa.override_redirect) return;
    if (w == root) return;

    Client *c = calloc(1, sizeof(Client));
    if (!c) return;
    c->win = w;
    c->workspace = current_workspace;

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

    XSelectInput(dpy, c->win, EnterWindowMask | FocusChangeMask | PropertyChangeMask | StructureNotifyMask | ButtonPressMask);

    add_client_to_list(c);

    XSetWMProtocols(dpy, c->win, &ATOM_WM_DELETE_WINDOW, 1);

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

    int gap = DEFAULT_GAP;
    int outer_gap = gap;
    int inner_gap = gap;

    int b = (int)border_unfocus_width;

    int effective_outer = outer_gap + b;

    int avail_w = rw - 2 * effective_outer;
    int avail_h = rh - 2 * effective_outer;
    if (avail_w < MIN_WIN_W) avail_w = MIN_WIN_W;
    if (avail_h < MIN_WIN_H) avail_h = MIN_WIN_H;

    if (count == 1) {
        for (Client *c = clients; c; c = c->next) if (c->workspace == ws) {
            c->x = effective_outer;
            c->y = effective_outer;
            c->w = (unsigned int)(avail_w - 2 * b);
            c->h = (unsigned int)(avail_h - 2 * b);
            XMoveResizeWindow(dpy, c->win, c->x, c->y, c->w, c->h);
        }
        return;
    }

    int master_w = (avail_w * DEFAULT_MASTER_FACTOR) / 100;
    if (master_w < MIN_WIN_W) master_w = MIN_WIN_W;

    int stack_w = avail_w - master_w - inner_gap;
    if (stack_w < MIN_WIN_W) stack_w = MIN_WIN_W;

    int idx = 0;
    int stack_count = count - 1;
    int stack_idx = 0;

    int total_stack_gap = (stack_count > 0) ? (stack_count - 1) * inner_gap : 0;
    int stack_each_h = (stack_count > 0) ? (avail_h - total_stack_gap) / stack_count : avail_h;

    for (Client *c = clients; c; c = c->next) {
        if (c->workspace != ws) continue;
        if (idx == 0) {
            c->x = effective_outer;
            c->y = effective_outer;
            c->w = (unsigned int)(master_w - 2 * b);
            c->h = (unsigned int)(avail_h - 2 * b);
            XMoveResizeWindow(dpy, c->win, c->x, c->y, c->w, c->h);
        } else {
            int ny = effective_outer + stack_idx * (stack_each_h + inner_gap);
            int nh = (stack_idx == stack_count - 1) ? (avail_h - (stack_each_h + inner_gap) * (stack_count - 1)) : stack_each_h;

            c->x = effective_outer + master_w + inner_gap;
            c->y = ny;
            c->w = (unsigned int)(stack_w - 2 * b);
            c->h = (unsigned int)(nh - 2 * b);
            XMoveResizeWindow(dpy, c->win, c->x, c->y, c->w, c->h);
            ++stack_idx;
        }
        ++idx;
    }
}

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

/* --- workspaces --- */
static void switch_workspace(int ws) {
    if (ws < 0 || ws >= MAX_WORKSPACES) return;
    if (ws == current_workspace) return;
    current_workspace = ws;

    for (Client *c = clients; c; c = c->next) {
        if (c->workspace == current_workspace) XMapWindow(dpy, c->win);
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
    update_borders();
    write_focused_workspace_file(current_workspace);
}

static void focus_window_at_pointer(void) {
    Window ret_root, ret_child;
    int root_x, root_y, win_x, win_y;
    unsigned int mask;
    if (!XQueryPointer(dpy, root, &ret_root, &ret_child, &root_x, &root_y, &win_x, &win_y, &mask)) return;
    Client *c = find_toplevel_client_from_window(ret_child ? ret_child : ret_root);
    if (c && c->workspace == current_workspace) focus_client_proper(c);
}

/* helper: compute overlap length between [a1,a2) and [b1,b2) */
static int overlap_len(int a1, int a2, int b1, int b2) {
    int lo = a1 > b1 ? a1 : b1;
    int hi = a2 < b2 ? a2 : b2;
    return (hi > lo) ? (hi - lo) : 0;
}

/* --- improved directional finder (sway/i3-like) ---
   dir:
     0 -> left
     1 -> down
     2 -> up
     3 -> right

   algorithm:
     - prefer candidates that lie in the requested half-plane (by edge).
     - among those prefer overlapping candidates (on perpendicular axis).
     - score: primary edge distance (small is better) weighted heavily,
              plus a small perpendicular distance penalty.
     - fallback to nearest-center if nothing in half-plane.
*/
static Client *find_neighbor_in_direction(Client *cur, int dir) {
    if (!clients) return NULL;

    /* pick a start focused client on current workspace */
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

        if (dir == 0) { /* left: candidate should be to the left */
            overlap_perp = overlap_len(ay1, ay2, cy1, cy2);
            if (ax2 <= cx1) {
                in_dir = 1;
                primary = (long long)(cx1 - ax2);
            } else if (overlap_perp > 0 && ax1 < cx1) {
                in_dir = 1;
                primary = 0;
            } else {
                in_dir = 0;
                primary = (long long)abs(acx - ccx);
            }
            secondary = overlap_perp > 0 ? 0 : (long long)abs(acy - ccy);
        } else if (dir == 3) { /* right */
            overlap_perp = overlap_len(ay1, ay2, cy1, cy2);
            if (ax1 >= cx2) {
                in_dir = 1;
                primary = (long long)(ax1 - cx2);
            } else if (overlap_perp > 0 && ax2 > cx2) {
                in_dir = 1;
                primary = 0;
            } else {
                in_dir = 0;
                primary = (long long)abs(acx - ccx);
            }
            secondary = overlap_perp > 0 ? 0 : (long long)abs(acy - ccy);
        } else if (dir == 2) { /* up */
            overlap_perp = overlap_len(ax1, ax2, cx1, cx2);
            if (ay2 <= cy1) {
                in_dir = 1;
                primary = (long long)(cy1 - ay2);
            } else if (overlap_perp > 0 && ay1 < cy1) {
                in_dir = 1;
                primary = 0;
            } else {
                in_dir = 0;
                primary = (long long)abs(acy - ccy);
            }
            secondary = overlap_perp > 0 ? 0 : (long long)abs(acx - ccx);
        } else { /* down */
            overlap_perp = overlap_len(ax1, ax2, cx1, cx2);
            if (ay1 >= cy2) {
                in_dir = 1;
                primary = (long long)(ay1 - cy2);
            } else if (overlap_perp > 0 && ay2 > cy2) {
                in_dir = 1;
                primary = 0;
            } else {
                in_dir = 0;
                primary = (long long)abs(acy - ccy);
            }
            secondary = overlap_perp > 0 ? 0 : (long long)abs(acx - ccx);
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

/* swap two nodes in the global clients linked list.
   only swap if both are non-null and in same workspace (safer).
   keep focused pointer unchanged (focus stays on the same window).
*/
static void swap_clients(Client *a, Client *b) {
    if (!a || !b || a == b) return;
    if (a->workspace != b->workspace) return;

    Client *a_prev = a->prev;
    Client *a_next = a->next;
    Client *b_prev = b->prev;
    Client *b_next = b->next;

    if (a_next == b) {
        if (a_prev) a_prev->next = b;
        b->prev = a_prev;

        a->next = b_next;
        if (b_next) b_next->prev = a;

        b->next = a;
        a->prev = b;
    } else if (b_next == a) {
        if (b_prev) b_prev->next = a;
        a->prev = b_prev;

        b->next = a_next;
        if (a_next) a_next->prev = b;

        a->next = b;
        b->prev = a;
    } else {
        if (a_prev) a_prev->next = b;
        if (a_next) a_next->prev = b;
        if (b_prev) b_prev->next = a;
        if (b_next) b_next->prev = a;

        a->prev = b_prev;
        a->next = b_next;
        b->prev = a_prev;
        b->next = a_next;
    }

    if (clients == a) clients = b;
    else if (clients == b) clients = a;
}

/* define the missing focus_in_direction wrapper so the prototype isn't unused */
static void focus_in_direction(int dir) {
    Client *cand = find_neighbor_in_direction(focused, dir);
    if (cand && cand->workspace == current_workspace) focus_client_proper(cand);
}

/* --- event handlers --- */
static void handle_maprequest(XEvent *ev) { manage(ev->xmaprequest.window); }
static void handle_destroynotify(XEvent *ev) { unmanage(ev->xdestroywindow.window); }
static void handle_unmapnotify(XEvent *ev) { (void)ev; }

static void handle_configurerequest(XEvent *ev) {
    XConfigureRequestEvent *e = &ev->xconfigurerequest;
    XWindowChanges changes;
    changes.x = e->x; changes.y = e->y;
    changes.width = e->width; changes.height = e->height;
    changes.border_width = e->border_width;
    changes.sibling = e->above; changes.stack_mode = e->detail;
    XConfigureWindow(dpy, e->window, e->value_mask, &changes);

    Client *c = find_client(e->window);
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
    if (c && c->workspace == current_workspace) focus_client_proper(c);
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

    focus_client_proper(c);

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
            /* swap focused with neighbor in that direction (sway-like) */
            if (!focused) return;
            Client *cand = find_neighbor_in_direction(focused, dir);
            if (cand && cand->workspace == current_workspace) {
                swap_clients(focused, cand);
                if (tag_mode[current_workspace] == MODE_TILING) tile_workspace(current_workspace);
                update_borders();
                write_occupied_workspace_file();
            }
            return;
        } else {
            /* normal focus movement */
            Client *cand = find_neighbor_in_direction(focused, dir);
            if (cand && cand->workspace == current_workspace) focus_client_proper(cand);
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
            if (XGetWindowAttributes(dpy, w, &wa) && !wa.override_redirect) manage(w);
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
    dpy = XOpenDisplay(NULL);
    if (!dpy) die("cannot open display");

    XSetErrorHandler(xerror_handler);

    screen_num = DefaultScreen(dpy);
    root = RootWindow(dpy, screen_num);

    ATOM_WM_PROTOCOLS      = XInternAtom(dpy, "WM_PROTOCOLS", False);
    ATOM_WM_DELETE_WINDOW  = XInternAtom(dpy, "WM_DELETE_WINDOW", False);

    border_focus_col   = alloc_color(BORDER_COLOR_FOCUS);
    border_unfocus_col = alloc_color(BORDER_COLOR_UNFOCUS);
    border_focus_width = BORDER_PX_FOCUSED;
    border_unfocus_width = BORDER_PX_UNFOCUSED;

    for (int i = 0; i < MAX_WORKSPACES; ++i) tag_mode[i] = (DEFAULT_TAG_MODE ? MODE_TILING : MODE_FLOATING);

    if (XSelectInput(dpy, root,
                     SubstructureRedirectMask | SubstructureNotifyMask |
                     ButtonPressMask | EnterWindowMask | PointerMotionMask | KeyReleaseMask) == BadAccess) {
        die("another window manager is running");
    }

    grab_keys_and_buttons();

    run_autolaunch();

    scan_existing_windows();

    for (int i = 0; i < MAX_WORKSPACES; ++i) if (tag_mode[i] == MODE_TILING) tile_workspace(i);

    write_focused_workspace_file(current_workspace);
    write_occupied_workspace_file();

    run_loop();

    XCloseDisplay(dpy);
    return 0;
}

