import socket

HOST = "192.168.178.70"
PORT = 8080
PAYLOAD = bytes.fromhex("0102030405")

with socket.create_connection((HOST, PORT)) as s:
    s.sendall(PAYLOAD)

print(PAYLOAD.hex(" "))
