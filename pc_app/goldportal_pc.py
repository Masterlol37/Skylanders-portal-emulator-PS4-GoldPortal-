#!/usr/bin/env python3
"""Goldportal PC app: puts Skylanders .dump figures on the PS4 virtual portal.

GUI:  python goldportal_pc.py
CLI:  python goldportal_pc.py --ip 192.168.1.50 status
      python goldportal_pc.py --ip 192.168.1.50 load 1 Spyro.dump
      python goldportal_pc.py --ip 192.168.1.50 remove 1
      python goldportal_pc.py --ip 192.168.1.50 save 1 Spyro.dump

Slots are numbered 1-8 for humans (the API uses 0-7). Only the Python
standard library is used, so no pip install is needed.
"""
import argparse
import json
import os
import re
import shutil
import sys
import threading
import time
import urllib.error
import urllib.parse
import urllib.request

DEFAULT_PORT = 8085
FIGURE_SIZE = 1024
SLOTS = 8
DUMP_EXTS = (".dump", ".dmp", ".bin", ".sky")
CONFIG_PATH = os.path.join(os.path.expanduser("~"), ".goldportal.json")


# ---------------------------------------------------------------------------
# API client
# ---------------------------------------------------------------------------
class PortalError(Exception):
    pass


class PortalClient:
    def __init__(self, host, port=DEFAULT_PORT, timeout=4):
        self.base = f"http://{host}:{port}"
        self.timeout = timeout

    def _req(self, method, path, params=None, body=None):
        url = self.base + path
        if params:
            url += "?" + urllib.parse.urlencode(params)
        req = urllib.request.Request(url, data=body, method=method)
        if body is not None:
            req.add_header("Content-Type", "application/octet-stream")
        try:
            with urllib.request.urlopen(req, timeout=self.timeout) as r:
                return r.read()
        except urllib.error.HTTPError as e:
            try:
                msg = json.loads(e.read()).get("error", str(e))
            except Exception:
                msg = str(e)
            raise PortalError(msg) from None
        except (urllib.error.URLError, OSError) as e:
            raise PortalError(f"cannot reach PS4 at {self.base} ({getattr(e, 'reason', e)})") from None

    def status(self):
        return json.loads(self._req("GET", "/api/status"))

    def load(self, slot, data, name):
        if len(data) < FIGURE_SIZE:
            raise PortalError(f"{name}: file is {len(data)} bytes, a figure dump needs {FIGURE_SIZE}")
        self._req("POST", "/api/load", {"slot": slot, "name": name}, data[:FIGURE_SIZE])

    def remove(self, slot):
        self._req("POST", "/api/remove", {"slot": slot})

    def dump(self, slot):
        return self._req("GET", "/api/dump", {"slot": slot})


def figure_info(data):
    """Figure id and variant from block 1 of a dump."""
    if len(data) < 0x20:
        return None, None
    return int.from_bytes(data[0x10:0x12], "little"), int.from_bytes(data[0x1C:0x1E], "little")


def write_back(path, data, make_bak=True):
    """Writes data to path, keeping the previous version as path.bak."""
    if make_bak and os.path.exists(path):
        shutil.copy2(path, path + ".bak")
    with open(path, "wb") as f:
        f.write(data)


def natural_key(name):
    """Sort key that puts "2) New" before "10) Unreleased"."""
    return [int(part) if part.isdigit() else part for part in re.split(r"(\d+)", name.lower())]


def scan_library(root):
    """Walks a folder tree. Returns {folder: ([subfolders], [dump files])} with
    naturally sorted entries, keeping only folders that contain dumps below."""
    tree = {}

    def walk(folder):
        try:
            entries = sorted(os.scandir(folder), key=lambda e: natural_key(e.name))
        except OSError:
            return False
        subs, files = [], []
        for e in entries:
            if e.is_dir(follow_symlinks=False):
                if walk(e.path):
                    subs.append(e.path)
            elif e.is_file() and e.name.lower().endswith(DUMP_EXTS):
                files.append(e.path)
        if subs or files:
            tree[folder] = (subs, files)
            return True
        return False

    if root and os.path.isdir(root):
        walk(root)
    return tree


def index_by_name(tree):
    """File name (lower case) -> list of full paths, for finding a figure's file."""
    index = {}
    for _subs, files in tree.values():
        for p in files:
            index.setdefault(os.path.basename(p).lower(), []).append(p)
    return index


