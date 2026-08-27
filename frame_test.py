import customtkinter as ctk
import can
import threading
import time
import csv
import json
import os
from datetime import datetime
from tkinter import filedialog

ctk.set_appearance_mode("Dark")
ctk.set_default_color_theme("blue")

CONFIG_FILE = "can_sender_config.json"

class AdvancedCanSender(ctk.CTk):
    def __init__(self):
        super().__init__()
        self.title("CAN FD | Fast Sender & Playback")
        self.geometry("600x600")
        self.protocol("WM_DELETE_WINDOW", self.on_closing)

        self.bus = None
        self.is_connected = False
        self.worker_thread = None
        self.stop_flag = False
        self.csv_filepath = ""

        self.settings = self.load_settings()
        self.setup_ui()
        self.apply_settings_to_ui()

    def load_settings(self):
        default = {
            "clock": "80",
            "nom_mbps": "1", "data_mbps": "5",
            "n_brp": "2", "n_tseg1": "31", "n_tseg2": "8", "n_sjw": "4",
            "d_brp": "1", "d_tseg1": "11", "d_tseg2": "4", "d_sjw": "2",
            "gen_count": "100000", "gen_interval": "0.5", "gen_dlc": "64",
            "last_csv": ""
        }
        if os.path.exists(CONFIG_FILE):
            try:
                with open(CONFIG_FILE, "r") as f:
                    default.update(json.load(f))
            except Exception: pass
        return default

    def save_settings(self):
        try:
            self.settings["clock"] = self.e_clock.get()
            self.settings["nom_mbps"] = self.e_nom_mbps.get()
            self.settings["data_mbps"] = self.e_data_mbps.get()
            self.settings["n_brp"] = self.e_n_brp.get()
            self.settings["n_tseg1"] = self.e_n_tseg1.get()
            self.settings["n_tseg2"] = self.e_n_tseg2.get()
            self.settings["n_sjw"] = self.e_n_sjw.get()
            self.settings["d_brp"] = self.e_d_brp.get()
            self.settings["d_tseg1"] = self.e_d_tseg1.get()
            self.settings["d_tseg2"] = self.e_d_tseg2.get()
            self.settings["d_sjw"] = self.e_d_sjw.get()
            self.settings["gen_count"] = self.entry_count.get()
            self.settings["gen_interval"] = self.entry_interval.get()
            self.settings["gen_dlc"] = self.combo_dlc.get()
            self.settings["last_csv"] = self.csv_filepath

            with open(CONFIG_FILE, "w") as f:
                json.dump(self.settings, f, indent=4)
        except Exception: pass

    def setup_ui(self):
        self.tabview = ctk.CTkTabview(self)
        self.tabview.pack(padx=10, pady=10, fill="both", expand=True)

        self.tab_cfg = self.tabview.add("Konfiguracja (Zegar & Prędkość)")
        self.tab_gen = self.tabview.add("Generator Ramek")
        self.tab_play = self.tabview.add("Odtwarzacz (Playback)")

        self.setup_cfg_tab()
        self.setup_gen_tab()
        self.setup_play_tab()

        bottom_frame = ctk.CTkFrame(self, fg_color="transparent")
        bottom_frame.pack(side="bottom", fill="x", pady=10)
        
        self.btn_connect = ctk.CTkButton(bottom_frame, text="Połącz z PCAN", command=self.toggle_connection, fg_color="green", font=ctk.CTkFont(weight="bold"))
        self.btn_connect.pack(side="left", padx=20)

        self.lbl_global_status = ctk.CTkLabel(bottom_frame, text="ROZŁĄCZONO", font=ctk.CTkFont(weight="bold"), text_color="gray")
        self.lbl_global_status.pack(side="right", padx=20)

    def setup_cfg_tab(self):
        self.tab_cfg.grid_columnconfigure((1, 2, 3, 4), weight=1)

        ctk.CTkLabel(self.tab_cfg, text="Zegar Bazowy (MHz):", font=ctk.CTkFont(weight="bold")).grid(row=0, column=0, padx=5, pady=10, sticky="e")
        self.e_clock = ctk.CTkEntry(self.tab_cfg, width=60)
        self.e_clock.grid(row=0, column=1, padx=5, pady=10, sticky="w")

        ctk.CTkLabel(self.tab_cfg, text="Szybkość (Mbps):", font=ctk.CTkFont(weight="bold")).grid(row=1, column=0, padx=5, pady=10, sticky="e")
        speed_frame = ctk.CTkFrame(self.tab_cfg, fg_color="transparent")
        speed_frame.grid(row=1, column=1, columnspan=4, sticky="w")
        
        ctk.CTkLabel(speed_frame, text="Nominal:").pack(side="left", padx=5)
        self.e_nom_mbps = ctk.CTkEntry(speed_frame, width=50)
        self.e_nom_mbps.pack(side="left", padx=5)
        
        ctk.CTkLabel(speed_frame, text="Data FD:").pack(side="left", padx=(15, 5))
        self.e_data_mbps = ctk.CTkEntry(speed_frame, width=50)
        self.e_data_mbps.pack(side="left", padx=5)

        self.btn_calc = ctk.CTkButton(speed_frame, text="Przelicz na Timingi", command=self.calculate_and_apply_timing, width=120)
        self.btn_calc.pack(side="left", padx=15)

        ctk.CTkFrame(self.tab_cfg, height=2, fg_color="gray30").grid(row=2, column=0, columnspan=5, sticky="ew", pady=10)

        ctk.CTkLabel(self.tab_cfg, text="Arbitraż (Nominal):").grid(row=3, column=0, padx=5, pady=5, sticky="e")
        self.e_n_brp = ctk.CTkEntry(self.tab_cfg, width=50); self.e_n_brp.grid(row=3, column=1, padx=2)
        self.e_n_tseg1 = ctk.CTkEntry(self.tab_cfg, width=50); self.e_n_tseg1.grid(row=3, column=2, padx=2)
        self.e_n_tseg2 = ctk.CTkEntry(self.tab_cfg, width=50); self.e_n_tseg2.grid(row=3, column=3, padx=2)
        self.e_n_sjw = ctk.CTkEntry(self.tab_cfg, width=50); self.e_n_sjw.grid(row=3, column=4, padx=2)
        
        ctk.CTkLabel(self.tab_cfg, text="BRP", text_color="gray", font=ctk.CTkFont(size=10)).grid(row=4, column=1)
        ctk.CTkLabel(self.tab_cfg, text="TSEG1", text_color="gray", font=ctk.CTkFont(size=10)).grid(row=4, column=2)
        ctk.CTkLabel(self.tab_cfg, text="TSEG2", text_color="gray", font=ctk.CTkFont(size=10)).grid(row=4, column=3)
        ctk.CTkLabel(self.tab_cfg, text="SJW", text_color="gray", font=ctk.CTkFont(size=10)).grid(row=4, column=4)

        ctk.CTkLabel(self.tab_cfg, text="Dane (Data FD):").grid(row=5, column=0, padx=5, pady=(15,5), sticky="e")
        self.e_d_brp = ctk.CTkEntry(self.tab_cfg, width=50); self.e_d_brp.grid(row=5, column=1, padx=2, pady=(10,0))
        self.e_d_tseg1 = ctk.CTkEntry(self.tab_cfg, width=50); self.e_d_tseg1.grid(row=5, column=2, padx=2, pady=(10,0))
        self.e_d_tseg2 = ctk.CTkEntry(self.tab_cfg, width=50); self.e_d_tseg2.grid(row=5, column=3, padx=2, pady=(10,0))
        self.e_d_sjw = ctk.CTkEntry(self.tab_cfg, width=50); self.e_d_sjw.grid(row=5, column=4, padx=2, pady=(10,0))

    def apply_settings_to_ui(self):
        self.e_clock.insert(0, self.settings["clock"])
        self.e_nom_mbps.insert(0, self.settings["nom_mbps"])
        self.e_data_mbps.insert(0, self.settings["data_mbps"])
        self.e_n_brp.insert(0, self.settings["n_brp"])
        self.e_n_tseg1.insert(0, self.settings["n_tseg1"])
        self.e_n_tseg2.insert(0, self.settings["n_tseg2"])
        self.e_n_sjw.insert(0, self.settings["n_sjw"])
        self.e_d_brp.insert(0, self.settings["d_brp"])
        self.e_d_tseg1.insert(0, self.settings["d_tseg1"])
        self.e_d_tseg2.insert(0, self.settings["d_tseg2"])
        self.e_d_sjw.insert(0, self.settings["d_sjw"])
        self.entry_count.insert(0, self.settings.get("gen_count", "100000"))
        self.entry_interval.insert(0, self.settings.get("gen_interval", "0.5"))
        self.combo_dlc.set(self.settings.get("gen_dlc", "64"))

        if self.settings.get("last_csv") and os.path.exists(self.settings["last_csv"]):
            self.csv_filepath = self.settings["last_csv"]
            self.lbl_csv_file.configure(text=self.csv_filepath.split("/")[-1], text_color="white")

    def calc_timing_math(self, clock_mhz, bitrate_mbps, preferred_tq):
        clock_hz = clock_mhz * 1_000_000
        bitrate_hz = bitrate_mbps * 1_000_000
        best = None
        for brp in range(1, 513):
            total_tq_float = clock_hz / (brp * bitrate_hz)
            total_tq = round(total_tq_float)
            if total_tq < 8 or total_tq > 80 or abs(total_tq_float - total_tq) > 0.0001: 
                continue
            for tseg2 in range(1, min(32, total_tq - 2) + 1):
                tseg1 = total_tq - 1 - tseg2
                if tseg1 < 1 or tseg1 > 64: 
                    continue
                sample_error = abs(((1 + tseg1) / total_tq) - 0.80)
                score = sample_error + abs(total_tq - preferred_tq) * 0.001
                if best is None or score < best[0]:
                    best = (score, (brp, tseg1, tseg2, min(4, tseg2)))
        return best[1] if best else (1, 15, 4, 2)

    def calculate_and_apply_timing(self):
        try:
            clock = float(self.e_clock.get().strip())
            nom = float(self.e_nom_mbps.get().strip())
            data = float(self.e_data_mbps.get().strip())
            n_brp, n_t1, n_t2, n_sjw = self.calc_timing_math(clock, nom, 40)
            d_brp, d_t1, d_t2, d_sjw = self.calc_timing_math(clock, data, 16)
            entries = [
                (self.e_n_brp, n_brp), (self.e_n_tseg1, n_t1), (self.e_n_tseg2, n_t2), (self.e_n_sjw, n_sjw),
                (self.e_d_brp, d_brp), (self.e_d_tseg1, d_t1), (self.e_d_tseg2, d_t2), (self.e_d_sjw, d_sjw)
            ]
            for entry, val in entries:
                entry.delete(0, "end")
                entry.insert(0, str(val))
            self.save_settings()
        except Exception as e:
            print("Błąd kalkulacji:", e)

    def setup_gen_tab(self):
        ctk.CTkLabel(self.tab_gen, text="Liczba ramek do testu:").grid(row=0, column=0, padx=10, pady=10, sticky="e")
        self.entry_count = ctk.CTkEntry(self.tab_gen, width=100)
        self.entry_count.grid(row=0, column=1, padx=10, pady=10, sticky="w")

        ctk.CTkLabel(self.tab_gen, text="Zwykły Interwał (ms):").grid(row=1, column=0, padx=10, pady=10, sticky="e")
        self.entry_interval = ctk.CTkEntry(self.tab_gen, width=100)
        self.entry_interval.grid(row=1, column=1, padx=10, pady=10, sticky="w")

        ctk.CTkLabel(self.tab_gen, text="Długość danych (DLC):").grid(row=2, column=0, padx=10, pady=10, sticky="e")
        dlc_options = ["0", "1", "2", "3", "4", "5", "6", "7", "8", "12", "16", "20", "24", "32", "48", "64"]
        self.combo_dlc = ctk.CTkComboBox(self.tab_gen, values=dlc_options, width=100)
        self.combo_dlc.grid(row=2, column=1, padx=10, pady=10, sticky="w")

        self.btn_gen_start = ctk.CTkButton(self.tab_gen, text="ZWYKŁY GENERATOR", command=self.toggle_generator, state="disabled")
        self.btn_gen_start.grid(row=3, column=0, pady=20, padx=10)

        self.btn_stress_start = ctk.CTkButton(self.tab_gen, text="REALNY STRESS TEST (100% LOAD)", command=self.toggle_stress_test, fg_color="darkred", hover_color="#8b0000", state="disabled")
        self.btn_stress_start.grid(row=3, column=1, pady=20, padx=10)

        self.lbl_gen_status = ctk.CTkLabel(self.tab_gen, text="Gotowy", text_color="gray", font=ctk.CTkFont(size=14))
        self.lbl_gen_status.grid(row=4, column=0, columnspan=2, pady=5)
        
        self.lbl_stress_results = ctk.CTkLabel(self.tab_gen, text="", text_color="cyan", font=ctk.CTkFont(size=14, weight="bold"))
        self.lbl_stress_results.grid(row=5, column=0, columnspan=2, pady=5)

    def setup_play_tab(self):
        self.btn_load_csv = ctk.CTkButton(self.tab_play, text="Wybierz plik z logiem", command=self.load_csv)
        self.btn_load_csv.pack(pady=10)
        self.lbl_csv_file = ctk.CTkLabel(self.tab_play, text="Brak wybranego pliku", text_color="gray")
        self.lbl_csv_file.pack(pady=5)
        self.btn_play_start = ctk.CTkButton(self.tab_play, text="START ODTWARZANIA", command=self.toggle_playback, state="disabled", fg_color="purple")
        self.btn_play_start.pack(pady=20)
        self.lbl_play_status = ctk.CTkLabel(self.tab_play, text="Wymaga połączenia z CAN", text_color="gray")
        self.lbl_play_status.pack(pady=5)

    def toggle_connection(self):
        if not self.is_connected:
            try:
                params = {
                    "f_clock_mhz": int(self.e_clock.get()),
                    "nom_brp": int(self.e_n_brp.get()), "nom_tseg1": int(self.e_n_tseg1.get()), 
                    "nom_tseg2": int(self.e_n_tseg2.get()), "nom_sjw": int(self.e_n_sjw.get()),
                    "data_brp": int(self.e_d_brp.get()), "data_tseg1": int(self.e_d_tseg1.get()), 
                    "data_tseg2": int(self.e_d_tseg2.get()), "data_sjw": int(self.e_d_sjw.get())
                }
                self.bus = can.interface.Bus(interface='pcan', channel='PCAN_USBBUS1', fd=True, **params)
                self.is_connected = True
                
                self.save_settings()
                self.btn_connect.configure(text="Rozłącz", fg_color="red")
                self.lbl_global_status.configure(text="POŁĄCZONO", text_color="green")
                self.btn_gen_start.configure(state="normal")
                self.btn_stress_start.configure(state="normal")
                if self.csv_filepath:
                    self.btn_play_start.configure(state="normal")
                    self.lbl_play_status.configure(text="Gotowy do odtworzenia")
            except Exception as e:
                self.lbl_global_status.configure(text=f"BŁĄD POŁĄCZENIA", text_color="red")
        else:
            self.stop_flag = True
            if self.bus: self.bus.shutdown()
            self.is_connected = False
            self.btn_connect.configure(text="Połącz z PCAN", fg_color="green")
            self.lbl_global_status.configure(text="ROZŁĄCZONO", text_color="gray")
            self.btn_gen_start.configure(state="disabled")
            self.btn_stress_start.configure(state="disabled")
            self.btn_play_start.configure(state="disabled")

    def toggle_generator(self):
        if self.worker_thread and self.worker_thread.is_alive():
            self.stop_flag = True
            self.btn_gen_start.configure(text="ZWYKŁY GENERATOR", fg_color="#1f538d")
        else:
            count = int(self.entry_count.get())
            interval_ms = float(self.entry_interval.get())
            dlc = int(self.combo_dlc.get())
            
            self.stop_flag = False
            self.save_settings()
            self.btn_gen_start.configure(text="STOP GENERATORA", fg_color="red")
            self.btn_stress_start.configure(state="disabled")
            self.btn_play_start.configure(state="disabled")
            self.lbl_stress_results.configure(text="")
            
            self.worker_thread = threading.Thread(target=self.generator_thread, args=(count, interval_ms, dlc), daemon=True)
            self.worker_thread.start()

    def generator_thread(self, target_count, interval_ms, dlc):
        delay_s = interval_ms / 1000.0
        sent = 0
        start_time = time.time()

        while not self.stop_flag:
            if target_count > 0 and sent >= target_count: break
            try:
                random_payload = os.urandom(dlc) 
                msg = can.Message(arbitration_id=0x123, data=random_payload, is_extended_id=False, is_fd=True)
                self.bus.send(msg)
                sent += 1
                
                if sent % 100 == 0: 
                    self.after(0, self.lbl_gen_status.configure, {"text": f"Wysłano: {sent}"})
                
                if delay_s > 0:
                    t_target = time.perf_counter() + delay_s
                    while time.perf_counter() < t_target:
                        time.sleep(0.001) if t_target - time.perf_counter() > 0.002 else time.sleep(0)
            except can.CanError: 
                time.sleep(0.001)
            except Exception: 
                break

        t_total = time.time() - start_time
        self.after(0, self.finish_task, self.btn_gen_start, self.lbl_gen_status, f"Koniec. Wysłano {sent} (Czas: {t_total:.2f}s)")

    def toggle_stress_test(self):
        if self.worker_thread and self.worker_thread.is_alive():
            self.stop_flag = True
            self.btn_stress_start.configure(text="REALNY STRESS TEST (100% LOAD)", fg_color="darkred")
        else:
            count = int(self.entry_count.get())
            if count <= 0: return
            dlc = int(self.combo_dlc.get())
            
            self.stop_flag = False
            self.save_settings()

            self.btn_stress_start.configure(text="ZATRZYMAJ STRESS TEST", fg_color="red")
            self.btn_gen_start.configure(state="disabled")
            self.btn_play_start.configure(state="disabled")
            self.lbl_stress_results.configure(text="Testowanie pełnego obciążenia...")
            
            self.worker_thread = threading.Thread(target=self.stress_test_thread, args=(count, dlc), daemon=True)
            self.worker_thread.start()

    def stress_test_thread(self, target_count, dlc):
        try:
            nom_mbps = float(self.e_nom_mbps.get())
            data_mbps = float(self.e_data_mbps.get())
        except:
            nom_mbps, data_mbps = 1.0, 5.0
            
        nom_bits = 29
        data_bits = (dlc * 8) + 45 
        
        physical_frame_time_s = (nom_bits / (nom_mbps * 1_000_000.0)) + (data_bits / (data_mbps * 1_000_000.0))
        
        sent = 0
        send_errors = 0 
        start_time = time.time()
        next_frame_time = time.perf_counter()

        while not self.stop_flag and sent < target_count:
            try:
                now = time.perf_counter()
                
                if now >= next_frame_time:
                    msg = can.Message(arbitration_id=0x666, data=os.urandom(dlc), is_extended_id=False, is_fd=True)
                    
                    self.bus.send(msg)
                    sent += 1
                    next_frame_time += physical_frame_time_s
                    
                    if sent % 2000 == 0: 
                        self.after(0, self.lbl_gen_status.configure, {"text": f"Wysłano: {sent} | Błędy PCAN: {send_errors}"})
                else:
                    if next_frame_time - now > 0.002:
                        time.sleep(0.001)
                    else:
                        time.sleep(0)
                        
            except can.CanError:
                send_errors += 1
                next_frame_time = time.perf_counter() + 0.002
            except Exception:
                break

        t_total = time.time() - start_time
        
        if t_total > 0:
            fps = sent / t_total
            bps = fps * (nom_bits + data_bits)
            mbps = bps / 1_000_000.0
            results_text = f"Wynik: {sent} wysłanych | {send_errors} błędów bufora | {mbps:.2f} Mbps"
        else:
            results_text = "Błąd obliczeń czasu."

        self.after(0, self.lbl_stress_results.configure, {"text": results_text})
        self.after(0, self.finish_task, self.btn_stress_start, self.lbl_gen_status, f"Koniec. Wysłano {sent} (Błędy PCAN: {send_errors})")

    # ----- POPRAWIONA LOGIKA PLAYBACK (Format: timestamp, id, dlc, bytes..., type) -----
    def load_csv(self):
        path = filedialog.askopenfilename(filetypes=[("Log Files", "*.csv;*.asc;*.txt"), ("All Files", "*.*")])
        if path:
            self.csv_filepath = path
            self.lbl_csv_file.configure(text=path.split("/")[-1], text_color="white")
            self.save_settings()
            if self.is_connected:
                self.btn_play_start.configure(state="normal")
                self.lbl_play_status.configure(text="Gotowy do odtworzenia")

    def toggle_playback(self):
        if self.worker_thread and self.worker_thread.is_alive():
            self.stop_flag = True
            self.btn_play_start.configure(text="START ODTWARZANIA", fg_color="purple")
        else:
            self.stop_flag = False
            self.btn_play_start.configure(text="STOP ODTWARZANIA", fg_color="red")
            self.btn_gen_start.configure(state="disabled")
            self.btn_stress_start.configure(state="disabled")
            self.lbl_play_status.configure(text="Odtwarzanie...", text_color="orange")
            
            self.worker_thread = threading.Thread(target=self.playback_thread, daemon=True)
            self.worker_thread.start()

    def playback_thread(self):
        sent = 0
        last_timestamp = None

        try:
            with open(self.csv_filepath, 'r') as f:
                reader = csv.reader(f, delimiter=',')
                for row in reader:
                    if self.stop_flag: break
                    if not row or len(row) < 4: continue
                    
                    # 1. Odczyt znacznika czasu (kolumna 0, np. 1307.00032)
                    try:
                        current_ts = float(row[0].strip())
                    except ValueError:
                        continue # Pomiń ewentualny nagłówek

                    # 2. Wyznaczenie odstępu czasowego (delty) względem poprzedniej ramki
                    delay_s = 0.0
                    if last_timestamp is not None:
                        delay_s = current_ts - last_timestamp
                    last_timestamp = current_ts

                    try:
                        # 3. Parsowanie ID (kolumna 1 w formacie HEX, np. 00A0)
                        msg_id = int(row[1].strip(), 16)
                        
                        # 4. Parsowanie danych (od kolumny 3 do przedostatniej)
                        # Kolumna 2 to długość/DLC, a ostatnia kolumna to typ (np. R)
                        data_hex_list = row[3:-1]
                        msg_data = [int(b.strip(), 16) for b in data_hex_list if b.strip() != '']
                        
                        is_fd = len(msg_data) > 8
                        is_ext = msg_id > 0x7FF

                        msg = can.Message(
                            arbitration_id=msg_id, 
                            data=bytes(msg_data), 
                            is_extended_id=is_ext, 
                            is_fd=True
                        )
                    except Exception:
                        continue

                    # 5. Precyzyjne odczekanie wyliczonego odstępu czasu
                    if delay_s > 0:
                        t_target = time.perf_counter() + delay_s
                        while time.perf_counter() < t_target and not self.stop_flag:
                            if t_target - time.perf_counter() > 0.002:
                                time.sleep(0.001)
                            else:
                                time.sleep(0)

                    if self.stop_flag: break

                    # 6. Wysłanie ramki na magistralę
                    try:
                        self.bus.send(msg)
                        sent += 1
                        if sent % 50 == 0:
                            self.after(0, self.lbl_play_status.configure, {"text": f"Odtworzono: {sent} ramek"})
                    except can.CanError:
                        time.sleep(0.001)

        except Exception as e:
            print("Playback Error:", e)

        self.after(0, self.finish_task, self.btn_play_start, self.lbl_play_status, f"Koniec. Odtworzono {sent} ramek.")

    def finish_task(self, btn, lbl, status_text):
        if btn == self.btn_gen_start:
            btn.configure(text="ZWYKŁY GENERATOR", fg_color="#1f538d")
            self.btn_stress_start.configure(state="normal")
            if self.is_connected and self.csv_filepath: self.btn_play_start.configure(state="normal")
        elif btn == self.btn_stress_start:
            btn.configure(text="REALNY STRESS TEST (100% LOAD)", fg_color="darkred")
            self.btn_gen_start.configure(state="normal")
            if self.is_connected and self.csv_filepath: self.btn_play_start.configure(state="normal")
        else:
            btn.configure(text="START ODTWARZANIA", fg_color="purple")
            if self.is_connected: 
                self.btn_gen_start.configure(state="normal")
                self.btn_stress_start.configure(state="normal")
            
        lbl.configure(text=status_text, text_color="green")
        self.stop_flag = True

    def on_closing(self):
        self.stop_flag = True
        self.save_settings()
        if self.bus: self.bus.shutdown()
        self.destroy()

if __name__ == "__main__":
    app = AdvancedCanSender()
    app.mainloop()