import customtkinter as ctk
from tkinter import filedialog
import struct
import os
import threading
import json
import can  # Wymaga: pip install python-can

# --- KONFIGURACJA GUI ---
ctk.set_appearance_mode("System")
ctk.set_default_color_theme("blue")

# --- FORMAT STRUKTURY C (ESP32) ---
STRUCT_FMT = '<I B I B 64s'
FRAME_SIZE = struct.calcsize(STRUCT_FMT)
DLC_TO_LEN = [0, 1, 2, 3, 4, 5, 6, 7, 8, 12, 16, 20, 24, 32, 48, 64]

CONFIG_FILE = "converter_config.json"

# --- ZARZĄDZANIE KONFIGURACJĄ ---
def load_config():
    try:
        with open(CONFIG_FILE, "r") as f:
            return json.load(f)
    except Exception:
        # Domyślne ścieżki w przypadku braku pliku
        return {"last_in_dir": "E:\\", "last_out_dir": os.path.expanduser("~\\Documents")}

def save_config(config_dict):
    try:
        with open(CONFIG_FILE, "w") as f:
            json.dump(config_dict, f)
    except Exception as e:
        print(f"Failed to save config: {e}")

# Inicjalizacja konfiguracji
app_config = load_config()

# --- WĄTEK KONWERSJI ---
def convert_to_asc_thread(input_filepath, output_dir):
    # Generowanie nazwy pliku wyjściowego w wybranym folderze docelowym
    base_name = os.path.basename(input_filepath)
    out_name = os.path.splitext(base_name)[0] + '.asc'
    output_filepath = os.path.join(output_dir, out_name)
    
    def log_to_gui(text):
        def update_text():
            text_area.configure(state="normal")
            text_area.insert("end", text)
            text_area.see("end")
            text_area.configure(state="disabled")
        app.after(0, update_text)

    log_to_gui(f"Opening raw file:\n{input_filepath}\n")
    log_to_gui(f"Destination:\n{output_filepath}\n\n")
    log_to_gui("Converting to ASC format...\nHigh-performance mode running.\n")

    try:
        # Jeśli folder docelowy nie istnieje, stwórz go
        if not os.path.exists(output_dir):
            os.makedirs(output_dir)

        count = 0
        FRAMES_PER_CHUNK = 100000 
        CHUNK_SIZE = FRAME_SIZE * FRAMES_PER_CHUNK
        
        Message = can.Message
        unpack_iter = struct.iter_unpack
        fmt = STRUCT_FMT
        dlc_table = DLC_TO_LEN
        
        with can.io.ASCWriter(output_filepath) as writer, open(input_filepath, 'rb') as f:
            write_msg = writer.on_message_received 
            
            while True:
                chunk = f.read(CHUNK_SIZE)
                if not chunk:
                    break
                
                valid_length = (len(chunk) // FRAME_SIZE) * FRAME_SIZE
                if valid_length == 0:
                    break
                    
                chunk = chunk[:valid_length]
                
                for ts_ms, node, raw_id, dlc, payload in unpack_iter(fmt, chunk):
                    safe_dlc = dlc if dlc <= 15 else 0
                    
                    write_msg(Message(
                        timestamp=ts_ms / 1000.0,
                        arbitration_id=raw_id & 0x1FFFFFFF,
                        is_extended_id=bool(raw_id & 0x80000000),
                        is_rx=True,
                        channel=node - 1, 
                        is_fd=True,          
                        bitrate_switch=True,
                        data=payload[:dlc_table[safe_dlc]]
                    ))
                
                frames_in_chunk = valid_length // FRAME_SIZE
                count += frames_in_chunk
                
                log_to_gui(f"Processed {count:,} frames...\n")
                
        log_to_gui(f"\n✅ SUCCESS!\nDecoded and saved {count:,} CAN FD frames.\n")
        log_to_gui(f"File ready for SavvyCAN:\n{output_filepath}\n")
        
    except Exception as e:
        log_to_gui(f"\n❌ An error occurred:\n{e}\n")
    finally:
        app.after(0, lambda: btn_open.configure(state="normal", text="Select .bin file to convert"))

# --- AKCJE PRZYCISKÓW ---
def change_output_dir():
    current_out = out_var.get()
    new_dir = filedialog.askdirectory(initialdir=current_out, title="Select Output Folder")
    if new_dir:
        out_var.set(new_dir)
        app_config["last_out_dir"] = new_dir
        save_config(app_config)

def open_file():
    last_in = app_config.get("last_in_dir", "E:\\")
    if not os.path.exists(last_in):
        last_in = "C:\\"

    filepath = filedialog.askopenfilename(
        initialdir=last_in, 
        title="Select a .bin file to convert",
        filetypes=(("CAN binary files", "*.bin"), ("All files", "*.*"))
    )
    
    if filepath:
        # Zapisz skąd pobrano plik wejściowy
        app_config["last_in_dir"] = os.path.dirname(filepath)
        
        # Upewnij się, że pole docelowe jest też zapisane (na wypadek ręcznej zmiany tekstu)
        output_dir = out_var.get()
        app_config["last_out_dir"] = output_dir
        save_config(app_config)
        
        text_area.configure(state="normal")
        text_area.delete("1.0", "end")
        text_area.configure(state="disabled")
        
        btn_open.configure(state="disabled", text="Processing... Please wait") 
        
        thread = threading.Thread(target=convert_to_asc_thread, args=(filepath, output_dir), daemon=True)
        thread.start()

# --- BUDOWA GŁÓWNEGO OKNA ---
app = ctk.CTk()
app.title("CAN FD -> SavvyCAN (.asc) Converter")
app.geometry("750x550")

title_label = ctk.CTkLabel(app, text="High-Performance CAN Log Converter", font=ctk.CTkFont(size=22, weight="bold"))
title_label.pack(pady=(20, 5))

subtitle_label = ctk.CTkLabel(app, text="Converts raw ESP32 .bin logs into Vector ASCII (.asc) format", font=ctk.CTkFont(size=14), text_color="gray")
subtitle_label.pack(pady=(0, 20))

# --- PANEL WYBORU MIEJSCA ZAPISU ---
out_frame = ctk.CTkFrame(app, fg_color="transparent")
out_frame.pack(fill="x", padx=40, pady=(0, 15))

out_label = ctk.CTkLabel(out_frame, text="Save converted files to:", font=ctk.CTkFont(size=13, weight="bold"))
out_label.pack(side="left", padx=(0, 10))

# Zmienna przechowująca wybraną ścieżkę wyjściową (domyślnie pobrana z configu)
out_var = ctk.StringVar(value=app_config.get("last_out_dir", os.path.expanduser("~\\Documents")))

out_entry = ctk.CTkEntry(out_frame, textvariable=out_var, width=300)
out_entry.pack(side="left", expand=True, fill="x", padx=(0, 10))

btn_change_out = ctk.CTkButton(out_frame, text="Browse...", width=80, command=change_output_dir)
btn_change_out.pack(side="left")

# --- PRZYCISK GŁÓWNY (WYBÓR PLIKU BIN) ---
btn_open = ctk.CTkButton(
    app, 
    text="Select .bin file to convert", 
    command=open_file, 
    font=ctk.CTkFont(size=15, weight="bold"),
    height=45,
    width=300,
    corner_radius=8,
    fg_color="#1f6aa5",
    hover_color="#144870"
)
btn_open.pack(pady=(5, 20))

# --- POLE LOGÓW ---
text_area = ctk.CTkTextbox(
    app, 
    wrap="word", 
    font=ctk.CTkFont(family="Consolas", size=13),
    corner_radius=8,
    border_width=1
)
text_area.pack(expand=True, fill="both", padx=20, pady=(0, 20))

text_area.insert("end", "Set your destination folder above.\n")
text_area.insert("end", "Then click 'Select .bin file' to choose a log directly from your SD card.\n\n")
text_area.configure(state="disabled")

# Uruchom pętlę programu
app.mainloop()