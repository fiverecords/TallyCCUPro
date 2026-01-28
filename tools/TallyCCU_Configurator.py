#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
TallyCCU Pro Serial Configurator V3.6
Configure Arduino network settings via serial port
"""

import tkinter as tk
from tkinter import ttk, scrolledtext, messagebox
import serial
import serial.tools.list_ports
import threading
import time

class SerialTerminal:
    def __init__(self, root):
        self.root = root
        self.root.title("TallyCCU Pro Configurator V3.6")
        self.root.resizable(False, False)
        
        # Try to load icon if exists
        try:
            self.root.iconbitmap("tallyccu_icon.ico")
        except:
            pass
        
        self.serial_port = None
        self.running = False
        self.connection_status = False

        # Dark theme colors
        self.bg_color = "#2d2d2d"
        self.fg_color = "#ffffff"
        self.btn_color = "#3c3c3c"
        self.success_color = "#4CAF50"
        self.error_color = "#f44336"

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
        
        tk.Label(info_frame, text="TallyCCU Pro - Firmware V3.6", 
                bg=self.bg_color, fg=self.fg_color, font=("Arial", 10, "bold")).pack()
        tk.Label(info_frame, text="Created by Joaquin Villodre - github.com/fiverecords", 
                bg=self.bg_color, fg="#888888", font=("Arial", 8)).pack()

        root.grid_rowconfigure(1, weight=1)
        root.grid_columnconfigure(0, weight=1)

    def refresh_ports(self):
        """Refresh available serial ports"""
        try:
            ports = [port.device for port in serial.tools.list_ports.comports()]
            self.port_menu['values'] = ports
            if ports:
                self.port_menu.current(0)
                self.update_terminal(f"Found {len(ports)} serial port(s)\n")
            else:
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
        """Connect to serial port"""
        try:
            port = self.port_var.get()
            if not port:
                messagebox.showerror("Error", "Please select a port first")
                return
            
            try:
                self.serial_port = serial.Serial(port, 115200, timeout=1)
            except serial.SerialException as e:
                error_msg = str(e).lower()
                
                if "access is denied" in error_msg or "permission denied" in error_msg or "permiso denegado" in error_msg:
                    messagebox.showerror(
                        "Port Busy", 
                        f"Port {port} is being used by another application.\n\n"
                        "Common causes:\n"
                        "- vMix has the Arduino Tally feature active\n"
                        "- Another serial monitor is open\n"
                        "- Previous connection wasn't closed properly\n\n"
                        "Solutions:\n"
                        "1. Close vMix temporarily\n"
                        "2. Disable Arduino Tally in vMix\n"
                        "3. Close other serial monitors\n"
                        "4. Unplug and replug the Arduino"
                    )
                    return
                else:
                    raise
            
            self.running = True
            self.connection_status = True
            
            self.connect_button.config(text="Disconnect")
            self.status_label.config(text="Connected", fg=self.success_color)
            
            threading.Thread(target=self.read_from_serial, daemon=True).start()
            
            self.update_terminal("=" * 60 + "\n")
            self.update_terminal(f"Connected to {port} @ 115200 baud\n")
            self.update_terminal("=" * 60 + "\n")
            
        except Exception as e:
            messagebox.showerror("Connection Error", f"Failed to connect:\n{str(e)}")

    def disconnect(self):
        """Disconnect from serial port"""
        self.running = False
        self.connection_status = False
        
        if self.serial_port:
            self.serial_port.close()
            self.serial_port = None
            
        self.connect_button.config(text="Connect")
        self.status_label.config(text="Disconnected", fg=self.error_color)
        
        self.update_terminal("\n" + "=" * 60 + "\n")
        self.update_terminal("Disconnected\n")
        self.update_terminal("=" * 60 + "\n\n")

    def read_from_serial(self):
        """Read data from serial port in background thread"""
        while self.running:
            if self.serial_port and self.serial_port.in_waiting:
                try:
                    data = self.serial_port.readline().decode('utf-8', errors='ignore')
                    if data.strip():
                        self.update_terminal(data)
                except Exception as e:
                    print(f"Error reading serial: {e}")
            time.sleep(0.05)

    def update_terminal(self, message):
        """Update terminal output with thread-safe method"""
        def _update():
            self.terminal_output.config(state='normal')
            self.terminal_output.insert(tk.END, message)
            self.terminal_output.see(tk.END)
            self.terminal_output.config(state='disabled')
        
        self.root.after(0, _update)

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
            filename = f"TallyCCU_Log_{timestamp}.txt"
            
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
        """Get current network configuration"""
        if self.send_command_direct("status"):
            self.update_terminal("\n>>> Getting current configuration...\n")

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
            
            entry.bind("<KeyRelease>", lambda event, idx=i: self.validate_octet(ip_vars[idx]))
            
            if i < 3:
                entry.bind("<KeyRelease>", lambda event, idx=i, next_var=ip_vars[i+1]: 
                          self.auto_jump(event, ip_vars[idx], next_var))
            
            ip_entries.append(entry)

        ttk.Button(row_frame, text=f"Set {label_text.replace(':', '')}",
                  command=lambda: self.send_ip_command(command, ip_vars)).pack(side=tk.LEFT, padx=10)

    def validate_octet(self, var):
        """Validate IP octet (0-255)"""
        value = var.get()
        
        if not value.isdigit() and value != '':
            var.set(''.join(filter(str.isdigit, value)))
            return
        
        if value and int(value) > 255:
            var.set("255")

    def auto_jump(self, event, current_var, next_var):
        """Auto-jump to next field when octet is complete"""
        value = current_var.get()
        
        if len(value) == 3 or (value and int(value) >= 100):
            event.widget.tk_focusNext().focus()

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
        if not self.serial_port or not self.serial_port.is_open:
            messagebox.showwarning("Not Connected", "Please connect to a serial port first")
            return False
        
        try:
            self.serial_port.write(f"{command}\n".encode('utf-8'))
            return True
        except Exception as e:
            messagebox.showerror("Send Error", f"Failed to send command:\n{str(e)}")
            return False

    def validate_ip(self, ip):
        """Validate IP address format"""
        parts = ip.split('.')
        if len(parts) != 4:
            return False
        
        for part in parts:
            if not part:
                return False
            if not part.isdigit():
                return False
            if not (0 <= int(part) <= 255):
                return False
        
        return True

    def reset_device(self):
        """Reset the Arduino device"""
        if not self.serial_port or not self.serial_port.is_open:
            messagebox.showwarning("Not Connected", "Please connect to a serial port first")
            return
        
        if messagebox.askyesno("Confirm Reset", 
                              "This will restart the Arduino.\n\nContinue?"):
            self.send_command_direct("reset")
            self.update_terminal("\n>>> RESET command sent\n")
            self.update_terminal(">>> Device will restart in 2 seconds...\n\n")
            
            self.root.after(3000, self.auto_reconnect)

    def auto_reconnect(self):
        """Automatically reconnect after reset"""
        if not self.connection_status:
            self.update_terminal(">>> Attempting to reconnect...\n")
            self.connect()

if __name__ == "__main__":
    root = tk.Tk()
    app = SerialTerminal(root)
    root.mainloop()
