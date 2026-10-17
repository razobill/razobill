# =============================================================================
# in-memory AWS S3 REST API emulator for local integration testing
#
# supports DuckDB httpfs client: single-part PUT, multi-part upload, range
# GET requests, HEAD, and DELETE without external MinIO/S3 daemon dependencies
# ~ in-memory S3 mock eliminating external LocalStack or MinIO dependencies
# =============================================================================

import hashlib
from http.server import BaseHTTPRequestHandler, HTTPServer
import re
import threading
import time
from urllib.parse import parse_qs, urlparse
import uuid


# thread-safe in-memory storage tracking buckets, keys, and multipart state
class MockS3Storage:
    """Thread-safe in-memory object store for mock S3 buckets and multipart state."""

    def __init__(self):
        self.lock = threading.Lock()
        self.buckets = {"telemetry": {}, "test-bucket": {}}
        self.etags = {"telemetry": {}, "test-bucket": {}}
        self.multipart_uploads = {}  # upload_id -> {"bucket": b, "key": k, "parts": {part_num: data}}

    def ensure_bucket(self, bucket):
        with self.lock:
            if bucket not in self.buckets:
                self.buckets[bucket] = {}
                self.etags[bucket] = {}

    def put_object(self, bucket, key, data):
        with self.lock:
            if bucket not in self.buckets:
                self.buckets[bucket] = {}
                self.etags[bucket] = {}
            self.buckets[bucket][key] = data
            etag = hashlib.md5(data).hexdigest()
            self.etags[bucket][key] = etag
            return etag

    def get_object(self, bucket, key):
        with self.lock:
            if bucket in self.buckets and key in self.buckets[bucket]:
                return self.buckets[bucket][key]
            return None

    def get_etag(self, bucket, key):
        with self.lock:
            if bucket in self.etags and key in self.etags[bucket]:
                return self.etags[bucket][key]
            data = self.buckets.get(bucket, {}).get(key)
            if data is not None:
                etag = hashlib.md5(data).hexdigest()
                self.etags[bucket][key] = etag
                return etag
            return None

    def has_object(self, bucket, key):
        with self.lock:
            return bucket in self.buckets and key in self.buckets[bucket]

    def has_bucket(self, bucket):
        with self.lock:
            return bucket in self.buckets

    def delete_object(self, bucket, key):
        with self.lock:
            if bucket in self.buckets and key in self.buckets[bucket]:
                del self.buckets[bucket][key]
            if bucket in self.etags and key in self.etags[bucket]:
                del self.etags[bucket][key]

    def init_multipart(self, bucket, key):
        upload_id = str(uuid.uuid4())
        with self.lock:
            self.multipart_uploads[upload_id] = {
                "bucket": bucket,
                "key": key,
                "parts": {},
            }
        return upload_id

    def put_part(self, upload_id, part_num, data):
        with self.lock:
            if upload_id in self.multipart_uploads:
                self.multipart_uploads[upload_id]["parts"][part_num] = data
                return True
            return False

    def complete_multipart(self, upload_id):
        with self.lock:
            if upload_id not in self.multipart_uploads:
                return None
            info = self.multipart_uploads[upload_id]
            bucket = info["bucket"]
            key = info["key"]
            parts = info["parts"]
            sorted_part_nums = sorted(parts.keys())
            combined = b"".join(parts[n] for n in sorted_part_nums)
            if bucket not in self.buckets:
                self.buckets[bucket] = {}
                self.etags[bucket] = {}
            self.buckets[bucket][key] = combined
            etag = hashlib.md5(combined).hexdigest()
            self.etags[bucket][key] = etag
            del self.multipart_uploads[upload_id]
            return bucket, key, combined, etag

    def abort_multipart(self, upload_id):
        with self.lock:
            if upload_id in self.multipart_uploads:
                del self.multipart_uploads[upload_id]


