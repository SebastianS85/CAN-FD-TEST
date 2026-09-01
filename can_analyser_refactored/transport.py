import socket
import time
import logging
from PyQt6.QtCore import QThread, pyqtSignal, Qt, QTimer, QAbstractTableModel, QModelIndex, QPointF, QRegularExpression
from .constants import FRAME_SIZE, QThread, logging, pyqtSignal, socket, time
class TCPReceiverThread(QThread):
    connection_status = pyqtSignal(bool, str)

    def __init__(self, ip, port, buffer_deque):
        super().__init__()
        self.ip, self.port = ip, port
        self.running = True
        self.connection_enabled = True
        self.capture_enabled = True
        self.buffer_deque = buffer_deque
        self.rx_socket = None
        self.tx_socket = None

    def update_ip(self, new_ip):
        if self.ip != new_ip:
            self.ip = new_ip
            self.close_sockets()

    def set_capture_enabled(self, enabled):
        was_enabled = self.capture_enabled
        self.capture_enabled = enabled
        if enabled and not was_enabled:
            logging.info("Live capture enabled; reconnecting to ESP32.")
            self.close_sockets()

    def set_connection_enabled(self, enabled):
        self.connection_enabled = enabled
        if not enabled:
            self.close_sockets()

    def close_sockets(self):
        if self.rx_socket:
            try: self.rx_socket.close()
            except: pass
            self.rx_socket = None
        if self.tx_socket:
            try: self.tx_socket.close()
            except: pass
            self.tx_socket = None

    def run(self):
        logging.info(f"TCP client thread started. Target: {self.ip}:{self.port}")

        while self.running:
            if not self.connection_enabled:
                self.connection_status.emit(False, "PAUSED")
                time.sleep(0.1)
                continue

            self.connection_status.emit(False, "SEARCHING...")
            try:
                # --- ODBIÓR DANYCH Z ESP32 (Port 3333) ---
                sock_rx = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
                sock_rx.settimeout(2.0)
                sock_rx.connect((self.ip, self.port))
                
                # --- WYSYŁANIE KOMEND DO ESP32 (Port 3334) ---
                sock_tx = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
                sock_tx.settimeout(2.0)
                sock_tx.connect((self.ip, self.port + 1))
                
                sock_tx.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
                
                self.rx_socket = sock_rx
                self.tx_socket = sock_tx
                
                self.connection_status.emit(True, "CONNECTED")
                logging.info(f"Connected to ESP32 at {self.ip} (RX: {self.port}, TX: {self.port+1})")

                stream_buffer = bytearray()
                
                while self.running:
                    try:
                        chunk = self.rx_socket.recv(1024 * 64) 
                        if not chunk: 
                            logging.warning("ESP32 closed connection.")
                            break 
                        stream_buffer.extend(chunk)
                        
                        while len(stream_buffer) >= FRAME_SIZE:
                            frame_data = stream_buffer[:FRAME_SIZE]
                            if self.capture_enabled:
                                self.buffer_deque.append(frame_data)
                            del stream_buffer[:FRAME_SIZE]
                            
                    except socket.timeout:
                        continue
                    except Exception as e:
                        logging.error(f"TCP stream error: {e}")
                        break
                        
                self.close_sockets()
                logging.warning("Disconnected from ESP32. Retrying...")
                
            except (socket.timeout, ConnectionRefusedError, OSError):
                self.close_sockets()
                time.sleep(1.0)
                continue
            except Exception as e:
                self.close_sockets()
                logging.error(f"TCP Error: {e}")
                time.sleep(1.0)

    def send_tcp_data(self, payload):
        if self.tx_socket:
            try:
                self.tx_socket.sendall(payload)
                return True
            except Exception as e:
                logging.error(f"TX error: {e}")
        return False

    def stop(self):
        self.running = False
        self.close_sockets()
        self.wait()

class CyclicSenderThread(QThread):
    def __init__(self, tcp_thread, payload, interval_ms):
        super().__init__()
        self.tcp_thread = tcp_thread
        self.payload = payload
        self.interval_sec = interval_ms / 1000.0
        self.running = True

    def run(self):
        while self.running:
            start_time = time.perf_counter()
            self.tcp_thread.send_tcp_data(self.payload)
            sleep_duration = self.interval_sec - (time.perf_counter() - start_time)
            if sleep_duration > 0: time.sleep(sleep_duration)

    def stop(self):
        self.running = False
        self.wait()

