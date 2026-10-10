"""Opt-in same-machine API correctness comparison against the real resident engine.

Starts isolated loopback servers sequentially; never stops an installed service or edits its config.
No speed conclusion is drawn from this check. Configs with network workers retain their own authentication env.
"""
from __future__ import annotations
import argparse
import http.client
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import time

ROOT = Path(__file__).resolve().parents[1]
sys.path[:0] = [str(ROOT / "tmp/native-text-test-deps"), str(ROOT), str(ROOT / "tools")]

def request(port, path, body=None):
    conn = http.client.HTTPConnection("127.0.0.1", port, timeout=600)
    conn.request("POST" if body is not None else "GET", path,
                 None if body is None else json.dumps(body, ensure_ascii=False).encode(),
                 {"Content-Type":"application/json"})
    res = conn.getresponse()
    raw = res.read().decode("utf-8")
    conn.close()
    if res.status != 200: raise AssertionError((res.status, raw[:2000]))
    return json.loads(raw)

def normalize(result):
    if "choices" in result:
        msg = result["choices"][0]["message"]
        for call in msg.get("tool_calls", []):
            call["id"] = "CALL"
            call["function"]["arguments"] = json.loads(call["function"]["arguments"])
        return {"choices":result["choices"], "usage":result["usage"]}
    for item in result.get("output", []): item.pop("id", None)
    result.pop("id", None)
    result.pop("created_at", None)
    result.pop("completed_at", None)
    return result

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--exe", type=Path, required=True)
    ap.add_argument("--config", type=Path, required=True)
    ap.add_argument("--port", type=int, default=18085)
    ap.add_argument("--report", type=Path, required=True)
    a = ap.parse_args()
    source = json.loads(a.config.read_text(encoding="utf-8-sig"))
    report = {"config":str(a.config), "runs":{}, "performance_claim":False}
    args = list(source["args"])
    for flag, value in (("--max-context", "8192"), ("--prefill", "512")):
        while flag in args:
            i = args.index(flag); del args[i:i+2]
        args += [flag, value]
    model = source.get("model_name", "strata")
    base = {"model":model, "reasoning_effort":"none", "max_tokens":32, "temperature":0}
    user = lambda s: {"role":"user", "content":s}
    checks = [
        ("short", "/v1/chat/completions", {**base,"messages":[user("Reply with exactly: Hello world")]}),
        ("long", "/v1/chat/completions", {**base,"messages":[user("Context notes:\n" + "The sky is blue and grass is green.\n" * 200 + "\nWhat color is the sky? Answer in five words or fewer.")]}),
        ("turn1", "/v1/chat/completions", {**base,"messages":[user("My code word is violet. Remember it. Reply OK.")]}),
        ("tools", "/v1/chat/completions", {**base,"max_tokens":128,"messages":[user("Call the echo tool with text set to hello. Do not answer in text.")],
            "tools":[{"type":"function","function":{"name":"echo","description":"Echo text","parameters":{"type":"object","properties":{"text":{"type":"string"}},"required":["text"]}}}]}),
        ("responses", "/v1/responses", {"model":model,"input":"Reply with exactly: Hello world", "reasoning":{"effort":"none"},"max_output_tokens":32,"temperature":0,"store":False}),
        ("anthropic", "/v1/messages", {"model":model,"messages":[user("Reply with exactly: Hello world")],"thinking":{"type":"disabled"},"max_tokens":32,"temperature":0}),
    ]
    with tempfile.TemporaryDirectory(prefix="native-real-", dir=ROOT / "tmp") as tmp:
        folder = Path(tmp)
        for native in (True, False):
            label = "native" if native else "python"
            cfg = {**source,"args":args,"host":"127.0.0.1","port":a.port,"open_browser":False,"log":str(folder/(label+"-engine.log"))}
            cfg.pop("api_key",None)
            path=folder/(label+".json");path.write_text(json.dumps(cfg),encoding="utf-8")
            cmd=[str(a.exe.resolve())] if native else [sys.executable,"-m","serve.server"]
            env=dict(os.environ);env.pop("STRATA_API_KEY",None);env["PYTHONPATH"]=os.pathsep.join(sys.path)
            with (folder/(label+"-server.log")).open("w",encoding="utf-8") as log:
                process=subprocess.Popen(cmd+["--engine","strata","--config",str(path),"--port",str(a.port)],cwd=ROOT,env=env,stdout=log,stderr=subprocess.STDOUT)
                try:
                    ready=False;deadline=time.monotonic()+900
                    while time.monotonic()<deadline:
                        if process.poll() is not None: raise AssertionError((folder/(label+"-server.log")).read_text(encoding="utf-8")[-6000:])
                        try: ready=request(a.port,"/health").get("loaded",False)
                        except OSError: pass
                        if ready: break
                        time.sleep(.5)
                    if not ready: raise AssertionError("real engine READY timeout")
                    print(label+": ready",flush=True)
                    results={}
                    for name,route,body in checks:
                        results[name]=normalize(request(a.port,route,body))
                        print(label+": "+name+" complete",flush=True)
                        if name=="turn1":
                            reply=results[name]["choices"][0]["message"]
                            results["turn2"]=normalize(request(a.port,route,{**base,"messages":[body["messages"][0],reply,user("What is my code word? Reply with just the word.")]}))
                    # A real decode is stopped by closing its SSE client. A fresh request
                    # then proves STOP was drained instead of delivering stale tokens.
                    conn=http.client.HTTPConnection("127.0.0.1",a.port,timeout=600)
                    conn.request("POST","/v1/chat/completions",json.dumps({**base,"max_tokens":128,"stream":True,"messages":[user("List the numbers from 1 to 100, one per line.")]}),{"Content-Type":"application/json"})
                    stream=conn.getresponse()
                    while True:
                        line=stream.readline().decode("utf-8")
                        if not line: raise AssertionError("real SSE ended before a content delta")
                        if line.startswith("data: {"):
                            delta=json.loads(line[6:])["choices"][0]["delta"]
                            if delta.get("content"):break
                    stream.close();conn.close()
                    fresh=request(a.port,"/v1/chat/completions",{**base,"messages":[user("Reply with exactly: fresh")]})
                    results["disconnect"]={"next_request_completed":fresh["choices"][0]["message"]["content"].strip()=="fresh"}
                    if not results["disconnect"]["next_request_completed"]:raise AssertionError(fresh)
                    report["runs"][label]=results
                    a.report.write_text(json.dumps(report,ensure_ascii=False,indent=2),encoding="utf-8")
                finally:
                    process.terminate();process.wait(timeout=20)
                    time.sleep(1) # Windows job teardown closes the child's engine log asynchronously.
        report["equal"]={name: report["runs"]["native"][name]==report["runs"]["python"][name] for name in report["runs"]["native"]}
        a.report.write_text(json.dumps(report,ensure_ascii=False,indent=2),encoding="utf-8")
        print(json.dumps(report["equal"],indent=2))
        if not all(report["equal"].values()): raise SystemExit(1)

if __name__=="__main__":main()