class AutoSaver:
    """Writes figures the game changed back into their .dump files.

    - The file is the one the figure was placed from, or else a unique file of
      the same name in the library.
    - The first save of a file in a session copies the original to .dump.bak;
      later saves don't touch the .bak, so it stays the pre-session version.
    - The dump is read twice, a moment apart, and only saved when both reads
      match, so a figure is never saved while the game is halfway through
      writing it.
    """

    def __init__(self, settle_seconds=1.0):
        self.slot_files = {}      # slot -> path the figure was placed from
        self.last_saved = {}      # slot -> bytes last written
        self.backed_up = set()    # paths that already got their .bak this session
        self.settle = settle_seconds
        self.lock = threading.Lock()

    def placed(self, slot, path, data):
        with self.lock:
            self.slot_files[slot] = path
            self.last_saved[slot] = bytes(data[:FIGURE_SIZE])

    def forget(self, slot):
        with self.lock:
            self.slot_files.pop(slot, None)
            self.last_saved.pop(slot, None)

    def path_for(self, slot, name, name_index):
        with self.lock:
            p = self.slot_files.get(slot)
        if p:
            return p
        matches = name_index.get((name or "").lower(), [])
        return matches[0] if len(matches) == 1 else None

    def save_slot(self, client, slot_status, name_index, force=False):
        """Saves one slot if it changed. Returns the path written, or None.

        force=True (Remove / quitting): if the game keeps writing, retry a few
        times and then save the newest data rather than lose it."""
        slot = slot_status["slot"]
        if not slot_status["loaded"] or not slot_status["dirty"]:
            return None
        path = self.path_for(slot, slot_status["name"], name_index)
        if not path:
            return None
        first = client.dump(slot)
        if self.settle:
            for _attempt in range(5 if force else 1):
                time.sleep(self.settle)
                second = client.dump(slot)
                if second == first:
                    break
                first = second
            else:
                if not force:
                    return None                   # game is writing right now: next time
        with self.lock:
            if self.last_saved.get(slot) == first:
                return None                       # nothing new since the last save
            make_bak = path not in self.backed_up
            self.backed_up.add(path)
            self.slot_files[slot] = path
            self.last_saved[slot] = first
        write_back(path, first, make_bak=make_bak)
        return path

    def save_all(self, client, name_index):
        saved = []
        for s in client.status()["slots"]:
            p = self.save_slot(client, s, name_index)
            if p:
                saved.append(p)
        return saved


OLD_CONFIG_PATH = os.path.join(os.path.expanduser("~"), ".skyportal.json")   # before the rename


def load_config():
    for path in (CONFIG_PATH, OLD_CONFIG_PATH):     # take over settings from "Sky Portal"
        try:
            with open(path, encoding="utf-8") as f:
                return json.load(f)
        except Exception:
            continue
    return {}


def save_config(cfg):
    try:
        with open(CONFIG_PATH, "w", encoding="utf-8") as f:
            json.dump(cfg, f, indent=2)
    except OSError:
        pass


# ---------------------------------------------------------------------------
# CLI
# ---------------------------------------------------------------------------
def run_cli(args):
    client = PortalClient(args.ip, args.port)
    try:
        if args.cmd == "status":
            st = client.status()
            print("Game connected to portal:", "yes" if st["active"] else "not yet")
            for s in st["slots"]:
                desc = f"{s['name']}  (id {s['id']}, variant {s['variant']}){'  *modified' if s['dirty'] else ''}" \
                    if s["loaded"] else "-"
                print(f"  slot {s['slot'] + 1}: {desc}")
        elif args.cmd == "load":
            with open(args.file, "rb") as f:
                data = f.read()
            client.load(args.slot - 1, data, os.path.basename(args.file))
            print(f"Placed {args.file} on slot {args.slot}")
        elif args.cmd == "remove":
            client.remove(args.slot - 1)
            print(f"Removed figure from slot {args.slot}")
        elif args.cmd == "save":
            write_back(args.file, client.dump(args.slot - 1))
            print(f"Saved slot {args.slot} to {args.file}")
    except (PortalError, OSError) as e:
        print("Error:", e, file=sys.stderr)
        return 1
    return 0


