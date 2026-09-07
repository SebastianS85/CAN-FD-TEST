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
        self.close_tx_socket()

    def close_tx_socket(self):
        if self.tx_socket:
            try: self.tx_socket.close()
            except: pass
            self.tx_socket = None

    @staticmethod
    def configure_keepalive(sock):
        sock.setsockopt(socket.SOL_SOCKET, socket.SO_KEEPALIVE, 1)
        for option_name, value in (
            ("TCP_KEEPIDLE", 10),
            ("TCP_KEEPINTVL", 3),
            ("TCP_KEEPCNT", 3),
        ):
            option = getattr(socket, option_name, None)
            if option is not None:
                try:
                    sock.setsockopt(socket.IPPROTO_TCP, option, value)
                except OSError:
                    pass

    def connect_tx_socket(self):
        if self.tx_socket:
            return True
        try:
            tx_socket = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
            tx_socket.settimeout(2.0)
            tx_socket.connect((self.ip, self.port + 1))
            tx_socket.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
            self.configure_keepalive(tx_socket)
            self.tx_socket = tx_socket
            logging.info("TCP command connection established at %s:%d", self.ip, self.port + 1)
            return True
        except OSError as error:
            try:
                tx_socket.close()
            except (OSError, UnboundLocalError):
                pass
            logging.warning("TCP command port %d is unavailable: %s", self.port + 1, error)
            return False

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
                self.configure_keepalive(sock_rx)
                
                self.rx_socket = sock_rx
                if self.connect_tx_socket():
                    self.connection_status.emit(True, "CONNECTED")
                    logging.info(f"Connected to ESP32 at {self.ip} (RX: {self.port}, TX: {self.port+1})")
                else:
                    self.connection_status.emit(False, "RX ONLY")

                stream_buffer = bytearray()
                last_frame_time = time.monotonic()
                idle_reported = False
                
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
                            last_frame_time = time.monotonic()
                            if idle_reported:
                                self.connection_status.emit(True, "RECEIVING")
                                idle_reported = False

                        if not idle_reported and time.monotonic() - last_frame_time >= 5:
                            self.connection_status.emit(True, "NO DATA")
                            idle_reported = True
                            
                    except socket.timeout:
                        if not idle_reported and time.monotonic() - last_frame_time >= 5:
                            self.connection_status.emit(True, "NO DATA")
                            idle_reported = True
                        continue
                    except Exception as e:
                        logging.error(f"TCP stream error: {e}")
                        break
                        
                self.close_sockets()
                logging.warning("Disconnected from ESP32. Retrying...")
                
            except (socket.timeout, ConnectionRefusedError, OSError) as error:
                self.close_sockets()
                logging.warning(
                    "Cannot connect to ESP32 at %s:%d: %s",
                    self.ip,
                    self.port,
                    error,
                )
                time.sleep(1.0)
                continue
            except Exception as e:
                self.close_sockets()
                logging.error(f"TCP Error: {e}")
                time.sleep(1.0)

    def send_tcp_data(self, payload):
        if not self.connect_tx_socket():
            return False
        try:
            self.tx_socket.sendall(payload)
            self.connection_status.emit(True, "CONNECTED")
            return True
        except OSError as error:
            logging.error("TCP command send error: %s", error)
            self.close_tx_socket()
            self.connection_status.emit(False, "RX ONLY")
        return False

    def send_tcp_control_command(self, payload):
        if not self.connect_tx_socket():
            return False
        try:
            self.tx_socket.sendall(payload)
            response = self.tx_socket.recv(1)
            if response == b'\x00':
                self.connection_status.emit(True, "CONNECTED")
                logging.info("ESP32 confirmed CAN configuration command")
                return True
            logging.error("ESP32 rejected CAN configuration command")
        except OSError as error:
            logging.error("CAN configuration TCP error: %s", error)
        self.close_tx_socket()
        self.connection_status.emit(False, "RX ONLY")
        return False

    def send_tcp_mode_command(self, payload):
        """Send a mode-control packet and return (ack_ok, mode, remote_enabled) or None on failure."""
        if not self.connect_tx_socket():
            return None
        try:
            self.tx_socket.sendall(payload)
            response = self.tx_socket.recv(2)
            if len(response) == 2:
                ack_ok = response[0] == 0
                mode = response[1] & 0x0F
                remote_enabled = bool(response[1] & 0x80)
                self.connection_status.emit(True, "CONNECTED")
                return ack_ok, mode, remote_enabled
            logging.error("ESP32 sent malformed mode-control response")
        except OSError as error:
            logging.error("Mode-control TCP error: %s", error)
        self.close_tx_socket()
        self.connection_status.emit(False, "RX ONLY")
        return None


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

