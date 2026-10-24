import argparse
import socket
import time

HOST = "192.168.178.70"
PORT = 8081
FRAME_TIMEOUT = 0.01


def parse_args():
    parser = argparse.ArgumentParser()
    parser.add_argument("--host", default=HOST)
    parser.add_argument("--port", type=int, default=PORT)
    parser.add_argument(
        "--out",
        metavar="dump.hex",
        help="write received hex frames to this file",
    )
    return parser.parse_args()


def emit_frame(buffer, out_file):
    line = buffer.hex(" ")
    print(line)
    if out_file:
        out_file.write(buffer.hex())
        out_file.flush()


def read_tcp(host, port, out_file):
    buffer = bytearray()
    last_data_time = None

    with socket.create_connection((host, port)) as s:
        s.settimeout(0.01)

        while True:
            try:
                data = s.recv(1024)
            except socket.timeout:
                data = None

            if data == b"":
                if buffer:
                    emit_frame(buffer, out_file)
                break

            if data:
                buffer.extend(data)
                last_data_time = time.monotonic()
                continue

            if buffer and last_data_time is not None:
                if time.monotonic() - last_data_time >= FRAME_TIMEOUT:
                    emit_frame(buffer, out_file)
                    buffer.clear()
                    last_data_time = None


def main():
    args = parse_args()
    if args.out:
        with open(args.out, "w", encoding="ascii") as out_file:
            read_tcp(args.host, args.port, out_file)
    else:
        read_tcp(args.host, args.port, None)


if __name__ == "__main__":
    main()