class MockS3Handler(BaseHTTPRequestHandler):
    storage = MockS3Storage()

    def setup(self):
        super().setup()
        if hasattr(self, "connection") and self.connection:
            try:
                self.connection.settimeout(5.0)
            except (OSError, AttributeError):
                # Socket connection may not support timeout or already be closed
                pass

    def log_message(self, format, *args):
        # Silence default request logging to keep test output clean
        pass

    def _parse_path(self):
        parsed = urlparse(self.path)
        path = parsed.path.lstrip("/")
        parts = path.split("/", 1)
        bucket = parts[0] if len(parts) > 0 and parts[0] else ""
        key = parts[1] if len(parts) > 1 else ""
        query = parse_qs(parsed.query)
        return bucket, key, query

    def _read_body(self):
        content_length = self.headers.get("Content-Length")
        transfer_encoding = self.headers.get("Transfer-Encoding", "")

        try:
            if content_length:
                length = int(content_length)
                return self.rfile.read(length)
            elif "chunked" in transfer_encoding.lower():
                chunks = []
                while True:
                    line = self.rfile.readline().strip()
                    if not line:
                        break
                    chunk_len = int(line, 16)
                    if chunk_len == 0:
                        self.rfile.readline()  # trailing CRLF
                        break
                    chunks.append(self.rfile.read(chunk_len))
                    self.rfile.readline()  # trailing CRLF
                return b"".join(chunks)
        except (TimeoutError, OSError):
            return b""
        return b""

    def do_HEAD(self):
        bucket, key, query = self._parse_path()
        if not bucket:
            self.send_response(200)
            self.end_headers()
            return

        if not key:
            # Bucket HEAD check
            self.storage.ensure_bucket(bucket)
            self.send_response(200)
            self.send_header("Content-Type", "application/xml")
            self.end_headers()
            return

        data = self.storage.get_object(bucket, key)
        if data is not None:
            etag = self.storage.get_etag(bucket, key) or hashlib.md5(data).hexdigest()
            self.send_response(200)
            self.send_header("Content-Length", str(len(data)))
            self.send_header("ETag", f'"{etag}"')
            self.send_header("Accept-Ranges", "bytes")
            self.send_header("Content-Type", "application/octet-stream")
            self.end_headers()
        else:
            self.send_response(404)
            self.end_headers()

    def do_GET(self):
        bucket, key, query = self._parse_path()
        if not bucket:
            self.send_response(200)
            self.end_headers()
            return

        if not key:
            # List bucket or bucket check
            self.storage.ensure_bucket(bucket)
            self.send_response(200)
            self.send_header("Content-Type", "application/xml")
            self.end_headers()
            return

        data = self.storage.get_object(bucket, key)
        if data is None:
            self.send_response(404)
            self.end_headers()
            return

        etag = self.storage.get_etag(bucket, key) or hashlib.md5(data).hexdigest()
        total_len = len(data)

        range_header = self.headers.get("Range")
        if range_header and range_header.startswith("bytes="):
            match = re.match(r"bytes=(\d+)-(\d+)?", range_header)
            if match:
                start = int(match.group(1))
                end = int(match.group(2)) if match.group(2) else total_len - 1
                end = min(end, total_len - 1)
                chunk = data[start : end + 1]

                self.send_response(206)
                self.send_header("Content-Range", f"bytes {start}-{end}/{total_len}")
                self.send_header("Content-Length", str(len(chunk)))
                self.send_header("ETag", f'"{etag}"')
                self.send_header("Accept-Ranges", "bytes")
                self.send_header("Content-Type", "application/octet-stream")
                self.end_headers()
                self.wfile.write(chunk)
                return

        self.send_response(200)
        self.send_header("Content-Length", str(total_len))
        self.send_header("ETag", f'"{etag}"')
        self.send_header("Accept-Ranges", "bytes")
        self.send_header("Content-Type", "application/octet-stream")
        self.end_headers()
        self.wfile.write(data)

    def do_PUT(self):
        bucket, key, query = self._parse_path()
        body = self._read_body()

        if not key:
            # Create bucket
            self.storage.ensure_bucket(bucket)
            self.send_response(200)
            self.end_headers()
            return

        if "partNumber" in query and "uploadId" in query:
            part_num = int(query["partNumber"][0])
            upload_id = query["uploadId"][0]
            etag = hashlib.md5(body).hexdigest()
            self.storage.put_part(upload_id, part_num, body)
            self.send_response(200)
            self.send_header("ETag", f'"{etag}"')
            self.end_headers()
            return

        # Single part PUT
        etag = self.storage.put_object(bucket, key, body)
        self.send_response(200)
        self.send_header("ETag", f'"{etag}"')
        self.end_headers()

    def do_POST(self):
        bucket, key, query = self._parse_path()
        _ = self._read_body()

        if "uploads" in query:
            # Initiate Multipart Upload
            upload_id = self.storage.init_multipart(bucket, key)
            xml_resp = f"""<?xml version="1.0" encoding="UTF-8"?>
<InitiateMultipartUploadResult xmlns="http://s3.amazonaws.com/doc/2006-03-01/">
    <Bucket>{bucket}</Bucket>
    <Key>{key}</Key>
    <UploadId>{upload_id}</UploadId>
</InitiateMultipartUploadResult>""".encode("utf-8")
            self.send_response(200)
            self.send_header("Content-Type", "application/xml")
            self.send_header("Content-Length", str(len(xml_resp)))
            self.end_headers()
            self.wfile.write(xml_resp)
            return

        if "uploadId" in query:
            # Complete Multipart Upload
            upload_id = query["uploadId"][0]
            res = self.storage.complete_multipart(upload_id)
            if res:
                b, k, data, etag = res
                xml_resp = f"""<?xml version="1.0" encoding="UTF-8"?>
<CompleteMultipartUploadResult xmlns="http://s3.amazonaws.com/doc/2006-03-01/">
    <Location>http://localhost:9000/{b}/{k}</Location>
    <Bucket>{b}</Bucket>
    <Key>{k}</Key>
    <ETag>"{etag}"</ETag>
</CompleteMultipartUploadResult>""".encode("utf-8")
                self.send_response(200)
                self.send_header("Content-Type", "application/xml")
                self.send_header("Content-Length", str(len(xml_resp)))
                self.end_headers()
                self.wfile.write(xml_resp)
            else:
                self.send_response(400)
                self.end_headers()
            return

        self.send_response(200)
        self.end_headers()

    def do_DELETE(self):
        bucket, key, query = self._parse_path()
        if "uploadId" in query:
            upload_id = query["uploadId"][0]
            self.storage.abort_multipart(upload_id)
            self.send_response(204)
            self.end_headers()
            return

        if bucket and key:
            self.storage.delete_object(bucket, key)
        self.send_response(204)
        self.end_headers()


class MockS3Server:
    def __init__(self, host="127.0.0.1", port=9000):
        self.host = host
        self.port = port
        self.httpd = None
        self.thread = None
        self.storage = MockS3Handler.storage

    def start(self):
        # If port 9000 is occupied, pick an available port
        import socket

        sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        result = sock.connect_ex((self.host, self.port))
        sock.close()
        if result == 0:
            # Port is in use, could be an already running MinIO or mock
            print(f"[*] Port {self.port} already in use. Attempting to bind next available port...")
            self.port = 9100

        self.httpd = HTTPServer((self.host, self.port), MockS3Handler)
        self.thread = threading.Thread(target=self.httpd.serve_forever, daemon=True)
        self.thread.start()
        print(f"[*] Mock S3 Server listening on http://{self.host}:{self.port}")
        return self.port

    def stop(self):
        if self.httpd:
            self.httpd.shutdown()
            self.httpd.server_close()
            print("[*] Mock S3 Server stopped.")


if __name__ == "__main__":
    server = MockS3Server()
    assignedPort = server.start()
    print(f"[*] Mock S3 Server listening on port {assignedPort}")
    try:
        while True:
            time.sleep(1)
    except KeyboardInterrupt:
        server.stop()
