#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
TallyCCU Pro Serial Configurator
Configure Arduino network settings via serial port

Connecting must not restart the board (it may be on air). On the Mega,
turning DTR on pulses RESET through the auto-reset capacitor:
- Windows: DTR/RTS are kept off from open(), so the board keeps running.
  Some clone drivers (CH340) may still pulse DTR; the tool then reports
  the restart when it sees the boot banner.
- Linux: the kernel turns DTR on inside open() and off on close
  (HUPCL). The first open after power-up, a USB replug, or another
  program closing the port restarts the board. HUPCL is cleared so later
  connections find DTR already on and make no edge. The warning is only
  skipped if this tool closed the port cleanly and the device node is
  unchanged since: udev recreates it on replug (new inode and ctime) and
  reads or writes by other programs move its atime/mtime (8 s steps).
  A program that only opens, sets HUPCL and closes is not detected.
- macOS: same as Linux, but the driver may restore HUPCL on every open:
  treat every connection as a possible restart.
The only sure fix on Linux/macOS is hardware: 10 uF between RESET and
GND, or cutting the RESET-EN jumper (close it again to upload firmware).

On Linux/macOS the port is also flock()ed: a second copy of this tool or
miniterm gets 'Port Busy'. Programs that do not flock are not detected.

TallyCCU_Configurator.exe is built from this file and must be rebuilt
after every change (older .exe files raise DTR and reset on connect).
Recipe: tools/TallyCCU_Configurator.spec (pinned versions in
tools/requirements-build.txt); every release attaches a fresh .exe:
  python -m pip install -r tools/requirements-build.txt
  python -m PyInstaller --clean --noconfirm tools/TallyCCU_Configurator.spec
