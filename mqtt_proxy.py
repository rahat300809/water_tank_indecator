"""
Smart Water Tank Controller - MQTT TCP & WebSocket Proxy (For VMware NAT / LAN Relay)
Listens on all network interfaces:
  - 0.0.0.0:1883 -> forwards to 192.168.110.133:1883 (ESP32 MQTT TCP)
  - 0.0.0.0:9001 -> forwards to 192.168.110.133:9001 (Web Dashboard WebSocket)
"""

import socket
import threading
import sys
import time

TARGET_HOST = '192.168.110.133'
PORTS = [1883, 9001]

def pipe(source, destination):
    try:
        while True:
            data = source.recv(4096)
            if not data:
                break
            destination.sendall(data)
    except Exception:
        pass
    finally:
        try:
            source.close()
        except Exception:
            pass
        try:
            destination.close()
        except Exception:
            pass

def handle_client(client_socket, client_addr, port):
    proto = "TCP" if port == 1883 else "WebSocket"
    print(f"[+] [{proto}:{port}] Client connected: {client_addr}", flush=True)
    try:
        target_socket = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        target_socket.connect((TARGET_HOST, port))
        
        t1 = threading.Thread(target=pipe, args=(client_socket, target_socket), daemon=True)
        t2 = threading.Thread(target=pipe, args=(target_socket, client_socket), daemon=True)
        t1.start()
        t2.start()
    except Exception as e:
        print(f"[-] [{proto}:{port}] Failed to connect to broker {TARGET_HOST}:{port}: {e}", flush=True)
        try:
            client_socket.close()
        except Exception:
            pass

def start_listener(port):
    server = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    server.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    try:
        server.bind(('0.0.0.0', port))
        server.listen(10)
        proto = "TCP" if port == 1883 else "WebSocket"
        print(f"  [OK] Listening on 0.0.0.0:{port} ({proto}) -> {TARGET_HOST}:{port}", flush=True)
    except Exception as e:
        print(f"  [!] Error binding port {port}: {e}", flush=True)
        return

    while True:
        try:
            client_socket, client_addr = server.accept()
            t = threading.Thread(target=handle_client, args=(client_socket, client_addr, port), daemon=True)
            t.start()
        except Exception as e:
            print(f"[-] Accept error on port {port}: {e}", flush=True)
            time.sleep(1)

def main():
    print("=========================================================", flush=True)
    print("  SMART WATER TANK CONTROLLER - DUAL MQTT RELAY RUNNING  ", flush=True)
    print("=========================================================", flush=True)

    threads = []
    for port in PORTS:
        t = threading.Thread(target=start_listener, args=(port,), daemon=True)
        t.start()
        threads.append(t)

    print("=========================================================", flush=True)
    print("  Relay active. Press Ctrl+C to stop.", flush=True)
    print("=========================================================", flush=True)

    try:
        while True:
            time.sleep(1)
    except KeyboardInterrupt:
        print("\nShutting down proxy.", flush=True)

if __name__ == '__main__':
    main()
