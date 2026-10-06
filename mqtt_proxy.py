"""
Smart Water Tank Controller - MQTT TCP Proxy (For VMware NAT / LAN Relay)
Listens on all network interfaces (0.0.0.0:1883) and bi-directionally forwards
traffic to the Ubuntu Mosquitto broker (192.168.110.133:1883).
"""

import socket
import threading
import sys
import time

LISTEN_HOST = '0.0.0.0'
LISTEN_PORT = 1883
TARGET_HOST = '192.168.110.133'
TARGET_PORT = 1883

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

def handle_client(client_socket, client_addr):
    print(f"[+] Connected client: {client_addr}")
    try:
        target_socket = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        target_socket.connect((TARGET_HOST, TARGET_PORT))
        
        t1 = threading.Thread(target=pipe, args=(client_socket, target_socket), daemon=True)
        t2 = threading.Thread(target=pipe, args=(target_socket, client_socket), daemon=True)
        t1.start()
        t2.start()
    except Exception as e:
        print(f"[-] Failed to connect to broker {TARGET_HOST}:{TARGET_PORT}: {e}")
        try:
            client_socket.close()
        except Exception:
            pass

def main():
    server = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    server.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    try:
        server.bind((LISTEN_HOST, LISTEN_PORT))
        server.listen(10)
        print("=========================================================")
        print("  SMART WATER TANK CONTROLLER - MQTT TCP PROXY RUNNING   ")
        print(f"  Listening on: {LISTEN_HOST}:{LISTEN_PORT}")
        print(f"  Target Broker: {TARGET_HOST}:{TARGET_PORT}")
        print("=========================================================")
        sys.stdout.flush()
    except Exception as e:
        print(f"[-] Error binding port {LISTEN_PORT}: {e}")
        sys.exit(1)

    while True:
        try:
            client_socket, client_addr = server.accept()
            t = threading.Thread(target=handle_client, args=(client_socket, client_addr), daemon=True)
            t.start()
        except KeyboardInterrupt:
            print("\nShutting down proxy.")
            break
        except Exception as e:
            print(f"[-] Accept error: {e}")
            time.sleep(1)

if __name__ == '__main__':
    main()
