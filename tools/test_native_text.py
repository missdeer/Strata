"""Stage 3 differential tests; Python is the oracle, never a native runtime dependency.

python tools/test_native_text.py --exe build-network/strata-text-check.exe \
    --tokenizer D:/Projects/Strata-data/packs/iq3_s/tokenizer
"""
from __future__ import annotations
import argparse
import codecs
import importlib
import io
import json
import os
from pathlib import Path
import random
import subprocess
import sys
import tempfile
import unittest
from unittest import mock

ROOT = Path(__file__).resolve().parents[1]
sys.path[:0] = [str(ROOT / "tmp/native-text-test-deps"), str(ROOT), str(ROOT / "tools")]
import strata_tokenizer as ST
from serve import frontend as F, structured as S
from serve.server import StopMatcher, stop_strings
from serve.test_detok import random_text, synthetic_tokenizer

AP = argparse.ArgumentParser()
AP.add_argument("--exe", type=Path, required=True)
AP.add_argument("--tokenizer", type=Path)
AP.add_argument("--report", type=Path)
ARGS = AP.parse_args()
COUNTS = {}

class Native:
    def __init__(self, tokenizer=None, template=ROOT / "serve/chat_template.jinja", no_python=False):
        cmd = [str(ARGS.exe.resolve())]
        if tokenizer: cmd += ["--tokenizer", str(tokenizer)]
        if template: cmd += ["--template", str(template)]
        env = dict(os.environ)
        if no_python: env["PATH"] = str(Path(env.get("SYSTEMROOT", "C:/Windows")) / "System32")
        self.p = subprocess.Popen(cmd, stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                                  text=True, encoding="utf-8", env=env)
    def raw(self, req):
        COUNTS[req["op"]] = COUNTS.get(req["op"], 0) + 1
        try:
            self.p.stdin.write(json.dumps(req, ensure_ascii=False) + "\n")
            self.p.stdin.flush()
        except OSError as error:
            raise AssertionError(f"native exited {self.p.poll()}: {self.p.stderr.read()}") from error
        line = self.p.stdout.readline()
        if not line: raise AssertionError(f"native exited {self.p.poll()}: {self.p.stderr.read()}")
        return json.loads(line)
    def ask(self, req):
        out = self.raw(req)
        if "error" in out: raise ValueError(out["error"])
        return out["result"]
    def close(self):
        self.p.stdin.close()
        self.p.wait(timeout=20)
        err = self.p.stderr.read()
        self.p.stdout.close(); self.p.stderr.close()
        if self.p.returncode: raise AssertionError(err)

def save_tokenizer(tok, path):
    path.mkdir(parents=True, exist_ok=True)
    (path / "vocab.json").write_text(json.dumps(tok.ids, ensure_ascii=False), encoding="utf-8")
    (path / "merges.txt").write_text("\n".join(" ".join(k) for k in tok.ranks), encoding="utf-8")
    (path / "token_type.json").write_text(json.dumps(tok.token_types), encoding="utf-8")
    (path / "tokenizer.json").write_text(json.dumps({"model":"gpt2", "pre":"qwen35", "pre_pattern":ST.QWEN35_PATTERN,
        "vocab_size":len(tok.tokens), "n_merges":len(tok.ranks), "special_ids":tok.special_ids}), encoding="utf-8")

def load_tokenizer(path):
    vocab = json.loads((path / "vocab.json").read_text(encoding="utf-8"))
    tokens = [None] * len(vocab)
    for token, i in vocab.items(): tokens[i] = token
    return ST.Tokenizer(tokens, (path / "merges.txt").read_text(encoding="utf-8").splitlines(),
                        json.loads((path / "token_type.json").read_text(encoding="utf-8")))

def normalized_events(events):
    # IDs are fresh per request; preserve their relationships across start/args/final.
    ids = {}
    result = []
    for e in events:
        call = e.call
        result.append({"kind":e.kind, "text":e.text, "call":None if call is None else {
            "name":call.name, "arguments":call.arguments, "id":ids.setdefault(call.id,len(ids))}})
    return result

def normalized_native(events):
    ids = {}
    for e in events:
        if e["call"] is not None:
            c = e["call"]; c["id"] = ids.setdefault(c["id"],len(ids))
    return events

