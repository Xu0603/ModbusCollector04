import socket
import struct


HOST = "127.0.0.1"
PORT = 19091

PYSM_VERSION = 1

MSG_PING = 1
MSG_SNAPSHOT = 2


def make_request(message_type, request_id, payload=""):

    payload_bytes = payload.encode("utf-8")

    header = struct.pack(
        ">4sHHII",
        b"PYSM",
        PYSM_VERSION,
        message_type,
        request_id,
        len(payload_bytes)
    )

    return header + payload_bytes


def recv_exact(sock, size):

    data = b""

    while len(data) < size:

        chunk = sock.recv(size - len(data))

        if not chunk:
            raise RuntimeError("服务器关闭连接")

        data += chunk

    return data


def recv_response(sock):

    header = recv_exact(sock, 16)

    magic, version, message_type, request_id, payload_length = \
        struct.unpack(">4sHHII", header)

    print()
    print("========================================")
    print("收到 PYSM 响应")
    print("magic        :", magic)
    print("version      :", version)
    print("messageType  :", hex(message_type))
    print("requestId    :", request_id)
    print("payloadLength:", payload_length)
    print("========================================")

    payload = b""

    if payload_length > 0:
        payload = recv_exact(
            sock,
            payload_length
        )

    print("payload:")
    print(payload.decode(
        "utf-8",
        errors="replace"
    ))

    print("========================================")

    return payload


def test(message_type, name):

    print()
    print("########################################")
    print("测试:", name)
    print("messageType:", message_type)
    print("########################################")

    sock = socket.socket(
        socket.AF_INET,
        socket.SOCK_STREAM
    )

    sock.settimeout(5)

    try:

        sock.connect(
            (HOST, PORT)
        )

        print(
            "TCP连接成功:",
            HOST,
            PORT
        )

        request_id = 1

        request = make_request(
            message_type,
            request_id
        )

        print(
            "发送 HEX:",
            request.hex(" ")
        )

        sock.sendall(request)

        recv_response(sock)

    except Exception as e:

        print(
            "测试失败:",
            e
        )

    finally:

        sock.close()


if __name__ == "__main__":

    test(
        MSG_PING,
        "PING"
    )

    test(
        MSG_SNAPSHOT,
        "SNAPSHOT"
    )
