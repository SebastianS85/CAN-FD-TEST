import tkinter as tk
from tkinter import filedialog, scrolledtext
import struct
import os
import can  # Wymaga: pip install python-can

# Format struktury C
STRUCT_FMT = '<I B I B 64s'
FRAME_SIZE = struct.calcsize(STRUCT_FMT)
DLC_TO_LEN = [0, 1, 2, 3, 4, 5, 6, 7, 8, 12, 16, 20, 24, 32, 48, 64]

def convert_to_asc(input_filepath):
    # Automatycznie generuje ścieżkę do pliku wyjściowego (.asc) na podstawie wejścia
    output_filepath = os.path.splitext(input_filepath)[0] + '.asc'
    
    # Wyświetlamy status początkowy
    report = f"Otwieranie surowego pliku:\n{input_filepath}\n\n"
    report += "Trwa konwersja do formatu ASC (SavvyCAN)...\nTo może zająć kilka sekund.\n"
    text_area.insert(tk.END, report)
    root.update()  # Wymusza odświeżenie okna, żeby tekst się pojawił przed ciężką pętlą

    try:
        # can.io.ASCWriter automatycznie generuje plik zgodny ze standardem Vector
        with can.io.ASCWriter(output_filepath) as writer:
            with open(input_filepath, 'rb') as f:
                count = 0
                while True:
                    raw_data = f.read(FRAME_SIZE)
                    if len(raw_data) < FRAME_SIZE:
                        break
                    
                    timestamp_ms, node_id, can_id_raw, dlc, payload = struct.unpack(STRUCT_FMT, raw_data)
                    
                    is_extended = bool(can_id_raw & 0x80000000)
                    can_id = can_id_raw & 0x1FFFFFFF
                    safe_dlc = dlc if dlc <= 15 else 0
                    data_len = DLC_TO_LEN[safe_dlc]
                    
                    valid_data = payload[:data_len]
                    
                    # Tworzymy oficjalny obiekt ramki CAN FD
                    msg = can.Message(
                        timestamp=timestamp_ms / 1000.0,
                        arbitration_id=can_id,
                        is_extended_id=is_extended,
                        is_rx=True,
                        channel=node_id - 1, # Bus 0 i 1
                        is_fd=True,          # KLUCZ: Zapobiega wywalaniu się SavvyCAN!
                        bitrate_switch=True,
                        data=valid_data
                    )
                    
                    # Zapisujemy ramkę do pliku .asc
                    writer.on_message_received(msg)
                    count += 1
                    
        # Wypisujemy sukces do okna
        text_area.insert(tk.END, f"\n✅ SUKCES!\n")
        text_area.insert(tk.END, f"Zdekodowano i zabezpieczono {count} ramek CAN FD.\n")
        text_area.insert(tk.END, f"Plik gotowy do SavvyCAN:\n{output_filepath}\n")
        
    except Exception as e:
        text_area.insert(tk.END, f"\n❌ Wystąpił błąd podczas konwersji:\n{e}\n")

def open_file():
    # Uruchamia okno wyboru pliku na zadanym dysku E:/
    filepath = filedialog.askopenfilename(
        initialdir="E:\\", 
        title="Wybierz plik .bin do konwersji",
        filetypes=(("Pliki binarne CAN", "*.bin"), ("Wszystkie pliki", "*.*"))
    )
    
    if filepath:
        text_area.delete(1.0, tk.END)
        convert_to_asc(filepath)

# --- Ustawienia okna GUI ---
root = tk.Tk()
root.title("CAN FD -> SavvyCAN (.asc) Converter")
root.geometry("650x450")
root.configure(padx=20, pady=20)

title_label = tk.Label(root, text="Konwerter Logów do formatu SavvyCAN", font=("Helvetica", 16, "bold"))
title_label.pack(pady=(0, 15))

# Przycisk otwierający plik
btn_open = tk.Button(root, text="Wybierz plik .bin z dysku E:\\", command=open_file, 
                     font=("Helvetica", 12), bg="#2196F3", fg="white", 
                     padx=20, pady=10, cursor="hand2")
btn_open.pack(pady=10)

# Okno tekstowe na raport z paskiem przewijania
text_area = scrolledtext.ScrolledText(root, wrap=tk.WORD, font=("Consolas", 11), height=15)
text_area.pack(expand=True, fill='both')
text_area.insert(tk.END, "Włóż kartę SD i wybierz plik .bin, aby przekonwertować go do standardu ASC.\n")

# Uruchomienie pętli GUI
root.mainloop()