# ---------------------------------------------------------------------------
# GUI
# ---------------------------------------------------------------------------
def run_gui():
    import tkinter as tk
    from tkinter import ttk, filedialog, messagebox

    AUTOSAVE_MS = 30_000
    cfg = load_config()
    root = tk.Tk()
    root.title("Goldportal")
    root.minsize(940, 540)
    style = ttk.Style(root)
    style.theme_use("clam")

    THEMES = {
        "light": dict(bg="#f3f3f3", fg="#1b1b1b", field="#ffffff", sel="#3a6ee8", selfg="#ffffff",
                      muted="#6b6b6b", border="#c9c9c9", btn="#e4e4e4", btnhot="#d4d4d4",
                      accent="#3a6ee8", warn="#a35b00", ok="#1d7a34"),
        "dark": dict(bg="#1e1f22", fg="#e6e6e6", field="#2b2d31", sel="#3a6ee8", selfg="#ffffff",
                     muted="#9a9a9a", border="#3a3c41", btn="#34363b", btnhot="#43464d",
                     accent="#5b8cff", warn="#e0a040", ok="#4cc46a"),
    }

    def apply_theme(dark):
        c = THEMES["dark" if dark else "light"]
        root.configure(bg=c["bg"])
        style.configure(".", background=c["bg"], foreground=c["fg"], fieldbackground=c["field"],
                        bordercolor=c["border"], lightcolor=c["bg"], darkcolor=c["bg"],
                        troughcolor=c["bg"], insertcolor=c["fg"], focuscolor=c["accent"])
        style.configure("TLabel", background=c["bg"], foreground=c["fg"])
        style.configure("Muted.TLabel", background=c["bg"], foreground=c["muted"])
        style.configure("Ok.TLabel", background=c["bg"], foreground=c["ok"])
        style.configure("TLabelframe", background=c["bg"], bordercolor=c["border"])
        style.configure("TLabelframe.Label", background=c["bg"], foreground=c["fg"])
        style.configure("TButton", background=c["btn"], foreground=c["fg"], bordercolor=c["border"], padding=(8, 3))
        style.map("TButton", background=[("pressed", c["btnhot"]), ("active", c["btnhot"])])
        style.configure("TCheckbutton", background=c["bg"], foreground=c["fg"])
        style.map("TCheckbutton", background=[("active", c["bg"])],
                  indicatorcolor=[("selected", c["accent"]), ("!selected", c["field"])])
        style.configure("TEntry", fieldbackground=c["field"], foreground=c["fg"], bordercolor=c["border"])
        style.configure("Treeview", background=c["field"], fieldbackground=c["field"], foreground=c["fg"],
                        bordercolor=c["border"], rowheight=22)
        style.map("Treeview", background=[("selected", c["sel"])], foreground=[("selected", c["selfg"])])
        style.configure("Treeview.Heading", background=c["btn"], foreground=c["fg"], bordercolor=c["border"])
        style.map("Treeview.Heading", background=[("active", c["btnhot"])])
        style.configure("Vertical.TScrollbar", background=c["btn"], troughcolor=c["bg"],
                        bordercolor=c["bg"], arrowcolor=c["fg"])
        style.configure("Slot.TLabel", background=c["field"], foreground=c["fg"], padding=(8, 4))
        style.configure("Empty.TLabel", background=c["field"], foreground=c["muted"], padding=(8, 4))
        style.configure("Dirty.TLabel", background=c["field"], foreground=c["warn"], padding=(8, 4))
        style.configure("TPanedwindow", background=c["bg"])

    state = {"client": None, "last_status": None, "polling": False, "tree": {}, "index": {}}
    saver = AutoSaver()

    # --- top bar -------------------------------------------------------------
    top = ttk.Frame(root, padding=(10, 8))
    top.pack(fill="x")
    ttk.Label(top, text="PS4 IP:").pack(side="left")
    ip_var = tk.StringVar(value=cfg.get("ip", ""))
    ip_entry = ttk.Entry(top, textvariable=ip_var, width=18)
    ip_entry.pack(side="left", padx=4)
    conn_btn = ttk.Button(top, text="Connect")
    conn_btn.pack(side="left")
    dark_var = tk.BooleanVar(value=cfg.get("dark", False))
    ttk.Checkbutton(top, text="Dark mode", variable=dark_var,
                    command=lambda: (apply_theme(dark_var.get()), remember("dark", dark_var.get()))
                    ).pack(side="right")
    status_var = tk.StringVar(value="Not connected")
    ttk.Label(top, textvariable=status_var, style="Muted.TLabel").pack(side="left", padx=12)

    body = ttk.Panedwindow(root, orient="horizontal")
    body.pack(fill="both", expand=True, padx=10, pady=(0, 10))

    # --- figure library (folder tree) --------------------------------------
    lib = ttk.LabelFrame(body, text="My figures", padding=6)
    body.add(lib, weight=3)
    folder_var = tk.StringVar(value=cfg.get("folder", ""))
    fbar = ttk.Frame(lib)
    fbar.pack(fill="x")
    ttk.Button(fbar, text="Choose main folder…", command=lambda: choose_folder()).pack(side="left")
    ttk.Button(fbar, text="Rescan", command=lambda: rescan()).pack(side="left", padx=4)
    ttk.Label(fbar, text="Search:").pack(side="left", padx=(10, 2))
    filter_var = tk.StringVar()
    filter_entry = ttk.Entry(fbar, textvariable=filter_var, width=18)
    filter_entry.pack(side="left", fill="x", expand=True)
    ttk.Label(lib, textvariable=folder_var, style="Muted.TLabel").pack(anchor="w", pady=(4, 2))

    tframe = ttk.Frame(lib)
    tframe.pack(fill="both", expand=True)
    tree = ttk.Treeview(tframe, columns=("id",), selectmode="browse")
    tree.heading("#0", text="Figure", anchor="w")
    tree.heading("id", text="ID", anchor="w")
    tree.column("#0", width=300, stretch=True)
    tree.column("id", width=60, stretch=False)
    scroll = ttk.Scrollbar(tframe, orient="vertical", command=tree.yview)
    tree.configure(yscrollcommand=scroll.set)
    tree.pack(side="left", fill="both", expand=True)
    scroll.pack(side="right", fill="y")
    ttk.Label(lib, text="Double-click a figure to place it on the first free slot.",
              style="Muted.TLabel").pack(anchor="w", pady=(4, 0))

    # --- portal slots --------------------------------------------------------
    portal = ttk.LabelFrame(body, text="Portal", padding=6)
    body.add(portal, weight=2)
    slot_labels = []
    for i in range(SLOTS):
        row = ttk.Frame(portal)
        row.pack(fill="x", pady=2)
        ttk.Label(row, text=f"{i + 1}", width=2).pack(side="left")
        lbl = ttk.Label(row, text="empty", style="Empty.TLabel", width=28)
        lbl.pack(side="left", fill="x", expand=True, padx=(0, 4))
        slot_labels.append(lbl)
        ttk.Button(row, text="Place", width=6, command=lambda i=i: place_selected(i)).pack(side="left")
        ttk.Button(row, text="Remove", width=7, command=lambda i=i: remove_slot(i)).pack(side="left", padx=2)
        ttk.Button(row, text="Save as…", width=8, command=lambda i=i: save_slot_as(i)).pack(side="left")
    ttk.Button(portal, text="Remove all", command=lambda: remove_all()).pack(fill="x", pady=(8, 4))
    ttk.Separator(portal).pack(fill="x", pady=6)
    autosave_var = tk.BooleanVar(value=cfg.get("autosave", True))
    ttk.Checkbutton(portal, text="Autosave changed figures every 30 s",
                    variable=autosave_var, command=lambda: remember("autosave", autosave_var.get())).pack(anchor="w")
    autosave_info = tk.StringVar(value="Changed figures are also saved when you press Remove.")
    ttk.Label(portal, textvariable=autosave_info, style="Muted.TLabel", wraplength=330).pack(anchor="w", pady=(2, 0))

    # --- helpers -------------------------------------------------------------
    def remember(key, value):
        cfg[key] = value
        save_config(cfg)

    def in_bg(fn, done=None):
        """Runs network work off the UI thread, reports back on it."""
        def worker():
            try:
                res, err = fn(), None
            except Exception as e:  # noqa: BLE001 - shown to the user
                res, err = None, e
            root.after(0, lambda: done(res, err) if done else (err and show_error(err)))
        threading.Thread(target=worker, daemon=True).start()

    def show_error(err):
        status_var.set(f"Error: {err}")

    def short(path):
        return os.path.splitext(os.path.basename(path))[0]

    # --- library tree ------------------------------------------------------
    def figure_id(path):
        try:
            with open(path, "rb") as f:
                fid, _ = figure_info(f.read(0x20))
            return fid if fid is not None else ""
        except OSError:
            return ""

    def build_tree():
        tree.delete(*tree.get_children())
        data = state["tree"]
        top_folder = folder_var.get()
        query = filter_var.get().strip().lower()
        if top_folder not in data:
            return

        def add(folder, parent):
            subs, files = data[folder]
            shown = False
            for sub in subs:
                node = tree.insert(parent, "end", iid="D:" + sub, text=os.path.basename(sub), open=bool(query))
                if add(sub, node):
                    shown = True
                else:
                    tree.delete(node)
            for p in files:
                if query and query not in os.path.basename(p).lower():
                    continue
                tree.insert(parent, "end", iid="F:" + p, text=short(p), values=(figure_id(p),))
                shown = True
            return shown

        add(top_folder, "")

    def rescan():
        state["tree"] = scan_library(folder_var.get())
        state["index"] = index_by_name(state["tree"])
        build_tree()
        n = sum(len(f) for _s, f in state["tree"].values())
        if folder_var.get() and not state["client"]:
            status_var.set(f"{n} figures found - not connected")

    def choose_folder():
        d = filedialog.askdirectory(initialdir=folder_var.get() or os.path.expanduser("~"))
        if d:
            folder_var.set(os.path.normpath(d))
            remember("folder", folder_var.get())
            rescan()

    def selected_file():
        sel = tree.selection()
        if sel and sel[0].startswith("F:"):
            return sel[0][2:]
        return None

    # --- portal status -----------------------------------------------------
    def apply_status(st):
        state["last_status"] = st
        info = f"plugin {st.get('version', '?')}"
        if st.get("title"):
            info += f", {st['title']}"
        status_var.set(f"Connected ({info}) - game " +
                       ("is using the portal" if st["active"] else "has not opened the portal yet"))
        for s in st["slots"]:
            i = s["slot"]
            if i >= SLOTS:
                continue
            if s["loaded"]:
                name = os.path.splitext(s["name"])[0]
                slot_labels[i].configure(text=name + ("  (changed by game)" if s["dirty"] else ""),
                                         style="Dirty.TLabel" if s["dirty"] else "Slot.TLabel")
            else:
                slot_labels[i].configure(text="empty", style="Empty.TLabel")

    def poll():
        client = state["client"]
        if not client:
            state["polling"] = False
            return

        def done(st, err):
            if err:
                status_var.set(f"Lost connection: {err}")
            else:
                apply_status(st)
            root.after(2000, poll)
        in_bg(client.status, done)

    def connect():
        ip = ip_var.get().strip()
        if not ip:
            messagebox.showinfo("Goldportal", "Enter the IP address of your PS4 (Settings > Network > View Connection Status).")
            return
        host, _, port = ip.partition(":")
        client = PortalClient(host, int(port) if port else DEFAULT_PORT)
        status_var.set("Connecting…")

        def done(st, err):
            if err:
                status_var.set(f"Error: {err}")
                return
            state["client"] = client
            remember("ip", ip)
            apply_status(st)
            if not state["polling"]:
                state["polling"] = True
                root.after(2000, poll)
        in_bg(client.status, done)

    def require_client():
        if not state["client"]:
            messagebox.showinfo("Goldportal", "Connect to the PS4 first.")
            return None
        return state["client"]

    def slot_status(client, slot):
        return client.status()["slots"][slot]

    # --- actions -----------------------------------------------------------
    def place_file(slot, path):
        client = require_client()
        if not client:
            return
        try:
            with open(path, "rb") as f:
                data = f.read()
        except OSError as e:
            show_error(e)
            return

        def work():
            saver.save_slot(client, slot_status(client, slot), state["index"], force=True)
            client.load(slot, data, os.path.basename(path))
            saver.placed(slot, path, data)
            return client.status()

        def done(st, err):
            if err:
                show_error(err)
            else:
                apply_status(st)
        in_bg(work, done)

    def place_selected(slot):
        path = selected_file()
        if not path:
            path = filedialog.askopenfilename(
                initialdir=folder_var.get() or None,
                filetypes=[("Skylanders dumps", " ".join("*" + e for e in DUMP_EXTS)), ("All files", "*.*")])
            if not path:
                return
        place_file(slot, path)

    def on_double_click(_event):
        path = selected_file()
        if not path:
            return                          # folder: Treeview opens/closes it itself
        st = state["last_status"]
        if not st:
            require_client()
            return
        free = next((s["slot"] for s in st["slots"] if not s["loaded"]), None)
        if free is None:
            messagebox.showinfo("Goldportal", "All portal slots are full. Remove a figure first.")
            return
        place_file(free, path)

    def remove_slot(slot):
        client = require_client()
        if not client:
            return

        def work():
            saved = saver.save_slot(client, slot_status(client, slot), state["index"], force=True)
            client.remove(slot)
            saver.forget(slot)
            return client.status(), saved

        def done(res, err):
            if err:
                show_error(err)
                return
            st, saved = res
            apply_status(st)
            if saved:
                autosave_info.set(f"Saved {short(saved)} at {time.strftime('%H:%M:%S')}")
        in_bg(work, done)

    def remove_all():
        client = require_client()
        if not client:
            return

        def work():
            for s in client.status()["slots"]:
                if s["loaded"]:
                    saver.save_slot(client, s, state["index"], force=True)
                    client.remove(s["slot"])
                    saver.forget(s["slot"])
            return client.status()

        def done(st, err):
            if err:
                show_error(err)
            else:
                apply_status(st)
        in_bg(work, done)

    def save_slot_as(slot):
        client = require_client()
        if not client:
            return
        st = state["last_status"]
        name = st["slots"][slot]["name"] if st and st["slots"][slot]["loaded"] else ""
        default = saver.path_for(slot, name, state["index"])
        path = filedialog.asksaveasfilename(
            initialdir=os.path.dirname(default) if default else (folder_var.get() or None),
            initialfile=os.path.basename(default) if default else f"slot{slot + 1}.dump",
            defaultextension=".dump")
        if not path:
            return

        def done(data, err):
            if err:
                show_error(err)
            else:
                write_back(path, data)
                autosave_info.set(f"Saved slot {slot + 1} to {short(path)}")
        in_bg(lambda: client.dump(slot), done)

    # --- autosave ----------------------------------------------------------
    def autosave_tick():
        client = state["client"]
        if client and autosave_var.get():
            def done(saved, err):
                if err:
                    autosave_info.set(f"Autosave failed: {err}")
                elif saved:
                    names = ", ".join(short(p) for p in saved)
                    autosave_info.set(f"Autosaved {names} at {time.strftime('%H:%M:%S')}")
                root.after(AUTOSAVE_MS, autosave_tick)
            in_bg(lambda: saver.save_all(client, state["index"]), done)
        else:
            root.after(AUTOSAVE_MS, autosave_tick)

    def on_close():
        client = state["client"]
        if client:
            try:
                status_var.set("Saving changed figures…")
                root.update_idletasks()
                for s in client.status()["slots"]:
                    saver.save_slot(client, s, state["index"], force=True)
            except Exception as e:  # noqa: BLE001
                if not messagebox.askyesno("Goldportal", f"Could not save figures ({e}). Quit anyway?"):
                    return
        root.destroy()

    conn_btn.configure(command=connect)
    ip_entry.bind("<Return>", lambda _e: connect())
    tree.bind("<Double-1>", on_double_click)
    filter_var.trace_add("write", lambda *_: build_tree())
    root.protocol("WM_DELETE_WINDOW", on_close)

    apply_theme(dark_var.get())
    rescan()
    if cfg.get("ip"):
        root.after(200, connect)
    root.after(AUTOSAVE_MS, autosave_tick)
    root.mainloop()


def main():
    p = argparse.ArgumentParser(description="Control the Goldportal PS4 plugin")
    p.add_argument("--ip", help="PS4 IP address (CLI mode)")
    p.add_argument("--port", type=int, default=DEFAULT_PORT)
    sub = p.add_subparsers(dest="cmd")
    sub.add_parser("status")
    lp = sub.add_parser("load");   lp.add_argument("slot", type=int, choices=range(1, SLOTS + 1)); lp.add_argument("file")
    rp = sub.add_parser("remove"); rp.add_argument("slot", type=int, choices=range(1, SLOTS + 1))
    sp = sub.add_parser("save");   sp.add_argument("slot", type=int, choices=range(1, SLOTS + 1)); sp.add_argument("file")
    args = p.parse_args()

    if args.cmd:
        if not args.ip:
            p.error("--ip is required for command line use")
        sys.exit(run_cli(args))
    run_gui()


if __name__ == "__main__":
    main()
