import tkinter as tk
from tkinter import filedialog, scrolledtext
import struct
import os
import collections

# Format struktury C
STRUCT_FMT = '<I B I B 64s'
FRAME_SIZE = struct.calcsize(STRUCT_FMT)

def analyze_file(filename):
    received_seqs = []
    
    try:
        with open(filename, 'rb') as f:
            while True:
                raw_data = f.read(FRAME_SIZE)
                if len(raw_data) < FRAME_SIZE:
                    break
                
                timestamp_ms, node_id, can_id_raw, dlc, payload = struct.unpack(STRUCT_FMT, raw_data)
                can_id = can_id_raw & 0x1FFFFFFF
                
                # Interesują nas tylko ramki z naszego generatora (0x55, 64 bajty)
                if can_id == 0x55 and dlc == 15:
                    seq = struct.unpack('<I', payload[:4])[0]
                    received_seqs.append(seq)
                    
        total_received = len(received_seqs)
        
        if total_received == 0:
            return "Brak ramek testowych! (Nie znaleziono ID 0x55 o długości 64 bajtów)."

        # 1. Analiza duplikatów za pomocą Counter
        counter = collections.Counter(received_seqs)
        duplicates = {seq: count for seq, count in counter.items() if count > 1}
        total_duplicate_instances = sum(count - 1 for count in duplicates.values())

        # 2. Analiza unikalnych ID i luk (zakładamy test od 0 do 99999)
        unique_seqs = set(received_seqs)
        expected_set = set(range(100000))
        truly_missing = expected_set - unique_seqs

        # Generowanie szczegółowego raportu
        report = "--- SZCZEGÓŁOWY RAPORT: LUKI I DUPLIKATY ---\n\n"
        report += f"Plik: {os.path.basename(filename)}\n"
        report += f"Łącznie odczytanych rekordów z pliku: {total_received}\n"
        report += f"Unikalnych ramek (ID):           {len(unique_seqs)} / 100000\n"
        report += f"Liczba zduplikowanych unikalnych ID: {len(duplicates)}\n"
        report += f"Nadmiarowych wpisów (duplikatów):    {total_duplicate_instances}\n"
        report += f"Faktycznie brakujących ID (luki):    {len(truly_missing)}\n\n"
        
        if len(truly_missing) == 0 and total_duplicate_instances == 0:
            report += "🎉 PERFEKCJA! Zapisano dokładnie 100 000 unikalnych ramek, zero duplikatów, zero luk!\n"
        else:
            if len(duplicates) > 0:
                report += "🔄 PRZYKŁADY ZDUPLIKOWANYCH RAMEK (ID : ile razy wystąpiła):\n"
                sample_dups = list(duplicates.items())[:15]
                for seq, count in sample_dups:
                    report += f"  Ramka #{seq} -> wystąpiła {count} razy\n"
                report += "\n"
            
            if len(truly_missing) > 0:
                report += "❌ FAKTYCZNIE BRAKUJĄCE NUMERY (pierwsze 25):\n"
                report += f"  {sorted(list(truly_missing))[:25]}\n"
            
        return report

    except Exception as e:
        return f"Wystąpił błąd podczas analizy pliku:\n{e}"

def open_file():
    filepath = filedialog.askopenfilename(
        initialdir="E:\\", 
        title="Wybierz plik z logiem CAN",
        filetypes=(("Pliki binarne CAN", "*.bin"), ("Wszystkie pliki", "*.*"))
    )
    
    if filepath:
        text_area.delete(1.0, tk.END)
        text_area.insert(tk.END, f"Wczytywanie i analiza pliku:\n{filepath}\n\nAnalizuję luki i duplikaty...")
        root.update()
        
        result = analyze_file(filepath)
        
        text_area.delete(1.0, tk.END)
        text_area.insert(tk.END, result)

# --- Ustawienia okna GUI ---
root = tk.Tk()
root.title("CAN FD Advanced Validator (Gaps & Duplicates)")
root.geometry("700x520")
root.configure(padx=20, pady=20)

title_label = tk.Label(root, text="Zaawansowany Walidator Logów (Luki + Duplikaty)", font=("Helvetica", 15, "bold"))
title_label.pack(pady=(0, 15))

# Przycisk otwierający plik
btn_open = tk.Button(root, text="Wybierz plik .bin z dysku E:\\", command=open_file, 
                     font=("Helvetica", 12), bg="#2196F3", fg="white", 
                     padx=20, pady=10, cursor="hand2")
btn_open.pack(pady=10)

# Okno tekstowe na raport z paskiem przewijania
text_area = scrolledtext.ScrolledText(root, wrap=tk.WORD, font=("Consolas", 10), height=18)
text_area.pack(expand=True, fill='both')
text_area.insert(tk.END, "Wybierz plik z karty SD, aby przeanalizować bilans unikalnych ID, duplikatów i brakujących luk.")

root.mainloop()