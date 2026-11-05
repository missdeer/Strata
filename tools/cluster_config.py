"""Validate the shared, static startup plan used by every Strata cluster node."""
from __future__ import annotations

import hashlib
import ipaddress
import json
from pathlib import Path
import re

HEX = "0123456789abcdef"


def fields(obj, required, optional=()):
    if not isinstance(obj, dict) or set(obj) - set(required) - set(optional) or set(required) - set(obj):
        raise ValueError(f"expected fields {sorted(required)}; optional {sorted(optional)}")


def integer(value, low, high, name):
    if type(value) is not int or not low <= value <= high:
        raise ValueError(f"{name} must be an integer in {low}..{high}")


def string(value, name):
    if not isinstance(value, str) or not value.strip():
        raise ValueError(f"{name} must be a nonempty string")


def no_duplicates(pairs):
    result = {}
    for key, value in pairs:
        if key in result:
            raise ValueError(f"duplicate JSON key: {key}")
        result[key] = value
    return result


class ClusterConfig:
    def __init__(self, data):
        fields(data, ("schema_version", "cluster_id", "revision", "model", "scheduler", "nodes"))
        integer(data["schema_version"], 1, 1, "schema_version")
        integer(data["revision"], 1, 0xffffffff, "revision")
        string(data["cluster_id"], "cluster_id")
        model = data["model"]
        fields(model, ("name", "layers", "experts"))
        string(model["name"], "model name")
        integer(model["layers"], 1, 128, "model layers")
        integer(model["experts"], 512, 512, "model experts")
        schedule = data["scheduler"]
        fields(schedule, ("policy", "local_gpu_cache_first", "timeout_ms"))
        if schedule["policy"] != "expert-ranges" or schedule["local_gpu_cache_first"] is not True:
            raise ValueError("supported policy: expert-ranges, local_gpu_cache_first=true")
        integer(schedule["timeout_ms"], 100, 60000, "timeout_ms")
        nodes = data["nodes"]
        if not isinstance(nodes, list) or not 2 <= len(nodes) <= 64:
            raise ValueError("nodes must contain 2..64 entries")
        self.nodes, self.workers = {}, []
        coordinators, addresses = [], set()
        for node in nodes:
            if not isinstance(node, dict):
                raise ValueError("node must be an object")
            common = ("id", "online", "role", "host")
            if node.get("role") == "coordinator":
                fields(node, common + ("server_config", "engine", "context", "prefill"),
                       ("network_reduce", "network_local_ram_mib"))
                if "network_reduce" in node and type(node["network_reduce"]) is not bool:
                    raise ValueError("network_reduce must be true or false")
                integer(node.get("network_local_ram_mib", 0), 0, 65536, "network_local_ram_mib")
                for name in ("server_config", "engine"):
                    string(node[name], name)
                integer(node["context"], 1024, 131072, "context")
                integer(node["prefill"], 512, 2048, "prefill")
                coordinators.append(node)
            elif node.get("role") == "expert-worker":
                fields(node, common + ("port", "backend", "device", "workers", "ram_cache_mib",
                    "gpu_cache_mib", "pack", "native", "library", "expert_ranges"), ("preload_experts",))
                if "preload_experts" in node and type(node["preload_experts"]) is not bool:
                    raise ValueError("preload_experts must be true or false")
                if node["backend"] not in ("cpu", "gpu"):
                    raise ValueError("worker backend must be cpu or gpu")
                for name in ("pack", "native", "library"):
                    string(node[name], name)
                for name, low, high in (("port", 1, 65535), ("device", 0, 255), ("workers", 0, 256),
                        ("ram_cache_mib", 64, 1048576), ("gpu_cache_mib", 64, 65536)):
                    integer(node[name], low, high, name)
                ranges = node["expert_ranges"]
                if not isinstance(ranges, list):
                    raise ValueError("expert_ranges must be an array of inclusive [first,last] ranges")
                seen = set()
                for interval in ranges:
                    if not isinstance(interval, list) or len(interval) != 2:
                        raise ValueError("expert range must be [first,last]")
                    lo, hi = interval
                    integer(lo, 0, 511, "first expert"); integer(hi, lo, 511, "last expert")
                    if seen.intersection(range(lo, hi + 1)):
                        raise ValueError("overlapping ranges within a node")
                    seen.update(range(lo, hi + 1))
                if node["online"] and not ranges:
                    raise ValueError("online worker needs expert ranges")
                if node["online"]:
                    self.workers.append(node)
            else:
                raise ValueError("role must be coordinator or expert-worker")
            if not isinstance(node["id"], str) or not re.fullmatch(r"[A-Za-z0-9_-]{1,64}", node["id"]):
                raise ValueError("node id must contain 1..64 letters, digits, underscores or hyphens")
            if node["id"] in self.nodes:
                raise ValueError("duplicate node id")
            if type(node["online"]) is not bool:
                raise ValueError("online must be true or false")
            ipaddress.IPv4Address(node["host"])
            if node["host"] in addresses:
                raise ValueError("each node must have a distinct IPv4 address")
            addresses.add(node["host"])
            self.nodes[node["id"]] = node
        if len(coordinators) != 1 or not coordinators[0]["online"]:
            raise ValueError("exactly one online coordinator is required")
        if not 1 <= len(self.workers) <= 16:
            raise ValueError("the plan requires 1..16 online expert workers")
        self.coordinator = coordinators[0]
        self.workers.sort(key=lambda node: node["id"])
        owners = [None] * 512
        for i, worker in enumerate(self.workers):
            for lo, hi in worker["expert_ranges"]:
                for expert in range(lo, hi + 1):
                    if owners[expert] is not None:
                        raise ValueError(f"expert {expert} has multiple online owners")
                    owners[expert] = i
        if None in owners:
            raise ValueError(f"expert {owners.index(None)} has no online owner")
        self.assignment = "".join(HEX[i] for i in owners)
        self.endpoints = ",".join(f"{node['host']}:{node['port']}" for node in self.workers)
        self.data = data
        self.digest = hashlib.sha256(json.dumps(data, sort_keys=True, ensure_ascii=True,
                                                separators=(",", ":")).encode("ascii")).hexdigest()

    def node(self, node_id):
        if node_id not in self.nodes:
            raise ValueError(f"node {node_id} is not in the configuration")
        node = self.nodes[node_id]
        if not node["online"]:
            raise ValueError(f"node {node_id} is offline in this startup plan")
        return node


def load_cluster(path):
    path = Path(path).resolve()
    if path.stat().st_size > 1 << 20:
        raise ValueError("cluster configuration exceeds 1 MiB")
    return ClusterConfig(json.loads(path.read_text(encoding="utf-8-sig"), object_pairs_hook=no_duplicates))
