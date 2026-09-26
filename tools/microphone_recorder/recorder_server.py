import argparse
import json
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
from urllib.parse import urlparse


def main() -> None:
    parser = argparse.ArgumentParser(description="Local microphone capture receiver")
    parser.add_argument("--port", type=int, default=8765)
    parser.add_argument("--output", type=Path, required=True)
    arguments = parser.parse_args()
    output = arguments.output.resolve()
    output.mkdir(parents=True, exist_ok=True)
    page = Path(__file__).with_name("recorder.html").read_bytes()

    class Handler(BaseHTTPRequestHandler):
        def do_GET(self) -> None:
            if urlparse(self.path).path != "/":
                self.send_error(404)
                return
            self.send_response(200)
            self.send_header("Content-Type", "text/html; charset=utf-8")
            self.send_header("Content-Length", str(len(page)))
            self.end_headers()
            self.wfile.write(page)

        def do_POST(self) -> None:
            route = urlparse(self.path).path
            target = {
                "/recording.wav": output / "microphone.wav",
                "/metrics.json": output / "microphone-metrics.json",
            }.get(route)
            if target is None:
                self.send_error(404)
                return
            length = int(self.headers.get("Content-Length", "0"))
            if length <= 0 or length > 256 * 1024 * 1024:
                self.send_error(400)
                return
            payload = self.rfile.read(length)
            target.write_bytes(payload)
            self.send_response(204)
            self.end_headers()
            print(json.dumps({"saved": str(target), "bytes": len(payload)}), flush=True)

        def log_message(self, format: str, *args: object) -> None:
            return

    server = ThreadingHTTPServer(("127.0.0.1", arguments.port), Handler)
    print(json.dumps({"listening": f"http://127.0.0.1:{arguments.port}/",
                      "output": str(output)}), flush=True)
    server.serve_forever()


if __name__ == "__main__":
    main()
