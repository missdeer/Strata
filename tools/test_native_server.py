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

ROOT = Path(__file__).resolve().parents[1]
sys.path[:0] = [str(ROOT / "tmp/native-text-test-deps"), str(ROOT), str(ROOT / "tools")]
import strata_tokenizer as ST
from serve.test_detok import synthetic_tokenizer

parser = argparse.ArgumentParser()
parser.add_argument("--exe", type=Path, required=True)
parser.add_argument("--engine", type=Path, required=True)
parser.add_argument("--report", type=Path)
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
        self.config = folder / ("native.json" if native else "python.json")
        self.config.write_text(json.dumps(cfg), encoding="utf-8")
        args = [str(ARGS.exe.resolve())] if native else [sys.executable, "-m", "serve.server"]
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
                raise AssertionError(self.config.with_name(self.log.name).read_text(encoding="utf-8"))
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

class ServerTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix="native-server-中文-", dir=ROOT / "tmp")
        self.folder = Path(self.temp.name)
        self.tok = synthetic_tokenizer(n_merges=200)
        # Both stop controls must exist: Service.stop_ids encodes both literals.
        self.tok = ST.Tokenizer(self.tok.tokens + ["<|endoftext|>"],
                                [" ".join(pair) for pair in self.tok.ranks], self.tok.token_types + [3])
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
        cfg = {"exe": str(ARGS.engine.resolve()), "args": ["--max-context", "4096"], "cwd": str(ROOT),
               "tokenizer": str(self.folder / "tokenizer"), "model_name": "test-native", "api_key": KEY,
               "host": "127.0.0.1", "open_browser": False, "api_monitor": True,
               "env": {"STRATA_TEST_TOKENS": tokens, "STRATA_TEST_CAPTURE": str(self.folder / "captured.txt")},
               **extra}
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
        self.assertEqual(svc.request("GET", "/", headers={"Host":"evil.example"})[0], 403)
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
                time.sleep(.04)
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
        for field in ("mcp_servers", "vision", "before_load", "slot_save_path", "idle_unload_s", "reasoning_loop_recovery"):
            cfg={"exe":str(ARGS.engine.resolve()), "args":[], "tokenizer":str(self.folder/"tokenizer"), field:{"enabled":True}}
            path=self.folder/"bad.json";path.write_text(json.dumps(cfg),encoding="utf-8")
            p=subprocess.run([str(ARGS.exe.resolve()),"--config",str(path),"--validate"],capture_output=True,text=True)
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
        launcher=ARGS.exe.resolve().with_name("strata-cluster.exe" if os.name=="nt" else "strata-cluster")
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
            proc=subprocess.Popen([str(launcher),"--config",str(shared),"--node",node["id"]],cwd=self.folder,env=env,stdout=log,stderr=subprocess.STDOUT)
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
    suite = unittest.defaultTestLoader.loadTestsFromTestCase(ServerTests)
    result = unittest.TextTestRunner(verbosity=2).run(suite)
    if ARGS.report:
        ARGS.report.write_text(json.dumps({"tests":result.testsRun,"failures":len(result.failures),"errors":len(result.errors),"skipped":len(result.skipped),**COUNTS},indent=2),encoding="utf-8")
    raise SystemExit(not result.wasSuccessful())