Running the .py needs Python 3 with tkinter and pyserial
(pip install -r tools/requirements.txt).
"""

import errno
import os
import queue
import re
import sys
import threading
import time
import tkinter as tk
from tkinter import ttk, scrolledtext, messagebox, filedialog
import serial
import serial.tools.list_ports

BAUDRATE = 115200
# Commands wait this long after opening (a reset shows its banner within
# it) and, once a banner shows, until "System ready." or BOOT_TIMEOUT_S.
SETTLE_S = 3.0              # the open may have reset the board
SETTLE_QUIET_S = 1.0        # it should not have
BOOT_TIMEOUT_S = 20.0
RESET_NOTICE_S = 5.0        # a banner this soon after open = reset on open
FILL_WINDOW_S = 3.0         # Get Config: accept status lines this long
MAX_TERMINAL_LINES = 5000
MAX_COMMAND_LEN = 63        # firmware serial buffer drops the rest
RESTART_COMMANDS = ('reset', 'safemode', 'normalboot')
# Arduino, Arduino.org, CH340, FTDI, CP210x
ARDUINO_VIDS = (0x2341, 0x2A03, 0x1A86, 0x0403, 0x10C4)

STATUS_FIELDS = {'Local IP': 'ip', 'Subnet': 'subnet',
                 'Gateway': 'gateway', 'vMix IP': 'vmixip'}
_FIELD_RE = re.compile(r'^(Local IP|Subnet|Gateway|vMix IP):\s*(\S+)$')
_OCTET_RE = re.compile(r'^[0-9]{1,3}$')
_SAFEMODE_END_RE = re.compile(r'^[0-9.]+ for diagnostics\.$')
# Firmware version: 'status' and the boot print it (DOC-12)
_FIRMWARE_RE = re.compile(r'^(?:Firmware:\s*|TallyCCU Pro V)([0-9][0-9A-Za-z.\-]*)$')
FIRMWARE_UNKNOWN = "TallyCCU Pro - Firmware: press Get Config"

POSIX_RESET_WARNING = (
    "On Linux and macOS opening the serial port can restart the "
    "TallyCCU: tally and CCU stop for a few seconds.\n\n"
    "On Linux it happens on the first connection after the board powers "
    "up, after the USB cable is reconnected or after another program "
    "used the port; on macOS it can happen on every connection. "
    "Do not connect while on air.\n\n"
    "Connect anyway?")


def resource_path(name):
    """Path of a file shipped with the tool (also inside a PyInstaller .exe)."""
    dirs = [getattr(sys, '_MEIPASS', None)]
    if getattr(sys, 'frozen', False):
        dirs.append(os.path.dirname(sys.executable))
    dirs.append(os.path.dirname(os.path.abspath(__file__)))
    for d in dirs:
        if d and os.path.exists(os.path.join(d, name)):
            return os.path.join(d, name)
    return name


def validate_ip(ip):
    """True for a dotted IPv4 address with 0-255 octets."""
    parts = ip.split('.')
    if len(parts) != 4:
        return False
    return all(_OCTET_RE.match(p) and int(p) <= 255 for p in parts)


def parse_config_line(line):
    """('ip', ['192', '168', '0', '100']) for a status line, else None."""
    m = _FIELD_RE.match(line.strip())
    if not m or not validate_ip(m.group(2)):
        return None
    return STATUS_FIELDS[m.group(1)], m.group(2).split('.')


def parse_firmware_line(line):
    """The version in 'Firmware: <version>' or the boot banner, else None."""
    m = _FIRMWARE_RE.match(line.strip())
    return m.group(1) if m else None


def pick_port(ports, current=''):
    """Keep the current choice, else prefer an Arduino-like USB VID."""
    devices = [p.device for p in ports]
    if current in devices:
        return current
    for p in ports:
        if getattr(p, 'vid', None) in ARDUINO_VIDS:
            return p.device
    return devices[0] if devices else ''


def open_error_kind(exc):
    """'busy', 'denied' or None for an exception raised by open()."""
    code = getattr(exc, 'errno', None)
    text = str(exc).lower()
    # EAGAIN/EWOULDBLOCK: flock() held by another process (POSIX)
    if code in (errno.EBUSY, errno.EAGAIN, errno.EWOULDBLOCK) or \
            'resource busy' in text or 'exclusively lock' in text:
        return 'busy'
    # Windows wraps PermissionError(13, <localized text>) in the message
    if code == errno.EACCES or 'permissionerror' in text or \
            any(s in text for s in ('access is denied', 'permission denied',
                                    'permiso denegado', 'acceso denegado')):
        # A Windows COM port is exclusive: denied means in use
        return 'busy' if os.name == 'nt' else 'denied'
    return None


def _clear_hupcl(s):
    """Clear HUPCL like 'stty -hupcl'. Returns (was_set, now_off):
    was_set (or unknown) means the last close dropped DTR, so this open
    may have reset; now_off (read back) means our close keeps DTR on."""
    try:
        import termios
        fd = s.fileno()
        attrs = termios.tcgetattr(fd)
        was_set = bool(attrs[2] & termios.HUPCL)
        if was_set:
            attrs[2] &= ~termios.HUPCL
            termios.tcsetattr(fd, termios.TCSANOW, attrs)
        return was_set, not termios.tcgetattr(fd)[2] & termios.HUPCL
    except Exception:
        return True, False


def node_id(path):
    """Identity of a device node, None if unknown. Changes on replug
    (udev recreates the node) and when another program reads or writes."""
    try:
        st = os.stat(path)
    except (OSError, ValueError):
        return None
    return (st.st_rdev, st.st_ino, st.st_ctime_ns,
            st.st_mtime_ns, st.st_atime_ns)


def open_serial_no_reset(port, baudrate=BAUDRATE, timeout=1):
    """Open `port` without the DTR edge that auto-resets the board.
    Returns (serial, may_have_reset, close_keeps_dtr). See the module
    docstring."""
    s = serial.Serial()                  # created closed
    s.port = port
    s.baudrate = baudrate
    s.timeout = timeout
    s.write_timeout = 2
    if os.name == 'nt':
        # Applied by the first SetCommState inside open(): no edge
        s.dtr = False
        s.rts = False
    else:
        # flock: a second copy of this tool or miniterm fails with EAGAIN
        s.exclusive = True
    # POSIX: the kernel already turned DTR on in open(). Turning it off
    # now would only arm a reset for the next open, so leave it on.
    s.open()
    may_reset, keeps_dtr = False, False
    if os.name == 'posix':
        may_reset, keeps_dtr = _clear_hupcl(s)
    return s, may_reset, keeps_dtr


class BootWatch:
    """Says when the board takes commands: not right after the port opens
    (a reset shows its banner within the settle time) nor while it boots."""

    def __init__(self, now, settle=SETTLE_S):
        self.hold_until = now + settle
        self.booting = False

    def expect_restart(self, now):
        self.booting = True
        self.hold_until = now + BOOT_TIMEOUT_S

    def feed(self, line, now):
        """'boot' when a boot banner starts, 'ready' when it ends."""
        if line.startswith('TallyCCU Pro V'):     # first banner line
            self.expect_restart(now)
            return 'boot'
        if self.booting and (line == 'System ready.' or
                             _SAFEMODE_END_RE.match(line)):
            self.booting = False
            self.hold_until = now
            return 'ready'
        return None

    def ready(self, now):
        return now >= self.hold_until


class SerialTerminal:
    def __init__(self, root):
        self.root = root
        self.root.title("TallyCCU Pro Configurator")
        self.root.resizable(False, False)
        
        # Try to load icon if exists (.ico works on Windows only)
        try:
            self.root.iconbitmap(resource_path("tallyccu_icon.ico"))
        except Exception:
            pass
        
        self.serial_port = None
        self._reader = None
        self._reader_stop = None
        self._rx = queue.Queue()        # any thread -> Tk thread
        self._boot = None
        self._rx_tail = ''
        self._last_nl = True
        self._connected_at = 0.0
        self._restart_sent = False
        self._fill_until = 0.0
        self._filled = set()
        # Linux: port -> node_id() taken after our clean close with HUPCL off
        self._kept_dtr = {}
        self._port_name = None
        self._keeps_dtr = False
        self._history = []
        self._hist_pos = 0
        self._status_text = None
        self._lost = False
        self._closing = False
        self._poll_id = None
        self.ip_fields = {}

        # Dark theme colors
        self.bg_color = "#2d2d2d"
        self.fg_color = "#ffffff"
        self.btn_color = "#3c3c3c"
        self.success_color = "#4CAF50"
        self.error_color = "#f44336"
        self.warn_color = "#FF9800"

        self.root.configure(bg=self.bg_color)

        # Frame for port selection and connection status
        frame = ttk.LabelFrame(root, text="Serial Connection")
        frame.grid(column=0, row=0, padx=10, pady=10, sticky="ew")
        
        port_frame = ttk.Frame(frame)
        port_frame.pack(pady=10)

        ttk.Label(port_frame, text="Port:").pack(side=tk.LEFT, padx=5)
        self.port_var = tk.StringVar()
        self.port_menu = ttk.Combobox(port_frame, textvariable=self.port_var, state="readonly", width=15)
        self.port_menu.pack(side=tk.LEFT, padx=5)
        self.refresh_ports()

        ttk.Button(port_frame, text="Refresh Ports", command=self.refresh_ports).pack(side=tk.LEFT, padx=5)
        
        button_frame = ttk.Frame(frame)
        button_frame.pack(pady=5)
        self.connect_button = ttk.Button(button_frame, text="Connect", command=self.toggle_connection, width=20)
        self.connect_button.pack()
        
        status_frame = ttk.Frame(frame)
        status_frame.pack(pady=5)
        self.status_label = tk.Label(status_frame, text="Disconnected", fg=self.error_color, bg=self.bg_color, font=("Arial", 10, "bold"))
        self.status_label.pack()

        # Frame for terminal output
        terminal_frame = ttk.LabelFrame(root, text="Terminal Output")
        terminal_frame.grid(column=0, row=1, padx=10, pady=10, sticky="nsew")

        self.terminal_output = scrolledtext.ScrolledText(
            terminal_frame, 
            wrap=tk.WORD, 
            width=70, 
            height=18, 
            state='disabled',
            bg="#1e1e1e",
            fg="#00ff00",
            font=("Consolas", 9)
        )
        self.terminal_output.grid(column=0, row=0, padx=5, pady=5)

        terminal_controls = ttk.Frame(terminal_frame)
        terminal_controls.grid(column=0, row=1, pady=5)
        
        buttons_inner = ttk.Frame(terminal_controls)
        buttons_inner.pack()
        
        ttk.Button(buttons_inner, text="Clear", command=self.clear_terminal).pack(side=tk.LEFT, padx=5)
        ttk.Button(buttons_inner, text="Status", command=self.show_status).pack(side=tk.LEFT, padx=5)
        ttk.Button(buttons_inner, text="Help", command=self.show_help).pack(side=tk.LEFT, padx=5)
        ttk.Button(buttons_inner, text="Save Log", command=self.save_log).pack(side=tk.LEFT, padx=5)

        # Free command line (tally, ram, state...); Up/Down browse history
        command_line = ttk.Frame(terminal_frame)
        command_line.grid(column=0, row=2, pady=(0, 5))
        ttk.Label(command_line, text="Command:").pack(side=tk.LEFT, padx=5)
        self.command_var = tk.StringVar()
        self.command_entry = ttk.Entry(command_line, textvariable=self.command_var, width=40)
        self.command_entry.pack(side=tk.LEFT, padx=5)
        self.command_entry.bind("<Return>", lambda e: self.send_free_command())
        self.command_entry.bind("<KP_Enter>", lambda e: self.send_free_command())
        self.command_entry.bind("<Up>", lambda e: self.history_step(-1))
        self.command_entry.bind("<Down>", lambda e: self.history_step(1))
        ttk.Button(command_line, text="Send", command=self.send_free_command).pack(side=tk.LEFT, padx=5)

        # Frame for network configuration
        command_frame = ttk.LabelFrame(root, text="Network Configuration")
        command_frame.grid(column=0, row=2, padx=10, pady=10)
        
        config_inner = ttk.Frame(command_frame)
        config_inner.pack(padx=20, pady=10)

        self.create_command_row(config_inner, "Local IP:", "ip", 0)
        self.create_command_row(config_inner, "Subnet:", "subnet", 1)
        self.create_command_row(config_inner, "Gateway:", "gateway", 2)
        self.create_command_row(config_inner, "vMix IP:", "vmixip", 3)

        control_frame = ttk.Frame(config_inner)
        control_frame.grid(column=0, row=4, columnspan=9, pady=10)
        
        control_inner = ttk.Frame(control_frame)
        control_inner.pack()
        
        ttk.Button(control_inner, text="Reset Device", command=self.reset_device).pack(side=tk.LEFT, padx=5)
        ttk.Button(control_inner, text="Get Config", command=self.get_current_config).pack(side=tk.LEFT, padx=5)

        # Frame for signature and version
        info_frame = tk.Frame(root, bg=self.bg_color)
        info_frame.grid(column=0, row=3, padx=10, pady=10)
        
        self.firmware_label = tk.Label(info_frame, text=FIRMWARE_UNKNOWN,
                bg=self.bg_color, fg=self.fg_color, font=("Arial", 10, "bold"))
        self.firmware_label.pack()
        tk.Label(info_frame, text="Created by Joaquin Villodre - github.com/fiverecords", 
                bg=self.bg_color, fg="#888888", font=("Arial", 8)).pack()

        root.grid_rowconfigure(1, weight=1)
        root.grid_columnconfigure(0, weight=1)

        root.protocol("WM_DELETE_WINDOW", self.on_close)
        self._poll()

    def refresh_ports(self):
        """Refresh available serial ports"""
        try:
            ports = list(serial.tools.list_ports.comports())
            devices = [port.device for port in ports]
            self.port_menu['values'] = devices
            for gone in [p for p in self._kept_dtr if p not in devices]:
                del self._kept_dtr[gone]
            if ports:
                self.port_var.set(pick_port(ports, self.port_var.get()))
                self.update_terminal(f"Found {len(ports)} serial port(s)\n")
            else:
                self.port_var.set('')
                messagebox.showwarning("No Ports", "No serial ports detected!\n\nMake sure:\n- Arduino is connected\n- Drivers are installed")
        except Exception as e:
            messagebox.showerror("Error", f"Failed to refresh ports:\n{str(e)}")

    def toggle_connection(self):
        """Toggle serial connection"""
        if self.serial_port:
            self.disconnect()
        else:
            self.connect()

    def connect(self):
        """Connect to serial port without resetting the board"""
        port = self.port_var.get()
        if not port:
            messagebox.showerror("Error", "Please select a port first")
            return
        if self.serial_port:
            return

        if os.name != 'nt' and not self._dtr_left_on(port):
            if not messagebox.askokcancel("Connect", POSIX_RESET_WARNING):
                return
        # Valid only while closed: re-taken by a clean disconnect
        self._kept_dtr.pop(port, None)

        try:
            port_obj, may_reset, keeps_dtr = open_serial_no_reset(port)
        except serial.SerialException as e:
            kind = open_error_kind(e)
            if kind == 'busy':
                messagebox.showerror(
                    "Port Busy",
                    f"Port {port} is being used by another application.\n\n"
                    "Common causes:\n"
                    "- vMix has the Arduino Tally feature active\n"
                    "- Another serial monitor or copy of this tool is open\n"
                    "- Previous connection wasn't closed properly\n\n"
                    "Solutions:\n"
                    "1. Close vMix temporarily\n"
                    "2. Disable Arduino Tally in vMix\n"
                    "3. Close other serial monitors\n"
                    "4. Unplug and replug the Arduino"
                )
            elif kind == 'denied':
                messagebox.showerror(
                    "Permission Denied",
                    f"No permission to open {port}.\n\n"
                    "On Linux add your user to the 'dialout' group:\n"
                    "  sudo usermod -aG dialout $USER\n"
                    "then log out and back in."
                )
            else:
                messagebox.showerror("Connection Error", f"Failed to connect:\n{str(e)}")
            return
        except Exception as e:
            messagebox.showerror("Connection Error", f"Failed to connect:\n{str(e)}")
            return

        now = time.monotonic()
        self.serial_port = port_obj
        self._port_name = port
        self._keeps_dtr = keeps_dtr and sys.platform.startswith('linux')
        self._boot = BootWatch(now, SETTLE_S if may_reset else SETTLE_QUIET_S)
        self._connected_at = now
        self._restart_sent = False
        self._rx_tail = ''
        self._lost = False

        # Each reader owns its port and stop event: a late old thread
        # can never read the new connection.
        stop = threading.Event()
        reader = threading.Thread(target=self.read_from_serial, args=(port_obj, stop), daemon=True)
        self._reader, self._reader_stop = reader, stop
        reader.start()

        self.connect_button.config(text="Disconnect")
        self._refresh_status()

        self.update_terminal("=" * 60 + "\n")
        self.update_terminal(f"Connected to {port} @ {BAUDRATE} baud\n")
        self.update_terminal("=" * 60 + "\n")
        if may_reset:
            self.update_terminal(">>> The OS raised DTR when opening the port: the board may be restarting.\n")

    def _dtr_left_on(self, port):
        """Linux: our last close kept DTR on and the node is unchanged"""
        kept = self._kept_dtr.get(port)
        return kept is not None and kept == node_id(port)

    def disconnect(self, reason=None):
        """Disconnect from serial port"""
        port, stop, reader = self.serial_port, self._reader_stop, self._reader
        name, keeps_dtr = self._port_name, self._keeps_dtr
        self.serial_port = None
        self._reader = self._reader_stop = None
        self._port_name = None
        self._keeps_dtr = False
        self._boot = None
        self._fill_until = 0.0
        if not self._closing:
            self.firmware_label.config(text=FIRMWARE_UNKNOWN)

        if stop:
            stop.set()
        if port:
            try:
                port.cancel_read()
            except Exception:
                pass
            if reader and reader is not threading.current_thread():
                reader.join(timeout=0.5)
            try:
                port.close()
                closed = True
            except Exception:
                closed = False
            # Only a clean close is remembered: after 'lost' (unplugged)
            # the next node is a new device with DTR off
            if name:
                self._kept_dtr.pop(name, None)
                if closed and keeps_dtr and reason is None:
                    nid = node_id(name)
                    if nid is not None:
                        self._kept_dtr[name] = nid

        if self._closing:
            return
        self._lost = reason is not None
        self.connect_button.config(text="Connect")
        self._refresh_status()
        
        self.update_terminal("\n" + "=" * 60 + "\n")
        if reason:
            self.update_terminal(f"Connection lost: {reason}\n")
        self.update_terminal("Disconnected\n")
        self.update_terminal("=" * 60 + "\n\n")

    def on_close(self):
        """Window closed: release the port before quitting"""
        self._closing = True
        if self._poll_id:
            try:
                self.root.after_cancel(self._poll_id)
            except Exception:
                pass
            self._poll_id = None
        self.disconnect()
        self.root.destroy()

    def read_from_serial(self, port, stop):
        """Reader thread: reads `port` until `stop` is set. Never touches Tk."""
        while not stop.is_set():
            try:
                if port.in_waiting:
                    data = port.readline()
                    if data:
                        self._rx.put(('rx', port, data.decode('utf-8', errors='ignore')))
                    continue
            except Exception as e:      # unplugged, closed, driver error
                if not stop.is_set():
                    self._rx.put(('lost', port, str(e) or e.__class__.__name__))
                return
            time.sleep(0.05)

    def update_terminal(self, message):
        """Queue text for the terminal; safe from any thread"""
        self._rx.put(('text', message))
        
    def _poll(self):
        """Tk thread: drain the queue every 50 ms. Always reschedules: an
        error must not stop the terminal and the 'lost' detection."""
        self._poll_id = None
        out = []
        try:
            for _ in range(500):
                try:
                    item = self._rx.get_nowait()
                except queue.Empty:
                    break
                try:
                    self._handle(item, out)
                except Exception as e:
                    out.append(f"\n>>> Internal error: {e!r}\n")
            self._append(''.join(out))
            self._refresh_status()
        finally:
            if not self._closing:
                self._poll_id = self.root.after(50, self._poll)

    def _handle(self, item, out):
        kind, *rest = item
        if kind == 'text':
            out.append(rest[0])
        elif kind == 'rx':
            port, text = rest
            # Blank lines are skipped, as before
            if text.strip() or not self._last_nl:
                out.append(text)
                self._last_nl = text.endswith('\n')
            if port is self.serial_port:
                self._scan_rx(text)
        elif kind == 'lost':
            port, reason = rest
            if port is self.serial_port:
                self._append(''.join(out))
                out.clear()
                self.disconnect(reason)

    def _append(self, text):
        if not text:
            return
        t = self.terminal_output
        t.config(state='normal')
        t.insert(tk.END, text)
        lines = int(t.index('end-1c').split('.')[0])
        if lines > MAX_TERMINAL_LINES:
            t.delete('1.0', f'{lines - MAX_TERMINAL_LINES + 1}.0')
        t.see(tk.END)
        t.config(state='disabled')

    def _scan_rx(self, text):
        """Split received text into whole lines for _on_line"""
        lines = (self._rx_tail + text).split('\n')
        self._rx_tail = lines.pop()[-256:]
        for line in lines:
            self._on_line(line.strip())

    def _on_line(self, line):
        now = time.monotonic()
        event = self._boot.feed(line, now) if self._boot else None
        if event == 'boot':
            if not self._restart_sent and now - self._connected_at < RESET_NOTICE_S:
                self.update_terminal(">>> The board restarted when the port was opened (the USB driver raised DTR).\n")
            self._restart_sent = False
            self.update_terminal(">>> Device is booting: commands wait for 'System ready.'\n")
        elif event == 'ready':
            self.update_terminal(">>> Device ready\n")

        version = parse_firmware_line(line)
        if version:
            self.firmware_label.config(text="TallyCCU Pro - Firmware " + version)

        if now < self._fill_until:
            parsed = parse_config_line(line)
            if parsed:
                name, octets = parsed
                for var, octet in zip(self.ip_fields[name], octets):
                    var.set(octet)
                self._filled.add(name)
                if len(self._filled) == len(STATUS_FIELDS):
                    self._fill_until = 0.0
                    self.update_terminal(">>> Network fields loaded from the device\n")

    def _device_ready(self):
        return self._boot is None or self._boot.ready(time.monotonic())

    def _refresh_status(self):
        if self.serial_port:
            if self._device_ready():
                state = ("Connected", self.success_color)
            else:
                state = ("Connected - device starting...", self.warn_color)
        elif self._lost:
            state = ("Disconnected (connection lost)", self.error_color)
        else:
            state = ("Disconnected", self.error_color)
        if state != self._status_text:
            self._status_text = state
            self.status_label.config(text=state[0], fg=state[1])

    def clear_terminal(self):
        """Clear terminal output"""
        self.terminal_output.config(state='normal')
        self.terminal_output.delete(1.0, tk.END)
        self.terminal_output.config(state='disabled')
        self.update_terminal("Terminal cleared.\n")

    def save_log(self):
        """Save terminal output to file"""
        try:
            timestamp = time.strftime("%Y%m%d_%H%M%S")
            filename = filedialog.asksaveasfilename(
                title="Save Log",
                initialfile=f"TallyCCU_Log_{timestamp}.txt",
                defaultextension=".txt",
                filetypes=[("Text files", "*.txt"), ("All files", "*.*")])
            if not filename:
                return
            
            content = self.terminal_output.get(1.0, tk.END)
            
            with open(filename, 'w', encoding='utf-8') as f:
                f.write(content)
            
            messagebox.showinfo("Success", f"Log saved to:\n{filename}")
        except Exception as e:
            messagebox.showerror("Error", f"Failed to save log:\n{str(e)}")

    def show_status(self):
        """Request system status from Arduino"""
        if self.send_command_direct("status"):
            self.update_terminal("\n>>> Requesting system status...\n")

    def show_help(self):
        """Send help command to Arduino"""
        if self.send_command_direct("help"):
            self.update_terminal("\n>>> Requesting help...\n")

    def get_current_config(self):
        """Get current network configuration and fill the fields"""
        if self.send_command_direct("status"):
            self._filled = set()
            self._fill_until = time.monotonic() + FILL_WINDOW_S
            self.update_terminal("\n>>> Getting current configuration...\n")

    def send_free_command(self):
        """Send the text typed in the command line"""
        command = self.command_var.get().strip()
        if not command:
            return
        if len(command) > MAX_COMMAND_LEN or not all(' ' <= c <= '~' for c in command):
            messagebox.showerror("Invalid Command",
                                 f"Commands are plain ASCII, up to {MAX_COMMAND_LEN} characters.")
            return
        if command in RESTART_COMMANDS and not messagebox.askyesno(
                "Confirm Restart",
                f"'{command}' restarts the TallyCCU: tally and CCU stop "
                "for a few seconds.\n\nContinue?"):
            return
        if not self.send_command_direct(command):
            return
        if command in RESTART_COMMANDS:
            self._expect_restart()
        if not self._history or self._history[-1] != command:
            self._history.append(command)
            del self._history[:-50]
        self._hist_pos = len(self._history)
        self.command_var.set('')
        self.update_terminal(f"\n>>> {command}\n")

    def history_step(self, delta):
        """Up/Down in the command line"""
        if not self._history:
            return "break"
        self._hist_pos = max(0, min(len(self._history), self._hist_pos + delta))
        if self._hist_pos < len(self._history):
            self.command_var.set(self._history[self._hist_pos])
        else:
            self.command_var.set('')
        self.command_entry.icursor(tk.END)
        return "break"

    def create_command_row(self, parent, label_text, command, row):
        """Create a row for IP configuration"""
        row_frame = ttk.Frame(parent)
        row_frame.grid(column=0, row=row, columnspan=9, pady=5)
        
        ttk.Label(row_frame, text=label_text, width=12).pack(side=tk.LEFT, padx=5)

        ip_vars = [tk.StringVar() for _ in range(4)]
        ip_entries = []

        for i in range(4):
            entry = ttk.Entry(row_frame, textvariable=ip_vars[i], width=5, justify='center')
            entry.pack(side=tk.LEFT, padx=2)
            
            if i < 3:
                ttk.Label(row_frame, text=".").pack(side=tk.LEFT)
            
            ip_entries.append(entry)
            
        for i, entry in enumerate(ip_entries):
            entry.bind("<KeyPress>", lambda event, idx=i:
                       self.octet_keypress(event, ip_vars, ip_entries, idx))
            entry.bind("<KeyRelease>", lambda event, idx=i: self.validate_octet(ip_vars, idx))
            entry.bind("<KeyRelease>", lambda event, idx=i:
                       self.auto_jump(event, ip_vars, ip_entries, idx), add='+')
            
        self.ip_fields[command] = ip_vars

        ttk.Button(row_frame, text=f"Set {label_text.replace(':', '')}",
                  command=lambda: self.send_ip_command(command, ip_vars)).pack(side=tk.LEFT, padx=10)

    def octet_keypress(self, event, ip_vars, ip_entries, idx):
        """'.', ',' or space jumps to the next octet; a digit typed into a
        full octet goes on to the next one instead of making 4 digits"""
        if event.char in ('.', ',', ' ') or event.keysym == 'KP_Decimal':
            return self.jump_next(ip_entries, idx)
        if len(event.char) == 1 and '0' <= event.char <= '9':
            entry = event.widget
            value = ip_vars[idx].get()
            if len(value) >= 3 and not entry.selection_present():
                if idx < 3 and entry.index(tk.INSERT) >= len(value):
                    ip_vars[idx + 1].set(event.char)
                    ip_entries[idx + 1].focus_set()
                    ip_entries[idx + 1].icursor(tk.END)
                return "break"
        return None

    def validate_octet(self, ip_vars, idx):
        """Keep an octet to digits 0-255; a pasted address fills the row"""
        var = ip_vars[idx]
        value = var.get()
        
        if value.count('.') == 3 and validate_ip(value.strip()):
            for v, part in zip(ip_vars, value.strip().split('.')):
                v.set(part)
            return
        
        digits = ''.join(c for c in value if '0' <= c <= '9')
        if digits and int(digits) > 255:
            digits = "255"
        if digits != value:
            var.set(digits)

    def auto_jump(self, event, ip_vars, ip_entries, idx):
        """Auto-jump to next field when a typed digit completes the octet"""
        key = event.keysym[3:] if event.keysym.startswith('KP_') else event.keysym
        if idx < 3 and len(key) == 1 and key.isdigit() and len(ip_vars[idx].get()) == 3:
            self.jump_next(ip_entries, idx)
        
    def jump_next(self, ip_entries, idx):
        """Focus the next octet with its text selected"""
        if idx < 3:
            nxt = ip_entries[idx + 1]
            nxt.focus_set()
            nxt.select_range(0, tk.END)
            nxt.icursor(tk.END)
        return "break"

    def send_ip_command(self, command, ip_vars):
        """Send IP configuration command"""
        ip_address = '.'.join(var.get() for var in ip_vars)
        
        if self.validate_ip(ip_address):
            full_command = f"{command} {ip_address}"
            if self.send_command_direct(full_command):
                self.update_terminal(f"\n>>> {full_command}\n")
        else:
            messagebox.showerror("Invalid IP", "Please enter a valid IP address (0-255 for each octet)")

    def send_command_direct(self, command):
        """Send command directly to serial port"""
        port = self.serial_port
        if not port or not port.is_open:
            messagebox.showwarning("Not Connected", "Please connect to a serial port first")
            return False
        
        if not self._device_ready() and not messagebox.askyesno(
                "Device Starting",
                "The device is still starting up (waiting for 'System ready.').\n"
                "Commands sent now may be lost.\n\nSend anyway?"):
            return False

        try:
            port.write(f"{command}\n".encode('utf-8'))
            return True
        except Exception as e:
            messagebox.showerror("Send Error", f"Failed to send command:\n{str(e)}")
            return False

    def validate_ip(self, ip):
        """Validate IP address format"""
        return validate_ip(ip)
        
    def _expect_restart(self):
        self._restart_sent = True
        if self._boot:
            self._boot.expect_restart(time.monotonic())

    def reset_device(self):
        """Reset the Arduino device"""
        if not self.serial_port or not self.serial_port.is_open:
            messagebox.showwarning("Not Connected", "Please connect to a serial port first")
            return
        
        if messagebox.askyesno("Confirm Reset", 
                              "This will restart the Arduino.\n\nContinue?"):
            if not self.send_command_direct("reset"):
                return
            # The USB port stays open across the restart: no reconnect
            self._expect_restart()
            self.update_terminal("\n>>> RESET command sent\n")
            self.update_terminal(">>> Device is restarting: tally and CCU stop until 'System ready.'\n\n")

if __name__ == "__main__":
    root = tk.Tk()
    app = SerialTerminal(root)
    root.mainloop()
