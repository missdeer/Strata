import json
from pathlib import Path
import time
import urllib.error
import urllib.request

folder = Path(__file__).resolve().parent / "dual-host-test"
key = (folder / "network.key").read_text().strip()
base = "http://127.0.0.1:18080"
headers = {"Authorization": "Bearer " + key, "Content-Type": "application/json"}
deadline = time.monotonic() + 180
next_report = 0
while True:
    try:
        request = urllib.request.Request(base + "/health", headers=headers)
        with urllib.request.urlopen(request, timeout=3) as response:
            status = json.load(response)
        print("API health:", status, flush=True)
        break
    except (OSError, urllib.error.URLError):
        now = time.monotonic()
        if now >= deadline:
            raise SystemExit("Coordinator did not become healthy within 180 seconds")
        if now >= next_report:
            print("Waiting for host16 model startup...", flush=True)
            next_report = now + 15
        time.sleep(2)

results = []
for prompt in ("请直接回答：2+2等于几？", "Write a Python function that returns the square of a number. No explanation."):
    body = {"model": "qwen3.8-flash-next-iq3_s-network", "messages": [{"role": "user", "content": prompt}],
            "max_tokens": 64, "temperature": 0, "reasoning_effort": "none", "stream": False}
    request = urllib.request.Request(base + "/v1/chat/completions", headers=headers,
                                     data=json.dumps(body, ensure_ascii=False).encode("utf-8"))
    print("Sending prompt:", prompt, flush=True)
    started = time.perf_counter()
    with urllib.request.urlopen(request, timeout=180) as response:
        data = json.load(response)
    elapsed = time.perf_counter() - started
    result = {"prompt": prompt, "elapsed_s": elapsed, "response": data}
    results.append(result)
    (folder / "api-results.json").write_text(json.dumps(results, ensure_ascii=False, indent=2), encoding="utf-8")
    choice = data.get("choices", [{}])[0]
    print(json.dumps({"elapsed_s": round(elapsed, 3), "usage": data.get("usage"),
                      "finish_reason": choice.get("finish_reason"), "message": choice.get("message")},
                     ensure_ascii=False), flush=True)
    if data.get("usage", {}).get("completion_tokens", 0) < 1:
        raise SystemExit("The engine produced no tokens")
print("Dual-host HTTP generation: PASS", flush=True)