class Parity(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.TemporaryDirectory(prefix="strata-native-text-", dir=ROOT / "tmp")
        cls.path = Path(cls.tmp.name) / "分词器"
        cls.tok = synthetic_tokenizer()
        save_tokenizer(cls.tok,cls.path)
        cls.native = Native(cls.path)
        cls.template = F.ChatTemplate(ROOT / "serve/chat_template.jinja")
    @classmethod
    def tearDownClass(cls):
        cls.native.close(); cls.tmp.cleanup()

    def tokenizer_corpus(self, tok, native, count):
        rng = random.Random(268)
        fixed = ["", "e\u0301_j", "\x00\x01\x7f", "\u00a0\u2028\u2029", "你好" * 4096,
                 "x" * 5000, "é\u0301" * 600, "<|im_end|><think></think><tool_call>", "I'm I'LL we've I'd ISN'T"]
        fixed += ["".join(chr(i) for i in range(0,256)), "a\n\n  b\t\r\n", " \u0301\u0301", "☀️👩‍💻"]
        for i, text in enumerate(fixed + [random_text(rng,12) for _ in range(count)]):
            for ps in (False,True):
                expected = tok.encode(text,parse_special=ps)
                self.assertEqual(native.ask({"op":"encode","text":text,"parse_special":ps}), expected, (i,ps,text[:50]))
            if i < 60:
                self.assertEqual(native.ask({"op":"decode","ids":expected}), tok.decode(expected))
        text = "你好<think>字面</think> and <|im_end|>"
        span = (2, text.index(" and"))
        byte_span = [len(text[:p].encode()) for p in span]
        self.assertEqual(native.ask({"op":"encode","text":text,"parse_special":True,"plain":[byte_span]}),
                         tok.encode(text,True,plain=[span]))

    def test_synthetic_full_token_ids(self): self.tokenizer_corpus(self.tok,self.native,160)

    @unittest.skipUnless(ARGS.tokenizer, "pass --tokenizer for real vocabulary parity")
    def test_real_full_token_ids(self):
        tok = load_tokenizer(ARGS.tokenizer); native = Native(ARGS.tokenizer)
        try: self.tokenizer_corpus(tok,native,160)
        finally: native.close()

    def test_golden_templates(self):
        for case in json.loads((ROOT / "serve/chat_golden.json").read_text(encoding="utf-8")):
            kw = {k:v for k,v in case.items() if k not in ("name","messages","tools","rendered","error","hf","add_generation_prompt")}
            request = {"op":"render","messages":case["messages"],"tools":case.get("tools"),"kwargs":kw,
                       "generation_prompt":case.get("add_generation_prompt",True)}
            got = self.native.ask(request)
            self.assertEqual(got,case["rendered"],case["name"])
        for effort in (False, "low", "medium", "high"):
            kw = {"enable_thinking":False} if effort is False else {"reasoning_effort":effort}
            messages = [{"role":"user","content":"问题\n\t"},{"role":"assistant","content":""}, {"role":"user","content":"Again"}]
            self.assertEqual(self.native.ask({"op":"render","messages":messages,"kwargs":kw}),self.template.render(messages,**kw))
        self.assertEqual(self.native.ask({"op":"capabilities"}),self.template.caps)

    def test_other_model_templates(self):
        candidates = [ROOT / "third_party/llama.cpp/models/templates/Qwen3-Coder.jinja",
                      ROOT / "third_party/llama.cpp/models/templates/Qwen3.5-4B.jinja"]
        if ARGS.tokenizer and (ARGS.tokenizer / "chat_template.jinja").exists(): candidates.append(ARGS.tokenizer / "chat_template.jinja")
        for path in candidates:
            native = Native(template=path); reference = F.ChatTemplate(path)
            try:
                for enabled in (False,True):
                    messages = [{"role":"user","content":"你好"}]
                    self.assertEqual(native.ask({"op":"render","messages":messages,"kwargs":{"enable_thinking":enabled}}),reference.render(messages,enable_thinking=enabled),str(path))
            finally: native.close()

    def test_normalization(self):
        cases = [
          ("openai", {"messages":[{"role":"developer","content":"sys"},{"role":"user","content":None},
              {"role":"system","content":"late"},{"role":"assistant","content":"ok", "reasoning_content":"r",
               "tool_calls":[{"function":{"name":"run","arguments":'{"z":1,"a":"é"}'}}]}],"reasoning_effort":"high"}),
          ("openai", {"messages":json.dumps([{"role":"user","content":[{"type":"text","text":"hi"},{"type":"image_url","image_url":{"url":"test.png"}}]}]),
              "tools":[{"type":"function","function":{"name":"run","parameters":{}}}],"chat_template_kwargs":{"reasoning_effort":"low","enable_thinking":False}}),
          ("anthropic", {"system":"x-anthropic-billing-header: cc_version=2.1.170.bf4; cc_entrypoint=sdk-cli; cch=b145e;",
              "messages":[{"role":"assistant","content":[{"type":"thinking","thinking":"r"},{"type":"tool_use","name":"run","input":{"x":3}}]},
                          {"role":"user","content":[{"type":"tool_result","content":[{"type":"image","source":{"type":"base64","data":"xxx"}}]}, {"type":"text","text":"ok"}]}],
              "tools":[{"name":"run","input_schema":{"type":"object"}}],"thinking":{"type":"enabled","budget_tokens":2048}})]
        cases.append(("anthropic", {"messages":[{"role":"user","content":[{"type":"image","source":{"type":"url","url":"a.png"}},
            {"type":"thinking","thinking":"ignored"},{"type":"text","text":"what"}]}]}))
        for budget in (2047,2048,8191,8192,1e30,2047.9," 2048 ","2048x"):
            cases.append(("anthropic",{"messages":[{"role":"user","content":"hi"}],"thinking":{"budget_tokens":budget}}))
        for api, req in cases:
            messages, tools, kwargs = (F.openai_to_messages if api == "openai" else F.anthropic_to_messages)(req)
            self.assertEqual(self.native.ask({"op":"normalize","api":api,"request":req}),
                             {"messages":messages,"tools":tools,"kwargs":kwargs,"images":F.images_of(messages)})
        for raw in (None,"",'{"x":1}',"bad", "[]", [1,2],False):
            req = {"messages":[{"role":"assistant","tool_calls":[{"function":{"name":"run","arguments":raw}}]}]}
            m,t,k = F.openai_to_messages(req)
            self.assertEqual(self.native.ask({"op":"normalize","request":req})["messages"],m)
        for req in ({"messages":{}},{"tools":"auto"},{"tools":[{"name":"x","parameters":[]}]}, {"reasoning_effort":"bad"}):
            self.assertIn("error",self.native.raw({"op":"normalize","request":req}))

    def test_literal_tags(self):
        for role in ("user","assistant","tool"):
            messages = [{"role":"user","content":"你好"}, {"role":role,"content":"<think>quoted</think> <|im_end|> <|vision_start|>"}]
            if role == "assistant": messages[1]["content"] = "<think>r</think>quoted </think> <|im_end|>"
            tags = F.literal_tags(self.tok.control_tokens)
            m,t,_ = F.mark_think_literals(messages,None,tags)
            prompt, spans = F.unmark_think_literals(self.template.render(m,tools=t),tags)
            want_spans = [[len(prompt[:a].encode()),len(prompt[:b].encode())] for a,b in spans]
            got = self.native.ask({"op":"prompt","messages":messages})
            self.assertEqual(got["text"],prompt)
            self.assertEqual(got["plain"],want_spans)
            self.assertEqual(got["ids"],self.tok.encode(prompt,True,plain=spans))

    def test_utf8_incremental_and_invalid(self):
        rng = random.Random(100)
        streams = [bytes(range(256)), "你好🌊👩‍💻".encode(),b"\xf0\x80\x80\x80\xed\xa0\x80\xe2\x82",b"\xe2x\xc0\x80"]
        streams += [bytes(rng.randrange(256) for _ in range(200)) for _ in range(30)]
        for stream in streams:
            for width in (1,2,7,9999):
                chunks = [stream[p:p+width] for p in range(0,len(stream),width)]
                decoder = codecs.getincrementaldecoder("utf-8")("replace")
                want = [decoder.decode(c) for c in chunks] + [decoder.decode(b"",final=True)]
                self.assertEqual(self.native.ask({"op":"utf8","chunks":[list(c) for c in chunks],"final":True}),want,(stream.hex(),width))
        ids = self.tok.encode("你好 🌊 é")
        decoder = codecs.getincrementaldecoder("utf-8")("replace")
        self.assertEqual(self.native.ask({"op":"detokenize","ids":ids}),[decoder.decode(self.tok.token_bytes(i)) for i in ids])

    def test_stop_boundaries(self):
        for stops in (["END"],["END","EN"],["STOP","END"],["世末"],[]):
            for text in ("alpha END beta STOP gamma","alpha EN","你世末好", "nothing"):
                for width in (1,2,7,999):
                    chunks = [text[p:p+width] for p in range(0,len(text),width)]
                    matcher = StopMatcher(stops); want = [matcher.push(c) for c in chunks]
                    self.assertEqual(self.native.ask({"op":"stop","request":{"stop_sequences":stops},"chunks":chunks}),
                                     {"chunks":want,"tail":matcher.flush(),"hit":matcher.hit})
        for value in (4,{},[None], ["x"]*5): self.assertIn("error",self.native.raw({"op":"stop","request":{"stop":value},"chunks":[]}))

    def test_reasoning_specimens(self):
        specimens = json.loads((ROOT / "serve/fixtures/rcall_specimens.json").read_text(encoding="utf-8"))["specimens"]
        for case in specimens:
            tools = [{"name":n} for n in case["tools"]]
            for width in (1,3,17,100000):
                for streaming in (False,True):
                    chunks = [case["input"][p:p+width] for p in range(0,len(case["input"]),width)]
                    parser = F.OutputParser(tools=tools,stream_tools=streaming)
                    events = []
                    for chunk in chunks: events += parser.feed(chunk)
                    events += parser.finish(case["finish"])
                    got = self.native.ask({"op":"parse","tools":tools,"stream_tools":streaming,"chunks":chunks,"finish":case["finish"]})
                    self.assertEqual(normalized_native(got["events"]),normalized_events(events),(case["name"],width,streaming))
                    self.assertEqual((got["rescued"],got["refused"]),(parser.rescued,parser.refused))

    def test_existing_parser_regressions(self):
        original = F.OutputParser
        owner = self
        class CheckedParser(original):
            def __init__(self, thinking=True, tools=None, stream_tools=False, recover=False):
                super().__init__(thinking,tools,stream_tools,recover)
                self.check_options = {"thinking":thinking,"tools":tools,"stream_tools":stream_tools,"recover":recover}
                self.check_chunks, self.check_events = [], []
            def feed(self, delta):
                events = super().feed(delta)
                self.check_chunks.append(delta); self.check_events.extend(events)
                return events
            def finish(self, reason=None):
                events = super().finish(reason); self.check_events.extend(events)
                # One legacy test deliberately changes Python's holding budget.
                # The production default is compared by every other case.
                if F.RCALL_MAX == 32768:
                    got = owner.native.ask({"op":"parse",**self.check_options,"chunks":self.check_chunks,"finish":reason or "stop"})
                    owner.assertEqual(normalized_native(got["events"]),normalized_events(self.check_events))
                    owner.assertEqual((got["rescued"],got["refused"]),(self.rescued,self.refused))
                return events
        selected = {
            "serve.test_server":["ToolCallTerminators","ToolCallTagInProse","ToolCallRecovery","ToolCallRecoveryOff"],
            "serve.test_reasoning_tools":["Parser"],
            "serve.test_reasoning_rescue":["Gates","VisibleAnswer"],
            "serve.test_tool_name_space":["SpacedName"],
        }
        modules = [importlib.import_module(name) for name in selected]
        patches = [mock.patch.object(F,"OutputParser",CheckedParser)]
        for module in modules:
            if hasattr(module,"OutputParser"): patches.append(mock.patch.object(module,"OutputParser",CheckedParser))
        for patch in patches: patch.start()
        try:
            suite = unittest.TestSuite(unittest.defaultTestLoader.loadTestsFromTestCase(getattr(module,cls))
                for module in modules for cls in selected[module.__name__])
            sink = io.StringIO(); result = unittest.TextTestRunner(stream=sink).run(suite)
            self.assertTrue(result.wasSuccessful(),sink.getvalue())
            COUNTS["existing_parser_tests"] = result.testsRun
        finally:
            for patch in reversed(patches): patch.stop()

    def test_structured_formats(self):
        if S.jsonschema_modules() is None:
            self.skipTest("install jsonschema for full schema parity")
        from serve.test_structured import FORMAT, STORY
        formats = [{"type":"json_object"},FORMAT]
        claim = {"type":"object","properties":{"claim_text":{"type":"string"}},"required":["claim_text"],"additionalProperties":False}
        for schema in ({"anyOf":[claim,{"type":"object","maxProperties":0}]}, {"oneOf":[claim,{"type":"object","maxProperties":0}]},
                       {"allOf":[claim,{"required":["claim_text"]}]}, {"$ref":"#/$defs/claim","$defs":{"claim":claim}}):
            formats.append({"type":"json_schema","json_schema":{"name":"union","schema":schema}})
        for fmt in formats:
            messages = [{"role":"system","content":"Write"}]
            want, validator = S.prepare_format(fmt,messages,with_tools=True)
            for text in (json.dumps(STORY) if fmt == FORMAT else '{"claim_text":"你好"}',"```json\n{}\n```", "prose {} suffix", "[]", "{}", '{"x":1,"x":2}', '{"x":NaN}'):
                for finish in ("stop","length","cancelled"):
                    try: result = S.validated_json(text,validator,finish); valid = True
                    except S.StructuredOutputError: valid = False
                    got = self.native.raw({"op":"structured","format":fmt,"messages":messages,"with_tools":True,"text":text,"finish":finish})
                    self.assertEqual("error" not in got,valid,(fmt,text,finish,got))
                    if valid: self.assertEqual(got["result"],{"enabled":True,"messages":want,"text":result})
        for schema in ({"type":"object","properties":{"x":{"type":"invalid"}}}, {"type":"object","$ref":"https://other.example/schema"},
                       {"type":["object","null"]},{"anyOf":[]},{"$ref":"#/$defs/a","$defs":{"a":{"$ref":"#/$defs/a"}}}):
            self.assertIn("error",self.native.raw({"op":"structured","format":{"type":"json_schema","json_schema":{"name":"bad","schema":schema}}}))

    def test_forced_tool_choice(self):
        for tools in (None,[{"name":"run"}],[{"name":"run"},{"name":"other"}]):
            for choice in (None,"auto","none","required",{"type":"any"},{"type":"function","function":{"name":"run"}}, {"type":"tool","name":"other"}):
                self.assertEqual(self.native.ask({"op":"forced_call","tools":tools,"choice":choice}),F.forced_call(choice,tools) or "")

    def test_without_python_on_path(self):
        native = Native(self.path,no_python=True)
        try:
            self.assertEqual(native.ask({"op":"encode","text":"你好"}),self.tok.encode("你好"))
            self.assertIn("assistant",native.ask({"op":"render","messages":[{"role":"user","content":"hi"}]}))
        finally: native.close()

    def test_tokenizer_golden_ids(self):
        fixture = json.loads((ROOT / "tests/runtime/tokenizer_golden.json").read_text(encoding="utf-8"))
        tok = ST.Tokenizer(fixture["tokens"],fixture["merges"],fixture["types"])
        directory = Path(self.tmp.name) / "golden"
        save_tokenizer(tok,directory); native = Native(directory)
        try:
            for case in fixture["cases"]:
                self.assertEqual(tok.encode(case["text"],case["parse_special"]),case["ids"])
                self.assertEqual(native.ask({"op":"encode","text":case["text"],"parse_special":case["parse_special"]}),case["ids"])
        finally: native.close()

    def test_invalid_tokenizer_metadata(self):
        # Startup must reject bad data instead of returning plausible token IDs.
        directory = Path(self.tmp.name) / "bad"
        for kind in ("pre","pattern","size","types","ids","merge","duplicate"):
            save_tokenizer(self.tok,directory)
            cfg = json.loads((directory / "tokenizer.json").read_text(encoding="utf-8"))
            if kind == "pre": cfg["pre"] = "qwen3"
            elif kind == "pattern": cfg["pre_pattern"] = "\\p{L}+"
            elif kind == "size": cfg["vocab_size"] += 1
            elif kind == "types": (directory / "token_type.json").write_text("[1]",encoding="utf-8")
            elif kind == "ids":
                vocab = json.loads((directory / "vocab.json").read_text(encoding="utf-8")); vocab[next(iter(vocab))] = len(vocab)
                (directory / "vocab.json").write_text(json.dumps(vocab),encoding="utf-8")
            elif kind == "merge": (directory / "merges.txt").write_text("missing token",encoding="utf-8")
            elif kind == "duplicate":
                (directory / "vocab.json").write_text('{"a":0,"a":1}',encoding="utf-8")
            (directory / "tokenizer.json").write_text(json.dumps(cfg),encoding="utf-8")
            result = subprocess.run([str(ARGS.exe.resolve()),"--tokenizer",str(directory)],capture_output=True,timeout=20)
            self.assertNotEqual(result.returncode,0,kind)
            self.assertTrue(result.stderr,kind)

if __name__ == "__main__":
    suite = unittest.defaultTestLoader.loadTestsFromTestCase(Parity)
    result = unittest.TextTestRunner(verbosity=2).run(suite)
    report = {"tests":result.testsRun,"failures":len(result.failures),"errors":len(result.errors),"skips":len(result.skipped),"native_operations":COUNTS}
    if ARGS.report: ARGS.report.write_text(json.dumps(report,indent=2),encoding="utf-8")
    print(json.dumps(report))
    sys.exit(not result.wasSuccessful())
