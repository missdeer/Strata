"""Stage 4 HTTP/pipe differential tests. Python is an oracle/driver, never a native runtime dependency."""
from __future__ import annotations
import argparse
from concurrent.futures import ThreadPoolExecutor
from contextlib import contextmanager
import http.client
import json
import os
from pathlib import Path
import socket
import subprocess
import sys
import tempfile
import time
import unittest
import shutil
import threading
import base64
import io
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

ROOT = Path(__file__).resolve().parents[1]
sys.path[:0] = [str(ROOT / "tmp/native-text-test-deps"), str(ROOT), str(ROOT / "tools")]
import strata_tokenizer as ST
from serve.test_detok import synthetic_tokenizer

parser = argparse.ArgumentParser()
parser.add_argument("--exe", type=Path, required=True)
parser.add_argument("--engine", type=Path, required=True)
parser.add_argument("--report", type=Path)
parser.add_argument("--only", help="comma-separated test names for focused development checks")
ARGS = parser.parse_args()
KEY = "native-api-test-key-2026"
COUNTS = {"native_requests": 0, "python_requests": 0}

def port():
    with socket.socket() as sock:
        sock.bind(("127.0.0.1", 0))
        return sock.getsockname()[1]

def save_tokenizer(tok, folder):
    folder.mkdir()
    (folder / "vocab.json").write_text(json.dumps(tok.ids, ensure_ascii=False), encoding="utf-8")
    (folder / "merges.txt").write_text("\n".join(" ".join(pair) for pair in tok.ranks), encoding="utf-8")
    (folder / "token_type.json").write_text(json.dumps(tok.token_types), encoding="utf-8")
    (folder / "tokenizer.json").write_text(json.dumps({"model": "gpt2", "pre": "qwen35", "special_ids": tok.special_ids}), encoding="utf-8")

class Running:
    def __init__(self, folder, cfg, native=True, no_python=False):
        self.port = port()
        self.native = native
        self.engine_cwd=Path(cfg.get("cwd",ROOT)).resolve()
        self.config = folder / ("native.json" if native else "python.json")
        self.config.write_text(json.dumps(cfg), encoding="utf-8")
        args = [str(ARGS.exe.resolve()), "serve"] if native else [sys.executable, "-m", "serve.server"]
        env = dict(os.environ)
        env.pop("STRATA_API_KEY", None)
        env["PYTHONPATH"] = os.pathsep.join(sys.path)
        if no_python:
            env["PATH"] = str(Path(env.get("SYSTEMROOT", "C:/Windows")) / "System32")
        self.log = (folder / ("native-server.log" if native else "python-server.log")).open("w", encoding="utf-8")
        self.proc = subprocess.Popen(args + ["--engine", "strata", "--config", str(self.config), "--port", str(self.port)],
                                     cwd=ROOT, env=env, stdout=self.log, stderr=subprocess.STDOUT)
        end = time.monotonic() + 20
        while time.monotonic() < end:
            if self.proc.poll() is not None:
                self.log.flush()
                diagnostic=Path(self.log.name).read_text(encoding="utf-8");self.log.close();raise AssertionError(diagnostic)
            try:
                if self.request("GET", "/health")[0] == 200: return
            except OSError: pass
            time.sleep(.05)
        self.close()
        raise AssertionError("server did not start")
    def request(self, method, path, body=None, headers=None, chunked=False):
        COUNTS["native_requests" if self.native else "python_requests"] += 1
        conn = http.client.HTTPConnection("127.0.0.1", self.port, timeout=15)
        hdr = {"Authorization": "Bearer " + KEY, "Content-Type": "application/json", **(headers or {})}
        data = None if body is None else json.dumps(body, ensure_ascii=False).encode()
        if chunked and data is not None: data = [data[:7], data[7:]]
        conn.request(method, path, body=data, headers=hdr, encode_chunked=chunked)
        res = conn.getresponse()
        raw = res.read().decode("utf-8")
        content_type = res.getheader("Content-Type", "")
        status = res.status
        conn.close()
        return status, json.loads(raw) if "application/json" in content_type else raw
    def close(self):
        self.proc.terminate()
        self.proc.wait(timeout=10)
        self.log.close()
        # Remove only caches bearing this test-owned service PID, after its job ended.
        for parent in {Path(tempfile.gettempdir()).resolve(),self.engine_cwd}:
            for cache in parent.glob(f"strata-vision-{self.proc.pid}_*"):
                if cache.resolve().parent==parent and cache.is_dir():
                    deadline=time.monotonic()+5
                    while True:
                        try:shutil.rmtree(cache);break
                        except PermissionError:
                            if time.monotonic()>=deadline:raise
                            time.sleep(.05)

class ServerTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix="native-server-中文-", dir=ROOT / "tmp")
        self.folder = Path(self.temp.name)
        self.tok = synthetic_tokenizer(n_merges=200)
        # Both stop controls must exist: Service.stop_ids encodes both literals.
        self.tok = ST.Tokenizer(self.tok.tokens + ["<|endoftext|>","<|vision_start|>","<|vision_end|>","<|image_pad|>"],
                                [" ".join(pair) for pair in self.tok.ranks], self.tok.token_types + [3,3,3,3])
        save_tokenizer(self.tok, self.folder / "tokenizer")
        self.active = []
    def tearDown(self):
        for svc in reversed(self.active): svc.close()
        # A forcibly ended Windows job closes descendant log handles asynchronously.
        deadline=time.monotonic()+5
        while True:
            try:self.temp.cleanup();break
            except PermissionError:
                if time.monotonic()>=deadline:raise
                time.sleep(.05)
    def start(self, text="Hello, 世界!", native=True, no_python=False, **extra):
        script = text if isinstance(text, list) else [text]
        tokens = ";".join(x if x in ("CRASH", "STALL") else ",".join(map(str, self.tok.encode(x) + [self.tok.ids["<|im_end|>"]])) for x in script)
        script_file=None
        if len(tokens)>20000:
            script_file=ROOT/"tmp"/(self.folder.name.replace("中文","ascii")+("-native" if native else "-python")+"-script.txt");script_file.write_text(tokens,encoding="ascii");self.addCleanup(lambda:script_file.unlink(missing_ok=True))
        cfg = {"exe": str(ARGS.engine.resolve()), "args": ["--max-context", "4096"], "cwd": str(ROOT),
               "tokenizer": str(self.folder / "tokenizer"), "model_name": "test-native", "api_key": KEY,
               "host": "127.0.0.1", "open_browser": False, "api_monitor": True,
               "env": {"STRATA_TEST_TOKENS": tokens, "STRATA_TEST_CAPTURE": str(self.folder / "captured.txt")},
               **extra}
        if script_file:
            cfg["env"].pop("STRATA_TEST_TOKENS",None);cfg["env"]["STRATA_TEST_SCRIPT_FILE"]=str(script_file)
        svc = Running(self.folder, cfg, native, no_python)
        self.active.append(svc)
        return svc
    def chat(self, **kw):
        return {"model": "test-native", "messages": [{"role": "user", "content": "Hi"}],
                "reasoning_effort": "none", "max_tokens": 200, **kw}
    def test_chat_python_parity_and_chunked(self):
        a, b = self.start(), self.start(native=False)
        req = self.chat(temperature=.7, top_p=.9, top_k=12, repetition_penalty=1.1)
        sa, x = a.request("POST", "/v1/chat/completions", req, chunked=True)
        sb, y = b.request("POST", "/v1/chat/completions", req)
        self.assertEqual((sa, sb), (200, 200), (x, y))
        self.assertEqual(x["choices"], y["choices"])
        self.assertEqual(x["usage"], y["usage"])
        captured = (self.folder / "captured.txt").read_text()
        self.assertIn("temperature=0.7 top_p=0.9 top_k=12 penalty_repeat=1.1 penalty_last_n=64", captured)
    def test_chat_stream_matches_json(self):
        svc = self.start()
        status, stream = svc.request("POST", "/v1/chat/completions", self.chat(stream=True))
        self.assertEqual(status, 200)
        chunks = [json.loads(line[6:]) for line in stream.splitlines() if line.startswith("data: ") and line != "data: [DONE]"]
        text = "".join(c["choices"][0]["delta"].get("content", "") for c in chunks)
        self.assertEqual(text, "Hello, 世界!")
        self.assertEqual(chunks[-1]["choices"][0]["finish_reason"], "stop")
        self.assertIn("completion_tokens", chunks[-1]["usage"])
    def test_thinking_and_tools_python_parity(self):
        raw = 'analysis here</think>\n<tool_call>\n<function=weather>\n<parameter=city>上海</parameter>\n</function>\n</tool_call>'
        a, b = self.start(raw), self.start(raw, native=False)
        req = self.chat(reasoning_effort="high", tools=[{"type":"function", "function":{"name":"weather", "parameters":{"type":"object", "properties":{"city":{"type":"string"}}}}}])
        xa = a.request("POST", "/v1/chat/completions", req)[1]
        xb = b.request("POST", "/v1/chat/completions", req)[1]
        for x in (xa, xb):
            for call in x["choices"][0]["message"].get("tool_calls", []): call["id"] = "ID"
        self.assertEqual(xa["choices"], xb["choices"])
        self.assertEqual(xa["usage"], xb["usage"])
    def test_anthropic_python_parity_and_count(self):
        a, b = self.start(), self.start(native=False)
        req = {"model":"test-native", "messages":[{"role":"user","content":"Hi"}], "max_tokens":200, "thinking":{"type":"disabled"}}
        xa = a.request("POST", "/v1/messages?beta=true", req)[1]
        xb = b.request("POST", "/v1/messages?beta=true", req)[1]
        for x in (xa, xb): x.pop("id")
        self.assertEqual(xa, xb)
        self.assertEqual(a.request("POST", "/v1/messages/count_tokens", req), b.request("POST", "/v1/messages/count_tokens", req))
        status, stream = a.request("POST", "/v1/messages", {**req, "stream":True})
        self.assertEqual(status, 200)
        self.assertIn("event: message_start", stream)
        self.assertIn("event: message_stop", stream)
    def test_responses_python_parity_and_replay(self):
        a, b = self.start(), self.start(native=False)
        req = {"model":"test-native", "input":"Hi", "reasoning":{"effort":"none"}, "max_output_tokens":200, "store":False}
        xa = a.request("POST", "/v1/responses", req)[1]
        xb = b.request("POST", "/v1/responses", req)[1]
        self.assertEqual(xa["status"], xb["status"])
        self.assertEqual(xa["usage"], xb["usage"])
        for x in (xa, xb):
            for item in x["output"]: item.pop("id")
        self.assertEqual(xa["output"], xb["output"])
        _, stream = a.request("POST", "/v1/responses", {**req,"stream":True})
        events = [json.loads(line[6:]) for line in stream.splitlines() if line.startswith("data: ")]
        self.assertEqual([e["sequence_number"] for e in events], list(range(len(events))))
        self.assertEqual(events[0]["type"], "response.created")
        self.assertEqual(events[-1]["type"], "response.completed")
        self.assertEqual(events[-1]["response"]["output"][0]["content"][0]["text"], "Hello, 世界!")
    def test_stop_and_length(self):
        svc = self.start("abcdSTOPtail")
        _, result = svc.request("POST", "/v1/chat/completions", self.chat(stop="STOP"))
        self.assertEqual(result["choices"][0]["message"]["content"], "abcd")
        _, result = svc.request("POST", "/v1/chat/completions", self.chat(max_tokens=2))
        self.assertEqual(result["choices"][0]["finish_reason"], "length")
        self.assertEqual(result["usage"]["completion_tokens"], 2)
    def test_context_and_invalid_body(self):
        svc = self.start()
        for req in (self.chat(max_tokens=9000), self.chat(max_tokens=True), self.chat(stream="yes"), self.chat(n=2)):
            self.assertEqual(svc.request("POST", "/v1/chat/completions", req)[0], 400)
        self.assertEqual(svc.request("POST", "/v1/responses", {"input":"hi", "previous_response_id":"x"})[0], 400)
        conn = http.client.HTTPConnection("127.0.0.1", svc.port)
        conn.request("POST", "/v1/chat/completions", '{"messages":[],"messages":[]}', {"Authorization":"Bearer "+KEY})
        self.assertEqual(conn.getresponse().status, 400)
        conn.close()
    def test_auth_and_web_monitor(self):
        svc = self.start(no_python=True)
        self.assertEqual(svc.request("GET", "/v1/models", headers={"Authorization":"Bearer wrong"})[0], 401)
        self.assertEqual(svc.request("GET", "/v1/models", headers={"Authorization":"", "x-api-key":KEY})[0], 200)
        self.assertEqual(svc.request("GET", "/health")[0], 200)
        self.assertEqual(svc.request("GET", "/", headers={"Host":"evil.example"})[0], 200) # a configured API key disables Python's Host restriction
        for path in ("/", "/web/app.js", "/web/app.css", "/fonts/outfit-latin-wght.woff2", "/api-monitor"):
            if path.endswith("woff2"):
                # binary font response: only inspect the HTTP status
                conn = http.client.HTTPConnection("127.0.0.1", svc.port)
                conn.request("GET", path); res = conn.getresponse(); self.assertEqual(res.status, 200); res.read(); conn.close()
            else: self.assertEqual(svc.request("GET", path)[0], 200)
        svc.request("POST", "/v1/chat/completions", self.chat())
        self.assertEqual(len(svc.request("GET", "/api/requests")[1]["requests"]), 1)
        self.assertEqual(svc.request("GET", "/metrics")[1]["totals"]["requests"], 1)
        self.assertIn("vllm:generation_tokens_total", svc.request("GET", "/metrics?format=prometheus")[1])
    def test_unload_load_and_session_reuse(self):
        svc = self.start()
        _, first = svc.request("POST", "/v1/chat/completions", self.chat())
        _, second = svc.request("POST", "/v1/chat/completions", self.chat())
        self.assertGreater(second["usage"]["prompt_tokens_details"]["cached_tokens"], 0)
        self.assertFalse(svc.request("POST", "/v1/unload", {})[1]["loaded"])
        self.assertFalse(svc.request("GET", "/health")[1]["loaded"])
        self.assertTrue(svc.request("POST", "/v1/load", {})[1]["loaded"])
        self.assertEqual(svc.request("POST", "/v1/chat/completions", self.chat())[0], 200)
    def test_structured_buffers_until_validated(self):
        svc = self.start(['{"ok":true}', 'not json'])
        req = self.chat(response_format={"type":"json_schema", "json_schema":{"name":"test", "schema":{"type":"object", "properties":{"ok":{"type":"boolean"}}, "required":["ok"]}}})
        status, result = svc.request("POST", "/v1/chat/completions", req)
        self.assertEqual(status, 200, result)
        status, stream = svc.request("POST", "/v1/chat/completions", {**req, "stream":True})
        self.assertEqual(status, 200)
        self.assertIn("structured_output_failed", stream)
        self.assertNotIn('not json', stream)
    def test_disconnect_stops_and_next_request_is_clean(self):
        svc = self.start(["x" * 1000, "fresh"], env={"STRATA_TEST_TOKENS": ",".join(map(str,self.tok.encode("x"*1000)))+";"+",".join(map(str,self.tok.encode("fresh"))), "STRATA_TEST_DELAY":"5"})
        conn = http.client.HTTPConnection("127.0.0.1", svc.port, timeout=10)
        conn.request("POST", "/v1/chat/completions", json.dumps(self.chat(stream=True, max_tokens=1500)), {"Authorization":"Bearer "+KEY, "Content-Type":"application/json"})
        res = conn.getresponse()
        for _ in range(15): res.readline()
        res.close(); conn.close()
        status, result = svc.request("POST", "/v1/chat/completions", self.chat())
        self.assertEqual(status, 200, result)
        self.assertEqual(result["choices"][0]["message"]["content"], "fresh")
    def test_fatal_engine_no_replay(self):
        svc = self.start("CRASH")
        self.assertEqual(svc.request("POST", "/v1/chat/completions", self.chat())[0], 503)
        self.assertFalse(svc.request("GET", "/health")[1]["loaded"])
        self.assertEqual(svc.request("POST", "/v1/chat/completions", self.chat())[0], 503)
    def test_next_request_restarts_failed_engine_and_completes(self):
        tokens=",".join(map(str,self.tok.encode("recovered")+[self.tok.ids["<|im_end|>"]]))
        svc=self.start("recovered",env={"STRATA_TEST_TOKENS":tokens,"STRATA_TEST_CRASH_ONCE":str(self.folder/"crashed.once")})
        self.assertEqual(svc.request("POST","/v1/chat/completions",self.chat())[0],503)
        status,result=svc.request("POST","/v1/chat/completions",self.chat())
        self.assertEqual(status,200,result)
        self.assertEqual(result["choices"][0]["message"]["content"],"recovered")
    def test_silent_engine_has_bounded_deadline(self):
        svc = self.start("STALL", engine_silence_s=.2)
        started = time.monotonic()
        self.assertEqual(svc.request("POST", "/v1/chat/completions", self.chat())[0], 503)
        self.assertLess(time.monotonic()-started, 3)
        self.assertFalse(svc.request("GET", "/health")[1]["loaded"])
    def test_fifo_orders_concurrent_requests(self):
        texts = ["first answer", "second answer", "third answer"]
        tokens = ";".join(",".join(map(str,self.tok.encode(x)+[self.tok.ids["<|im_end|>"]])) for x in texts)
        svc = self.start(texts, env={"STRATA_TEST_TOKENS":tokens, "STRATA_TEST_DELAY":"10"})
        with ThreadPoolExecutor(max_workers=3) as pool:
            futures=[]
            for _ in texts:
                futures.append(pool.submit(svc.request,"POST","/v1/chat/completions",self.chat()))
                # Confirm admission before submitting the next request. A fixed
                # sleep does not establish arrival order under compiler load.
                deadline = time.monotonic() + 10
                while True:
                    state = svc.request("GET", "/status")[1]
                    admitted = state["queued"] + int(state["busy"]) + sum(f.done() for f in futures)
                    if admitted >= len(futures): break
                    self.assertLess(time.monotonic(), deadline, "request was not admitted")
                    time.sleep(.01)
            results=[f.result()[1]["choices"][0]["message"]["content"] for f in futures]
        self.assertEqual(results,texts)
    def test_responses_namespace_and_reasoning_replay(self):
        raw='thinking</think>\n<tool_call>\n<function=agent.echo>\n<parameter=text>你好</parameter>\n</function>\n</tool_call>'
        a,b=self.start(raw),self.start(raw,native=False)
        req={"model":"test-native","input":"hi","reasoning":{"effort":"high"},"max_output_tokens":200,"include":["reasoning.encrypted_content"],
             "tools":[{"type":"namespace","name":"agent","tools":[{"type":"function","name":"echo","parameters":{"type":"object","properties":{"text":{"type":"string"}}}}]}]}
        outputs=[]
        for svc in (a,b):
            status,result=svc.request("POST","/v1/responses",req)
            self.assertEqual(status,200,result)
            outputs.append(result)
        for result in outputs:
            for item in result["output"]:
                item.pop("id",None)
                if "call_id" in item:item["call_id"]="CALL"
        self.assertEqual(outputs[0]["output"],outputs[1]["output"])
        self.assertEqual(outputs[0]["usage"],outputs[1]["usage"])
        self.assertEqual(outputs[0]["tools"],req["tools"])
    def test_unsupported_config_fails_before_engine(self):
        for field in ("not_a_server_setting",):
            cfg={"exe":str(ARGS.engine.resolve()), "args":[], "tokenizer":str(self.folder/"tokenizer"), field:{"enabled":True}}
            path=self.folder/"bad.json";path.write_text(json.dumps(cfg),encoding="utf-8")
            p=subprocess.run([str(ARGS.exe.resolve()),"serve","--config",str(path),"--validate"],capture_output=True,text=True)
            self.assertNotEqual(p.returncode,0)
            self.assertIn(field,p.stderr)
    def test_settings_and_config_persist_without_exposing_key(self):
        svc=self.start()
        status,settings=svc.request("POST","/settings",{"defaults":{"max_tokens":7,"temperature":.7}})
        self.assertEqual(status,200,settings)
        self.assertEqual(json.loads((self.folder/"native.shared-settings.json").read_text())["max_tokens"],7)
        status,view=svc.request("POST","/config",{"set":{"fit_max_tokens":True,"sampling.temperature":.5}})
        self.assertEqual(status,200,view)
        self.assertNotIn(KEY,json.dumps(view))
        current=json.loads(svc.config.read_text(encoding="utf-8"))
        backup=json.loads(Path(str(svc.config)+".bak").read_text(encoding="utf-8"))
        self.assertEqual(current["api_key"],KEY)
        self.assertEqual(backup["api_key"],KEY)
        self.assertTrue(current["fit_max_tokens"])
        self.assertNotIn("fit_max_tokens",backup)
        self.assertEqual(svc.request("POST","/config",{"set":{"api_key":"hijack"}})[0],400)
        self.assertEqual(svc.request("POST","/settings",{"defaults":{"max_tokens":True}})[0],400)
    def test_vram_and_disk_sessions(self):
        svc=self.start(slot_save_path=str(self.folder/"sessions"),vram_elastic=True,vram_segment_mib=64)
        svc.request("POST","/v1/chat/completions",self.chat())
        status,saved=svc.request("POST","/slots/0?action=save",{"filename":"会话.session"})
        self.assertEqual(status,200,saved);self.assertGreater(saved["n_saved"],0)
        status,restored=svc.request("POST","/slots/0?action=restore",{"filename":"会话.session"})
        self.assertEqual(status,200,restored);self.assertEqual(restored["n_restored"],saved["n_saved"])
        for filename in ("../outside","NUL.txt","bad:stream",".hidden","trailing."):
            self.assertEqual(svc.request("POST","/slots/0?action=save",{"filename":filename})[0],400)
        self.assertEqual(svc.request("POST","/slots/1?action=save",{"filename":"ok"})[0],400)
        self.assertEqual(svc.request("POST","/slots/0?action=restore",{"filename":"absent"})[0],404)
        status,vram=svc.request("POST","/v1/vram",{"reserve_mib":1024});self.assertEqual(status,200,vram);self.assertEqual(vram["reserve_mib"],1024)
        self.assertEqual(svc.request("POST","/v1/vram",{"reserve_mib":True})[0],400)
        self.assertEqual(svc.request("POST","/v1/chat/completions",self.chat())[0],200)
    def test_parallel_slots_overlap_and_complete_independently(self):
        fixture=ARGS.engine.resolve().with_name("strata-server-batch-test-engine.exe" if os.name=="nt" else "strata-server-batch-test-engine")
        texts=["alpha "*90,"beta "*90,"third"]
        svc=self.start(texts,exe=str(fixture),parallel=2)
        with ThreadPoolExecutor(max_workers=3) as pool:
            a=pool.submit(svc.request,"POST","/v1/chat/completions",self.chat(max_tokens=1500,messages=[{"role":"user","content":"first task"}]));time.sleep(.05)
            b=pool.submit(svc.request,"POST","/v1/chat/completions",self.chat(max_tokens=1500,messages=[{"role":"user","content":"second task"}]))
            seen=False;end=time.monotonic()+3
            while time.monotonic()<end:
                status,slots=svc.request("GET","/slots")
                if len(slots)==2 and all(s["is_processing"] for s in slots):seen=True;break
                time.sleep(.02)
            self.assertTrue(seen,slots)
            self.assertEqual(a.result()[1]["choices"][0]["message"]["content"],texts[0]);self.assertEqual(b.result()[1]["choices"][0]["message"]["content"],texts[1])
        status,result=svc.request("POST","/v1/chat/completions",self.chat());self.assertEqual(status,200,result);self.assertEqual(result["choices"][0]["message"]["content"],"third")
    def test_parallel_cancel_does_not_stop_other_slot(self):
        fixture=ARGS.engine.resolve().with_name("strata-server-batch-test-engine.exe" if os.name=="nt" else "strata-server-batch-test-engine")
        svc=self.start(["x"*1000,"survived","fresh"],exe=str(fixture),parallel=2)
        conn=http.client.HTTPConnection("127.0.0.1",svc.port,timeout=10)
        conn.request("POST","/v1/chat/completions",json.dumps(self.chat(stream=True,max_tokens=1500,messages=[{"role":"user","content":"first task"}])),{"Authorization":"Bearer "+KEY,"Content-Type":"application/json"})
        stream=conn.getresponse()
        with ThreadPoolExecutor(max_workers=1) as pool:
            other=pool.submit(svc.request,"POST","/v1/chat/completions",self.chat())
            for _ in range(15):stream.readline()
            stream.close();conn.close()
            status,result=other.result();self.assertEqual(status,200,result);self.assertEqual(result["choices"][0]["message"]["content"],"survived")
        self.assertEqual(svc.request("POST","/v1/chat/completions",self.chat())[1]["choices"][0]["message"]["content"],"fresh")
    def test_mcp_stdio_tool_round_and_private_environment(self):
        fixture=ARGS.engine.resolve().with_name("strata-mcp-test-server.exe" if os.name=="nt" else "strata-mcp-test-server")
        raw="<tool_call>\n<function=test__echo>\n<parameter=text>hello</parameter>\n</function>\n</tool_call>"
        svc=self.start([raw,"tool done"],mcp_servers={"test":{"command":str(fixture),"env":{"MCP_TEST_PREFIX":"child-only-"}}})
        status,result=svc.request("POST","/v1/chat/completions",self.chat(strata_mcp=True,max_tokens=500))
        self.assertEqual(status,200,result);msg=result["choices"][0]["message"]
        self.assertEqual(msg["content"],"tool done");self.assertNotIn("tool_calls",msg)
        self.assertEqual([e["event"] for e in msg["strata_mcp"]],["start","call","result"])
        self.assertEqual(msg["strata_mcp"][-1]["text"],"child-only-hello")
        self.assertEqual(result["usage"]["completion_tokens"],len(self.tok.encode(raw))+len(self.tok.encode("tool done"))+2)
        self.assertEqual(svc.request("GET","/mcp")[1]["tools"],3)
        captured=(self.folder/"captured.txt").read_text(encoding="utf-8").splitlines()
        self.assertEqual(len(captured),2);ids=list(map(int,captured[-1].split()[-1].split(',')))
        self.assertIn("child-only-hello",self.tok.decode(ids))
    def test_mcp_streamable_http_session_and_sse(self):
        observed=[]
        class Handler(BaseHTTPRequestHandler):
            protocol_version="HTTP/1.1"
            def log_message(self,*args):pass
            def handle(self):
                try:super().handle()
                except ConnectionResetError:pass # native client stops after the matching SSE response
            def do_POST(self):
                req=json.loads(self.rfile.read(int(self.headers.get("Content-Length",0))))
                method=req.get("method");observed.append((method,self.headers.get("Mcp-Session-Id"),self.headers.get("Authorization")))
                if "id" not in req:
                    self.send_response(202);self.send_header("Content-Length","0");self.end_headers();return
                if method=="initialize":result={"protocolVersion":"2025-06-18","serverInfo":{"name":"http-fixture","version":"1"},"capabilities":{"tools":{}}}
                elif method=="tools/list":result={"tools":[{"name":"echo","description":"Echo text","inputSchema":{"type":"object","properties":{"text":{"type":"string"}}}}]}
                else:result={"content":[{"type":"text","text":req["params"]["arguments"]["text"]}]}
                response=json.dumps({"jsonrpc":"2.0","id":req["id"],"result":result}).encode()
                self.send_response(200);self.send_header("Mcp-Session-Id","native-session")
                if method=="tools/list":
                    self.send_header("Content-Type","text/event-stream");self.send_header("Transfer-Encoding","chunked");self.end_headers()
                    for data in (b'data: {"jsonrpc":"2.0","method":"notifications/progress"}\n\n',b'data: '+response+b'\n\n'):
                        self.wfile.write(f"{len(data):x}\r\n".encode()+data+b"\r\n");self.wfile.flush()
                    self.wfile.write(b"0\r\n\r\n");self.wfile.flush()
                else:
                    self.send_header("Content-Type","application/json");self.send_header("Content-Length",str(len(response)));self.end_headers();self.wfile.write(response)
        server=ThreadingHTTPServer(("127.0.0.1",0),Handler);server.daemon_threads=True;thread=threading.Thread(target=server.serve_forever,daemon=True);thread.start()
        try:
            raw="<tool_call>\n<function=remote__echo>\n<parameter=text>network hello</parameter>\n</function>\n</tool_call>"
            svc=self.start([raw,"HTTP done"],mcp_servers={"remote":{"url":f"http://127.0.0.1:{server.server_port}/mcp","headers":{"Authorization":"Bearer fixture-private-key"}}})
            status,result=svc.request("POST","/v1/chat/completions",self.chat(strata_mcp=True,max_tokens=500))
            self.assertEqual(status,200,result);self.assertEqual(result["choices"][0]["message"]["content"],"HTTP done")
            self.assertEqual(result["choices"][0]["message"]["strata_mcp"][-1]["text"],"network hello")
            self.assertTrue(all(h=="Bearer fixture-private-key" for _,_,h in observed));self.assertTrue(all(s=="native-session" for m,s,_ in observed if m!="initialize"))
            self.assertNotIn("fixture-private-key",json.dumps(svc.request("GET","/mcp")[1]))
        finally:server.shutdown();server.server_close();thread.join()
    def test_vision_uses_host_without_an_external_encoder(self):
        for override in (False, True):
            log = self.folder / ("builtin-explicit.log" if override else "builtin-default.log")
            cfg = {"mmproj": str(self.folder / "missing-projector.gguf"),
                   "model": str(self.folder / "missing-model.gguf")}
            if override: cfg["exe"] = str(ARGS.exe.resolve())
            svc = self.start(lazy_load=True, vision=cfg, log=str(log))
            status, result = svc.request("POST", "/v1/load", {})
            self.assertEqual(status, 503, result)
            # The linked encoder actually ran and failed on its model. A
            # missing subcommand or sibling exe must not produce this result.
            diagnostic = log.read_text(encoding="utf-8", errors="replace")
            self.assertIn("missing-model.gguf", diagnostic)
            self.assertNotIn("unknown command", diagnostic)
            self.assertFalse(svc.request("GET", "/health")[1]["loaded"])

    def test_vision_generation_count_tokens_and_cache(self):
        fixture=ARGS.engine.resolve().with_name("strata-vision-test-encoder.exe" if os.name=="nt" else "strata-vision-test-encoder")
        png=base64.b64decode("iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAQAAAC1HAwCAAAAC0lEQVR42mP8/x8AAwMCAO+aYu0AAAAASUVORK5CYII=")
        data="data:image/png;base64,"+base64.b64encode(png).decode()
        # The native fixture uses narrow getenv; keep this fixture-only path ASCII.
        captured=ROOT/"tmp"/(self.folder.name.replace("中文","ascii")+"-encoded.txt")
        cfg={"vision":{"exe":str(fixture),"mmproj":"unused.gguf","model":"unused.gguf"},"env":{"STRATA_TEST_TOKENS":",".join(map(str,self.tok.encode("image done")+[self.tok.ids["<|im_end|>"]])) ,"STRATA_TEST_IMAGE_CAPTURE":str(captured)}}
        a=self.start("image done",**cfg)
        req=self.chat(messages=[{"role":"user","content":[{"type":"text","text":"describe"},{"type":"image_url","image_url":{"url":data}}]}])
        status,result=a.request("POST","/v1/chat/completions",req);self.assertEqual(status,200,result);self.assertEqual(result["choices"][0]["message"]["content"],"image done")
        self.assertTrue(a.request("GET","/health")[1]["images"])
        self.assertEqual(a.request("POST","/v1/chat/completions",req)[0],200)
        self.assertEqual(len(captured.read_text().splitlines()),1);captured.unlink()
        request={"messages":[{"role":"user","content":[{"type":"image","source":{"type":"base64","media_type":"image/png","data":base64.b64encode(png).decode()}}]}],"thinking":{"type":"disabled"}}
        native=a.request("POST","/v1/messages/count_tokens",request)
        b=self.start("image done",native=False,vision=cfg["vision"])
        self.assertEqual(native,b.request("POST","/v1/messages/count_tokens",request))
    def test_vision_more_than_cache_capacity_keeps_request_files(self):
        fixture=ARGS.engine.resolve().with_name("strata-vision-test-encoder.exe" if os.name=="nt" else "strata-vision-test-encoder")
        png=base64.b64decode("iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAQAAAC1HAwCAAAAC0lEQVR42mP8/x8AAwMCAO+aYu0AAAAASUVORK5CYII=")
        svc=self.start("many images",vision={"exe":str(fixture),"mmproj":"unused","model":"unused"})
        content=[{"type":"image_url","image_url":{"url":"data:image/png;base64,"+base64.b64encode(png+bytes([i])).decode()}} for i in range(70)]
        status,result=svc.request("POST","/v1/chat/completions",self.chat(messages=[{"role":"user","content":content}]))
        self.assertEqual(status,200,result)
    def test_native_exif_orientation(self):
        from PIL import Image,ImageOps,ImageChops
        checker=ARGS.exe.resolve().with_name("strata-server-check.exe" if os.name=="nt" else "strata-server-check")
        original=Image.new("RGB",(12,8));original.putdata([(x*17,y*23,(x+y)*9) for y in range(8) for x in range(12)])
        for orientation in range(1,9):
            exif=Image.Exif();exif[274]=orientation;source=self.folder/f"source-{orientation}.jpg";destination=self.folder/f"normalized-{orientation}.png"
            original.save(source,quality=100,subsampling=0,exif=exif)
            p=subprocess.run([str(checker)],input=json.dumps({"op":"normalize_image","input":str(source),"output":str(destination)})+"\n",capture_output=True,text=True,encoding="utf-8")
            self.assertNotIn("error",json.loads(p.stdout),p.stdout)
            with Image.open(source) as image:expected=ImageOps.exif_transpose(image).convert("RGB")
            with Image.open(destination) as image:actual=image.convert("RGB")
            self.assertEqual(actual.size,expected.size)
            difference=ImageChops.difference(actual,expected);self.assertLessEqual(max(hi for lo,hi in difference.getextrema()),3)
    def test_security_cors_and_own_page(self):
        svc=self.start(api_key="",cors_origins=["https://chat.example"],trusted_origins=["https://trusted.example"],allowed_hosts=[".example.net"])
        self.assertEqual(svc.request("GET","/",headers={"Host":"evil.example"})[0],403)
        self.assertEqual(svc.request("GET","/",headers={"Host":"sub.example.net"})[0],200)
        self.assertEqual(svc.request("POST","/v1/chat/completions",self.chat(),headers={"Origin":"https://evil.example"})[0],403)
        self.assertEqual(svc.request("POST","/v1/chat/completions",self.chat(),headers={"Origin":"https://chat.example"})[0],200)
        self.assertEqual(svc.request("POST","/settings",{"defaults":{}},headers={"Origin":"https://chat.example"})[0],403)
        self.assertEqual(svc.request("POST","/settings",{"defaults":{}},headers={"Origin":"https://trusted.example"})[0],200)
        conn=http.client.HTTPConnection("127.0.0.1",svc.port)
        conn.request("OPTIONS","/v1/chat/completions",headers={"Origin":"https://chat.example","Access-Control-Request-Headers":"Authorization, x-custom"});r=conn.getresponse()
        self.assertEqual(r.status,204);self.assertEqual(r.getheader("Access-Control-Allow-Origin"),"https://chat.example");self.assertEqual(r.getheader("Access-Control-Allow-Headers"),"Authorization, x-custom");r.read();conn.close()
    def test_lazy_idle_unload_and_before_load(self):
        hook=ARGS.exe.resolve().with_name("strata-server-check.exe" if os.name=="nt" else "strata-server-check")
        marker=self.folder/"hook.txt"
        svc=self.start(lazy_load=True,idle_unload_s=.5,before_load=[str(hook),"--touch",str(marker)])
        self.assertFalse(svc.request("GET","/health")[1]["loaded"])
        self.assertEqual(svc.request("POST","/v1/chat/completions",self.chat())[0],200)
        self.assertTrue(marker.is_file())
        end=time.monotonic()+4
        while time.monotonic()<end and svc.request("GET","/health")[1]["loaded"]:time.sleep(.1)
        self.assertFalse(svc.request("GET","/health")[1]["loaded"])
        marker.unlink();self.assertEqual(svc.request("POST","/v1/chat/completions",self.chat())[0],200);self.assertTrue(marker.is_file())
    def test_engine_parameter_derivation_python_parity(self):
        from serve.server import engine_args
        checker=ARGS.exe.resolve().with_name("strata-server-check.exe" if os.name=="nt" else "strata-server-check")
        base={"exe":str(ARGS.engine.resolve()),"args":["--native","unused"],"tokenizer":str(self.folder/"tokenizer"),"cwd":str(self.folder)}
        cases=[{"parallel":2},{"vram_elastic":True,"vram_segment_mib":64},{"gpu":[0,1],"gpu_order":"as_given","layer_split":[24],"split_skip_if_fits":True},{"vision":{"exe":"encoder","mmproj":"projector","model":"model","gpu":True}},{"expert_profile_save":"learned.bin","expert_profile_save_every":0}]
        import struct
        for name in ("base.bin","learned.bin"):(self.folder/name).write_bytes(b"STRP"+struct.pack("<5I",1,48,512,0,1)+struct.pack("<I",0))
        cases.append({"args":["--native","unused","--expert-profile","base.bin"],"expert_profile_save":"learned.bin"})
        for extra in cases:
            cfg={**base,**extra};p=subprocess.run([str(checker)],input=json.dumps({"op":"engine_args","config":cfg})+"\n",capture_output=True,text=True,encoding="utf-8");native=json.loads(p.stdout)
            self.assertNotIn("error",native,native);self.assertEqual(native["result"],["--serve",*engine_args(cfg)])
    def test_prometheus_all_metric_families_python_parity(self):
        from serve.prometheus import render,Latencies
        checker=ARGS.exe.resolve().with_name("strata-server-check.exe" if os.name=="nt" else "strata-server-check")
        latency=Latencies();latency.observe(.123,5,.4,1.2)
        metrics={"engine":{"model":"fixture \"x\"","max_context":4096},"live":{"state":"generating","queued":2,"prompt_tokens":200,"generated":5,"tok_s":12.5,"slots":[{"state":"generating"},{"state":"idle"}]},"totals":{"requests":3,"prompt_tokens":400,"output_tokens":20,"reused":100,"prompt_ms":2345.6,"decode_ms":1234.5,"drafts_offered":12,"drafts_accepted":10},"requests":[{"hit_rate":.4,"decode_tok_s":20.5}],"hardware":{"gpu_util":52,"gpu_mem_used":1000,"gpu_mem_total":2000,"gpu_temp":42,"gpu_power":100,"cpu":33,"ram_used":10000,"ram_total":20000}}
        p=subprocess.run([str(checker)],input=json.dumps({"op":"metrics","metrics":metrics,"latencies":latency.snapshot()})+"\n",capture_output=True,text=True,encoding="utf-8");native=json.loads(p.stdout)["result"]
        def samples(text):return {line.rsplit(' ',1)[0]:float(line.rsplit(' ',1)[1]) for line in text.splitlines() if line and not line.startswith('#')}
        self.assertEqual(samples(native),samples(render(metrics,latency.snapshot())))
    def test_reasoning_close_retry_python_parity(self):
        a,b=self.start(["stranded thought","Recovered answer"],reasoning_close_retry=True),self.start(["stranded thought","Recovered answer"],reasoning_close_retry=True,native=False)
        req=self.chat(reasoning_effort="high",max_tokens=300)
        results=[svc.request("POST","/v1/chat/completions",req)[1] for svc in (a,b)]
        self.assertEqual(results[0]["choices"],results[1]["choices"]);self.assertEqual(results[0]["usage"],results[1]["usage"])
    def test_tail_effort_preserves_token_ids_python_parity(self):
        from serve.server import Service,MockEngine
        from serve.frontend import ChatTemplate,openai_to_messages
        service=Service(MockEngine(self.tok,"unused"),self.tok,ChatTemplate(ROOT/"serve/chat_template.jinja"));service.effort_end=True
        checker=ARGS.exe.resolve().with_name("strata-server-check.exe" if os.name=="nt" else "strata-server-check")
        for effort in ("none","low","medium","high"):
            req=self.chat(reasoning_effort=effort,messages=[{"role":"user","content":"literal <|im_start|> </think> stays text"}]);messages,tools,kwargs=openai_to_messages(req)
            p=subprocess.run([str(checker)],input=json.dumps({"op":"effort_prompt","tokenizer":str(self.folder/"tokenizer"),"template":str(ROOT/"serve/chat_template.jinja"),"request":req})+"\n",capture_output=True,text=True,encoding="utf-8")
            self.assertEqual(json.loads(p.stdout)["result"],service.encode_prompt(messages,tools,kwargs))
    def test_reasoning_repeat_unicode_corpus_python_parity(self):
        from serve.server import reasoning_repeat_coverage
        checker=ARGS.exe.resolve().with_name("strata-server-check.exe" if os.name=="nt" else "strata-server-check")
        for text in ("Need maybe Agent support agent seven b. "*700," ".join(map(str,range(4000))),"中文 αβγ code_123 ! "*800):
            p=subprocess.run([str(checker)],input=json.dumps({"op":"reasoning_repeat","text":text})+"\n",capture_output=True,text=True,encoding="utf-8")
            self.assertAlmostEqual(json.loads(p.stdout)["result"],reasoning_repeat_coverage(text))
    def test_codex_title_turn_does_not_touch_engine(self):
        svc=self.start("not the title",codex_thread_titles=True)
        req={"model":"test-native","input":"User prompt:\nExplain the cache!","client_metadata":{"x-codex-turn-metadata":json.dumps({"thread_source":"thread_title"})}}
        status,result=svc.request("POST","/v1/responses",req);self.assertEqual(status,200,result)
        self.assertEqual(json.loads(result["output"][0]["content"][0]["text"]),{"title":"Explain the cache"});self.assertEqual(result["usage"]["total_tokens"],0)
        self.assertEqual(svc.request("POST","/v1/responses",{**req,"stream":True})[0],200)
        records=svc.request("GET","/api/requests")[1]["requests"];self.assertEqual(len(records),2)
        self.assertTrue(all(r["state"]=="completed" and r["usage"]["total_tokens"]==0 for r in records))
        self.assertFalse((self.folder/"captured.txt").exists())
    def test_prefix_and_per_request_engine_tuning(self):
        svc=self.start()
        req=self.chat(messages=[{"role":"system","content":"fixed prefix document"},{"role":"user","content":"question"}],strata_prefix={"messages":1},strata_checkpoint=False,strata_tune={"pcie_frac":.25,"spec_min_p":.75})
        status,result=svc.request("POST","/v1/chat/completions",req);self.assertEqual(status,200,result)
        command=(self.folder/"captured.txt").read_text(encoding="utf-8");self.assertIn(" pin=",command);self.assertIn(" ckpt=0",command);self.assertIn(" pcie_frac=0.25",command);self.assertIn(" spec_min_p=0.75",command)
        self.assertEqual(svc.request("POST","/v1/chat/completions",self.chat(strata_prefix={"messages":True}))[0],400)
    def test_native_image_formats_composite_like_python(self):
        from PIL import Image,ImageOps,ImageChops
        from serve.server import Vision
        checker=ARGS.exe.resolve().with_name("strata-server-check.exe" if os.name=="nt" else "strata-server-check")
        original=Image.new("RGBA",(16,16),(200,30,50,128))
        for format in ("WEBP","AVIF","TIFF","ICO","PPM"):
            source=self.folder/("image."+format.lower());destination=self.folder/("out-"+format+".png")
            opts={"lossless":True} if format=="WEBP" else {"quality":100,"subsampling":"4:4:4"} if format=="AVIF" else {}
            original.save(source,format=format,**opts)
            p=subprocess.run([str(checker)],input=json.dumps({"op":"normalize_image","input":str(source),"output":str(destination)})+"\n",capture_output=True,text=True,encoding="utf-8")
            result=json.loads(p.stdout);self.assertNotIn("error",result,result)
            expected=Image.open(io.BytesIO(Vision.normalize(source.read_bytes()))).convert("RGB");actual=Image.open(destination).convert("RGB")
            self.assertEqual(expected.size,actual.size);self.assertLessEqual(max(hi for lo,hi in ImageChops.difference(actual,expected).getextrema()),3)
    def test_engine_refusal_stays_in_step_fatal_discards_trailing_done(self):
        response=self.folder/"response.txt";counter=self.folder/"requests.txt"
        env={"STRATA_TEST_TOKENS":",".join(map(str,self.tok.encode("healthy")+[self.tok.ids["<|im_end|>"]])) ,"STRATA_TEST_REQUEST_FILE":str(response),"STRATA_TEST_COUNTER_FILE":str(counter)}
        # Narrow getenv paths are fixture-only; use ASCII file names at an ASCII parent.
        response=ROOT/"tmp"/(self.folder.name.replace("中文","ascii")+"-response.txt");counter=response.with_suffix('.counter');env["STRATA_TEST_REQUEST_FILE"]=str(response);env["STRATA_TEST_COUNTER_FILE"]=str(counter)
        try:
            response.write_text("REFUSE");svc=self.start("healthy",env=env)
            self.assertEqual(svc.request("POST","/v1/chat/completions",self.chat())[0],400)
            self.assertTrue(svc.request("GET","/health")[1]["loaded"])
            response.write_text("FATAL");status,result=svc.request("POST","/v1/chat/completions",self.chat());self.assertEqual(status,503,result)
            self.assertFalse(svc.request("GET","/health")[1]["loaded"])
            self.assertEqual(counter.read_text(),'2')
            response.write_text("OK");status,result=svc.request("POST","/v1/chat/completions",self.chat());self.assertEqual(status,200,result);self.assertEqual(result["choices"][0]["message"]["content"],"healthy")
        finally:
            for path in (response,counter):
                if path.exists():path.unlink()
    def test_responses_structured_json_python_parity(self):
        a,b=self.start('{"ok":true}'),self.start('{"ok":true}',native=False)
        req={"model":"test-native","input":"return json","max_output_tokens":200,"reasoning":{"effort":"none"},"text":{"format":{"type":"json_schema","name":"answer","schema":{"type":"object","properties":{"ok":{"type":"boolean"}},"required":["ok"]}}},"stream":True}
        streams=[svc.request("POST","/v1/responses",req)[1] for svc in (a,b)]
        for text in streams:
            events=[json.loads(line[6:]) for line in text.splitlines() if line.startswith("data: ")]
            self.assertEqual(len([e for e in events if e["type"]=="response.output_text.delta"]),1)
            self.assertEqual(events[-1]["type"],"response.completed")
            self.assertEqual(events[-1]["response"]["output"][0]["content"][0]["text"],'{"ok":true}')
    def test_structured_output_returns_canonical_json(self):
        for native in (True,False):
            svc=self.start('```json\n{ "ok" : true }\n```',native=native)
            status,result=svc.request("POST","/v1/chat/completions",self.chat(response_format={"type":"json_object"}));self.assertEqual(status,200,result);self.assertEqual(result["choices"][0]["message"]["content"],'{"ok":true}')
            status,text=svc.request("POST","/v1/chat/completions",self.chat(stream=True,response_format={"type":"json_object"}));self.assertEqual(status,200,text)
            chunks=[json.loads(line[6:]) for line in text.splitlines() if line.startswith('data: {')]
            self.assertEqual(len(chunks),3);self.assertEqual(chunks[1]["choices"][0]["delta"]["content"],'{"ok":true}')
    def test_reasoning_loop_recovery_stops_and_continues(self):
        loop="Need maybe Agent support agent seven b. "*300
        for recovery in ("stop","recover"):
            req=self.chat(reasoning_effort="high",max_tokens=20000)
            svc=self.start([loop,"Verified.\n</think>\nRecovered solution."],args=["--max-context","32768"],env={"STRATA_TEST_CONTEXT":"32768"},reasoning_loop_recovery=recovery)
            status,result=svc.request("POST","/v1/chat/completions",req);self.assertEqual(status,200,result)
            if recovery=="stop":self.assertEqual(result["choices"][0]["finish_reason"],"length")
            else:self.assertEqual(result["choices"][0]["message"]["content"],"Recovered solution.");self.assertEqual(result["choices"][0]["finish_reason"],"stop")
            # Native/Python use the same unchanged task and the same recovery mode.
            ref=self.start([loop,"Verified.\n</think>\nRecovered solution."],native=False,args=["--max-context","32768"],env={"STRATA_TEST_CONTEXT":"32768"},reasoning_loop_recovery=recovery)
            status,expected=ref.request("POST","/v1/chat/completions",req);self.assertEqual(status,200,expected);self.assertEqual(result["choices"],expected["choices"]);self.assertEqual(result["usage"],expected["usage"])
    def test_responses_null_fields_and_model_fallback(self):
        a,b=self.start(),self.start(native=False)
        req={"input":"hi","model":"client-placeholder","max_output_tokens":100,"reasoning":None,"text":None,"tools":None,"metadata":None}
        results=[svc.request("POST","/v1/responses",req)[1] for svc in (a,b)]
        self.assertEqual(results[0]["model"],"test-native")
        for x in results:
            x.pop("id",None);x.pop("created_at",None);x.pop("completed_at",None)
            for item in x["output"]:item.pop("id",None)
        self.assertEqual(results[0],results[1])
    def test_mcp_limit_truncation_and_cancel(self):
        fixture=ARGS.engine.resolve().with_name("strata-mcp-test-server.exe" if os.name=="nt" else "strata-mcp-test-server")
        raw="<tool_call>\n<function=test__echo>\n<parameter=text>hello world 中文</parameter>\n</function>\n</tool_call>"
        svc=self.start([raw,raw],mcp_servers={"test":{"command":str(fixture)}},mcp={"max_rounds":1,"max_result_chars":5})
        status,result=svc.request("POST","/v1/chat/completions",self.chat(strata_mcp=True,max_tokens=500));self.assertEqual(status,200,result)
        activity=result["choices"][0]["message"]["strata_mcp"];first=next(e for e in activity if e["event"]=="result" and not e.get("skipped"));self.assertTrue(first["truncated"]);self.assertTrue(first["text"].startswith("hello\n\n[... truncated"));self.assertTrue(any(e["event"]=="limit" for e in activity))
        slow="<tool_call>\n<function=test__slow>\n</function>\n</tool_call>"
        other=self.start([slow,"after cancel"],mcp_servers={"test":{"command":str(fixture)}})
        conn=http.client.HTTPConnection("127.0.0.1",other.port,timeout=10);conn.request("POST","/v1/chat/completions",json.dumps(self.chat(strata_mcp=True,stream=True,max_tokens=500)),{"Authorization":"Bearer "+KEY,"Content-Type":"application/json"});stream=conn.getresponse()
        while True:
            line=stream.readline()
            if not line:self.fail("MCP stream ended before call event")
            if line.startswith(b'data: {') and json.loads(line[6:]).get("strata_mcp",{}).get("event")=="call":break
        stream.close();conn.close();start=time.monotonic();self.assertEqual(other.request("POST","/v1/chat/completions",self.chat())[0],200);self.assertLess(time.monotonic()-start,5)
    def test_unicode_api_key_latin1_and_utf8(self):
        svc=self.start(api_key=[" café ","another-key"])
        for key in ("café","café".encode().decode('latin1'),"another-key"):
            self.assertEqual(svc.request("GET","/v1/models",headers={"Authorization":"bEaReR "+key})[0],200)
        self.assertEqual(svc.request("GET","/v1/models",headers={"Authorization":"Bearer wrong"})[0],401)
    def test_failure_accounting_monitor_and_prometheus(self):
        svc=self.start(["thoughts without finishing", "CRASH"])
        req=self.chat(reasoning_effort="high",reasoning_budget_tokens=4,max_tokens=300)
        status,result=svc.request("POST","/v1/chat/completions",req);self.assertEqual(status,503,result)
        metrics=svc.request("GET","/metrics")[1];self.assertEqual(metrics["totals"]["requests"],1);self.assertGreater(metrics["totals"]["decode_ms"],0);self.assertEqual(metrics["requests"][0]["finish"],"error")
        records=svc.request("GET","/api/requests")[1]["requests"];self.assertEqual(records[0]["state"],"error");self.assertNotIn("input",records[0]);detail=svc.request("GET","/api/requests?id="+records[0]["id"])[1];self.assertIn("input",detail)
    def test_preparation_errors_are_retained_without_generation(self):
        svc=self.start()
        for change in ({"messages":[{"role":"user","content":123}]}, {"response_format":{"type":"json_schema","json_schema":{"schema":{"type":"imaginary"}}}}, {"max_tokens":99999}):
            status,result=svc.request("POST","/v1/chat/completions",self.chat(**change));self.assertEqual(status,400,result)
        records=svc.request("GET","/api/requests")[1]["requests"];self.assertEqual(len(records),3)
        for record in records:
            self.assertEqual(record["state"],"error");self.assertEqual(record["http_status"],400)
            detail=svc.request("GET","/api/requests?id="+record["id"])[1];self.assertIn("input",detail);self.assertIn("error",detail)
        self.assertEqual(svc.request("GET","/metrics")[1]["totals"]["requests"],0)
        self.assertEqual(svc.request("POST","/v1/chat/completions",self.chat())[0],200)
    def test_session_errors_have_numeric_codes_and_restart(self):
        disabled=self.start()
        status,error=disabled.request("POST","/slots/0?action=save",{"filename":"ok"});self.assertEqual(status,501);self.assertEqual(error["error"]["code"],501)
        svc=self.start(slot_save_path=str(self.folder/"sessions"))
        for slot,action,name,expected in (("1","save","ok",400),("0","save","../outside",400),("0","restore","absent",404)):
            status,error=svc.request("POST",f"/slots/{slot}?action={action}",{"filename":name});self.assertEqual(status,expected,error);self.assertEqual(error["error"]["code"],expected)
        self.assertEqual(svc.request("POST","/unload",{})[0],200)
        status,error=svc.request("POST","/slots/0?action=save",{"filename":"ok"});self.assertEqual(status,503,error);self.assertEqual(error["error"]["code"],503)
        self.assertEqual(svc.request("POST","/v1/chat/completions",self.chat())[0],200)
    def test_serialized_messages_python_parity(self):
        request=self.chat(messages=json.dumps([{"role":"user","content":"Hi"}]))
        native=self.start();python=self.start(native=False)
        ns,n=native.request("POST","/v1/chat/completions",request);ps,p=python.request("POST","/v1/chat/completions",request)
        self.assertEqual(ns,200,n);self.assertEqual(ps,200,p);self.assertEqual(n["choices"],p["choices"]);self.assertEqual(n["usage"],p["usage"])
    def test_codex_compaction_reuses_only_owner_tools(self):
        svc=self.start("ok",codex_compaction_cache=True)
        tool={"type":"function","name":"echo","parameters":{"type":"object","properties":{"text":{"type":"string"}}}}
        meta=lambda kind,thread:{"x-codex-turn-metadata":json.dumps({"request_kind":kind,"session_id":"session","thread_id":thread})}
        base={"model":"test-native","input":"hi","reasoning":{"effort":"none"},"max_output_tokens":20,"client_metadata":meta("normal","one"),"tools":[tool]}
        self.assertEqual(svc.request("POST","/v1/responses",base)[0],200)
        self.assertEqual(svc.request("POST","/v1/responses",{**base,"tools":[],"client_metadata":meta("compaction","one")})[0],200)
        self.assertEqual(svc.request("POST","/v1/responses",{**base,"tools":[],"client_metadata":meta("compaction","two")})[0],200)
        commands=(self.folder/"captured.txt").read_text().splitlines();prompts=[self.tok.decode(list(map(int,line.split()[-1].split(',')))) for line in commands]
        self.assertEqual(prompts[0],prompts[1]);self.assertNotEqual(prompts[1],prompts[2])
    def test_reasoning_budget_continuation_python_parity(self):
        scripts=["thinking for quite a long time without ending", "the answer"]
        a,b=self.start(scripts),self.start(scripts,native=False)
        req=self.chat(reasoning_effort="high",reasoning_budget_tokens=4,max_tokens=300)
        outputs=[svc.request("POST","/v1/chat/completions",req)[1] for svc in (a,b)]
        self.assertEqual(outputs[0]["choices"],outputs[1]["choices"])
        self.assertEqual(outputs[0]["usage"],outputs[1]["usage"])
    def test_forced_tool_after_thinking_python_parity(self):
        scripts=["thinking</think>\n\n", "<parameter=text>hello</parameter>\n</function>\n</tool_call>"]
        a,b=self.start(scripts),self.start(scripts,native=False)
        req=self.chat(reasoning_effort="high",max_tokens=300,tools=[{"type":"function","function":{"name":"echo","parameters":{"type":"object","properties":{"text":{"type":"string"}}}}}],tool_choice={"type":"function","function":{"name":"echo"}})
        outputs=[svc.request("POST","/v1/chat/completions",req)[1] for svc in (a,b)]
        for x in outputs:
            for c in x["choices"][0]["message"].get("tool_calls",[]):c["id"]="CALL"
        self.assertEqual(outputs[0]["choices"],outputs[1]["choices"])
        self.assertEqual(outputs[0]["usage"],outputs[1]["usage"])
    def test_native_launcher_to_api_without_python_on_path(self):
        launcher=ARGS.exe.resolve()
        if not launcher.is_file():self.skipTest("native cluster launcher not built in this host-only configuration")
        number=port()
        source=self.folder/"coordinator.json"
        source.write_text(json.dumps({"exe":str(ARGS.engine.resolve()),"args":["--native","unused.gguf"],"cwd":str(ROOT),
            "tokenizer":str(self.folder/"tokenizer"),"web_root":str(ROOT/"serve/web"),"model_name":"launcher-model","api_key":KEY,"port":number,"open_browser":False,
            "env":{"STRATA_TEST_TOKENS":",".join(map(str,self.tok.encode("native chain")+[self.tok.ids["<|im_end|>"]]))}}),encoding="utf-8")
        plan=json.loads((ROOT/"configs/cluster-2hosts.json").read_text(encoding="utf-8"))
        node=next(n for n in plan["nodes"] if n["role"]=="coordinator")
        node.update(server_config=str(source),engine=str(ARGS.engine.resolve()),context=4096,prefill=512)
        shared=self.folder/"cluster.json";shared.write_text(json.dumps(plan),encoding="utf-8")
        env=dict(os.environ,STRATA_NETWORK_KEY=KEY)
        env.pop("STRATA_API_KEY",None)
        env["PATH"]=str(Path(env.get("SYSTEMROOT","C:/Windows"))/"System32")
        with (self.folder/"chain.log").open("w",encoding="utf-8") as log:
            proc=subprocess.Popen([str(launcher),"cluster","--config",str(shared),"--node",node["id"]],cwd=self.folder,env=env,stdout=log,stderr=subprocess.STDOUT)
            try:
                end=time.monotonic()+20
                while True:
                    if proc.poll() is not None:raise AssertionError((self.folder/"chain.log").read_text(encoding="utf-8"))
                    try:
                        conn=http.client.HTTPConnection("127.0.0.1",number,timeout=2)
                        conn.request("GET","/health");res=conn.getresponse();health=json.loads(res.read());conn.close()
                        if health.get("loaded"):break
                    except OSError:pass
                    if time.monotonic()>end:raise AssertionError("launcher API timeout")
                    time.sleep(.05)
                conn=http.client.HTTPConnection("127.0.0.1",number,timeout=10)
                conn.request("POST","/v1/chat/completions",json.dumps({"messages":[{"role":"user","content":"hello"}],"reasoning_effort":"none","max_tokens":100}),{"Authorization":"Bearer "+KEY,"Content-Type":"application/json"})
                res=conn.getresponse();result=json.loads(res.read());conn.close()
                self.assertEqual(res.status,200,result)
                self.assertEqual(result["choices"][0]["message"]["content"],"native chain")
                self.assertEqual(json.loads(source.read_text(encoding="utf-8"))["args"],["--native","unused.gguf"])
            finally:proc.terminate();proc.wait(timeout=10)

if __name__ == "__main__":
    suite = unittest.defaultTestLoader.loadTestsFromTestCase(ServerTests) if not ARGS.only else unittest.TestSuite(ServerTests(name) for name in ARGS.only.split(','))
    result = unittest.TextTestRunner(verbosity=2).run(suite)
    if ARGS.report:
        ARGS.report.write_text(json.dumps({"tests":result.testsRun,"failures":len(result.failures),"errors":len(result.errors),"skipped":len(result.skipped),**COUNTS},indent=2),encoding="utf-8")
    raise SystemExit(not result.wasSuccessful())
