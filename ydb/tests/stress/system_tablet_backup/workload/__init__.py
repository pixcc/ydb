# -*- coding: utf-8 -*-
import grpc
import os
import re
import requests
import subprocess
import threading
import time
import yaml
import ydb
import shlex

from urllib.parse import urlparse, quote

from ydb.public.api.protos import ydb_config_pb2 as config_api
from ydb.public.api.protos.draft import ydb_dynamic_config_pb2 as dynconfig_api
from ydb.public.api.protos.ydb_status_codes_pb2 import StatusIds
from ydb.tests.library.clients.kikimr_config_client import ConfigClient
from ydb.tests.library.clients.kikimr_dynconfig_client import DynConfigClient

from .ledger import open_ledger
from .registry import WorkloadContext, build_workloads, select_workloads
from .generators.register_node import WorkloadRegisterNode  # noqa: F401  (kept importable)


class BackupValidator:
    SELECTOR_NAME = "system_tablet_backup_workload_validator"
    RESTORE_TIMEOUT_SECONDS = 600

    SYSTEM_TABLETS = {
        "FLAT_BS_CONTROLLER":   [72057594037932033],
        "NODE_BROKER":          [72057594037936129],
        "CONSOLE":              [72057594037936131],
    }

    RECOVERY_TABLETS = {"NODE_BROKER"}

    def __init__(self, endpoint, mon_endpoint, backup_path):
        if "://" not in endpoint:
            endpoint = "grpc://" + endpoint
        if "://" not in mon_endpoint:
            mon_endpoint = "http://" + mon_endpoint

        parsed = urlparse(endpoint)
        self.config_client = ConfigClient(parsed.hostname, parsed.port)
        self.dynconfig_client = DynConfigClient(parsed.hostname, parsed.port)

        mon_parsed = urlparse(mon_endpoint)
        self.mon_port = mon_parsed.port
        self.mon_scheme = mon_parsed.scheme

        self.mon_endpoint = mon_endpoint
        self.backup_path = backup_path

    @staticmethod
    def _assert_success(response, operation_name):
        status = response.operation.status
        if status != StatusIds.SUCCESS:
            issues = [issue.message for issue in response.operation.issues]
            raise RuntimeError(f"{operation_name} failed with status {status}: {issues}")

    def _retry(self, func, operation_name, retries=10):
        for attempt in range(retries):
            response = func()
            if response.operation.status == StatusIds.SUCCESS:
                return response
            if response.operation.status == StatusIds.UNAVAILABLE and attempt < retries - 1:
                time.sleep(1)
                continue
            self._assert_success(response, operation_name)
        return response

    def _fetch_config(self):
        try:
            response = self._retry(self.config_client.fetch_all_configs, "FetchConfig")
            result = config_api.FetchConfigResult()
            response.operation.result.Unpack(result)
            return result.config[0].config
        except grpc.RpcError as e:
            if e.code() != grpc.StatusCode.UNIMPLEMENTED:
                raise

        response = self._retry(self.dynconfig_client.fetch_config, "GetConfig")
        result = dynconfig_api.GetConfigResult()
        response.operation.result.Unpack(result)

        if result.config and result.config[0]:
            return result.config[0]

        response = self._retry(self.dynconfig_client.fetch_startup_config, "FetchStartupConfig")
        result = dynconfig_api.FetchStartupConfigResult()
        response.operation.result.Unpack(result)
        full_config = {
            "metadata": {
                "kind": "MainConfig",
                "version": 0,
                "cluster": "",
            },
            "config": yaml.safe_load(result.config),
        }
        return yaml.dump(full_config)

    def _replace_config(self, config_yaml):
        try:
            full_config = yaml.safe_load(config_yaml)
            full_config["metadata"]["version"] += 1
            versioned_yaml = yaml.dump(full_config)

            def do_replace():
                request = config_api.ReplaceConfigRequest()
                request.replace = versioned_yaml
                return self.config_client.invoke(request, "ReplaceConfig")
            self._retry(do_replace, "ReplaceConfig")
            return
        except grpc.RpcError as e:
            if e.code() != grpc.StatusCode.UNIMPLEMENTED:
                raise

        self._retry(lambda: self.dynconfig_client.replace_config(config_yaml), "ReplaceConfig")

    def _make_bootstrap_selector(self, erasure_name):
        def channel(i):
            return {
                "channel": i,
                "history": [{"from_generation": 0, "group_id": 0}],
                "channel_erasure_name": erasure_name,
            }
        tablets = []
        for tablet_type, tablet_ids in self.SYSTEM_TABLETS.items():
            for tablet_id in tablet_ids:
                tablet = {
                    "type": tablet_type,
                    "node": [1, 2, 3, 4, 5, 6, 7, 8, 9],
                    "info": {
                        "tablet_id": str(tablet_id),
                        "channels": [channel(0), channel(1), channel(2)],
                    },
                }
                if tablet_type in self.RECOVERY_TABLETS:
                    tablet["boot_type"] = "RECOVERY"
                tablets.append(tablet)
        return {
            "description": self.SELECTOR_NAME,
            "selector": {},
            "config": {
                "bootstrap_config": {
                    "tablet": tablets,
                },
            },
        }

    def _wait_recovery_node_broker_alive(self, retries=10):
        tablet_id = self.SYSTEM_TABLETS["NODE_BROKER"][0]
        url = f"{self.mon_endpoint}/tablets/?TabletID={tablet_id}"
        for attempt in range(retries):
            try:
                response = requests.get(url, timeout=30)
                response.raise_for_status()
                if "Recovery" in response.text:
                    return
            except Exception:
                pass
            time.sleep(1)
        raise RuntimeError(
            f"NodeBroker recovery mode not found after {retries} retries")

    def _restart_node_broker(self):
        tablet_id = self.SYSTEM_TABLETS["NODE_BROKER"][0]
        restart_url = f"{self.mon_endpoint}/tablets?RestartTabletID={tablet_id}"
        response = requests.get(restart_url, timeout=30)
        response.raise_for_status()
        self._wait_recovery_node_broker_alive()

    def _enable_node_broker_recovery(self):
        config_yaml = self._fetch_config()
        full_config = yaml.safe_load(config_yaml)
        erasure_name = full_config["config"].get("static_erasure", "mirror-3-dc")
        full_config.setdefault("selector_config", []).append(
            self._make_bootstrap_selector(erasure_name)
        )
        updated_yaml = yaml.dump(full_config)
        self._replace_config(updated_yaml)

    def _disable_node_broker_recovery(self):
        config_yaml = self._fetch_config()
        full_config = yaml.safe_load(config_yaml)
        full_config["selector_config"] = [
            s for s in full_config.get("selector_config", [])
            if s.get("description") != self.SELECTOR_NAME
        ]
        updated_yaml = yaml.dump(full_config)
        self._replace_config(updated_yaml)

    def _get_tablet_host(self, tablet_id, retries=10):
        url = f"{self.mon_endpoint}/tablets?TabletID={tablet_id}"
        for attempt in range(retries):
            try:
                response = requests.get(url, timeout=30)
                response.raise_for_status()
                match = re.search(r'NodeID:\s*(\d+)', response.text)
                if match:
                    node_id = int(match.group(1))
                    return self.nodes[node_id]["Host"]
            except Exception:
                pass
            time.sleep(1)
        raise RuntimeError(
            f"Failed to find host for tablet {tablet_id} after {retries} retries")

    def _fetch_node_list(self):
        response = requests.get(f"{self.mon_endpoint}/viewer/json/nodelist", timeout=30)
        response.raise_for_status()
        nodes = response.json()
        return {
            node["Id"]: node
            for node in nodes
            if not node["Host"].startswith("system.tablet.backup.fake")
        }

    def _run_on_host(self, host, cmd, timeout=30):
        is_local = host in ("localhost", "127.0.0.1", "::1")
        if is_local:
            result = subprocess.run(
                ["bash", "-c", cmd],
                capture_output=True, text=True, timeout=timeout,
            )
        else:
            result = subprocess.run(
                ["ssh", "-o", "StrictHostKeyChecking=no", host, cmd],
                capture_output=True, text=True, timeout=timeout,
            )
        if result.returncode != 0:
            raise RuntimeError(
                f"Command failed on {host}: rc={result.returncode} stderr={result.stderr.strip()}")
        return result

    def _copy_backup(self, src_host, dst_host, backup_dir):
        backup_name = os.path.basename(backup_dir)
        tmp_dir = "~/backup_tmp/" + shlex.quote(backup_name)
        parent_dir = os.path.dirname(backup_dir)
        self._run_on_host(dst_host, "mkdir -p ~/backup_tmp")
        scp_cmd = ["scp", "-o", "StrictHostKeyChecking=no", "-r",
                   f"{src_host}:{backup_dir}", f"{dst_host}:{tmp_dir}"]
        result = subprocess.run(scp_cmd, capture_output=True, text=True, timeout=120)
        if result.returncode != 0:
            raise RuntimeError(
                f"Failed to copy backup from {src_host} to {dst_host}: {result.stderr}")
        self._run_on_host(dst_host, f"sudo mkdir -p {shlex.quote(parent_dir)}")
        self._run_on_host(dst_host, f"sudo mv {tmp_dir} {shlex.quote(backup_dir)}")
        self._run_on_host(dst_host, "rmdir ~/backup_tmp 2>/dev/null || true")

    def _delete_copied_backup(self, host, backup_dir):
        self._run_on_host(host, f"sudo rm -rf {shlex.quote(backup_dir)}")

    def _node_mon_url(self, hostname):
        return f"{self.mon_scheme}://{hostname}:{self.mon_port}"

    def _start_restore(self, hostname, tablet_id, backup_path):
        url = (f"{self._node_mon_url(hostname)}/tablets/app?TabletID={tablet_id}&restoreBackup={quote(backup_path, safe='')}")
        response = requests.post(url, timeout=30)
        response.raise_for_status()
        return response.text

    @staticmethod
    def _extract_alert_message(html, alert_class):
        import re
        pattern = rf'<div[^>]*class="[^"]*{alert_class}[^"]*"[^>]*>(.*?)</div>'
        match = re.search(pattern, html, re.DOTALL)
        if match:
            text = re.sub(r'<[^>]+>', '', match.group(1)).strip()
            return text
        return ""

    def _check_restore_status(self, hostname, tablet_id):
        url = f"{self._node_mon_url(hostname)}/tablets/app?TabletID={tablet_id}"
        response = requests.get(url, timeout=30)
        response.raise_for_status()
        html = response.text

        if "alert-success" in html:
            return "success", ""
        if "alert-danger" in html:
            return "error", self._extract_alert_message(html, "alert-danger")
        if "alert-warning" in html:
            return "warning", self._extract_alert_message(html, "alert-warning")
        if "alert-info" in html:
            return "in_progress", ""
        return "restarted", ""

    def _discover_backups(self):
        hosts = set(node["Host"] for node in self.nodes.values())
        tablet_id = self.SYSTEM_TABLETS["NODE_BROKER"][0]
        tablet_backup_path = f"{self.backup_path}/node_broker/{tablet_id}"

        all_backups = []
        for host in sorted(hosts):
            cmd = (
                f'for d in {shlex.quote(tablet_backup_path)}/*; do '
                f'[ -d "$d/snapshot" ] && basename "$d"; '
                f'done; true'
            )
            result = self._run_on_host(host, cmd)
            lines = [line for line in result.stdout.strip().split("\n") if line]
            for backup_name in lines:
                all_backups.append({
                    "host": host,
                    "backup_name": backup_name,
                    "backup_path": f"{tablet_backup_path}/{backup_name}",
                })

        return all_backups

    def _restore_single_backup(self, backup_info, max_retries=3):
        tablet_id = self.SYSTEM_TABLETS["NODE_BROKER"][0]
        backup_host = backup_info["host"]
        backup_path = backup_info["backup_path"]
        backup_name = backup_info["backup_name"]

        copied_to_host = None
        try:
            for attempt in range(1, max_retries + 1):
                self._restart_node_broker()
                target_host = self._get_tablet_host(tablet_id)

                if target_host != backup_host:
                    self._copy_backup(backup_host, target_host, backup_path)
                    copied_to_host = target_host

                self._start_restore(target_host, tablet_id, backup_path)

                restarted = False
                poll_deadline = time.time() + self.RESTORE_TIMEOUT_SECONDS
                while time.time() < poll_deadline:
                    time.sleep(2)
                    status, message = self._check_restore_status(target_host, tablet_id)

                    if status == "success":
                        return "success", None
                    elif status in ("error", "warning"):
                        return status, {"backup": backup_name, "host": target_host, "reason": message}
                    elif status == "restarted":
                        restarted = True
                        break
                    # status == "in_progress" - keep polling
                else:
                    return "error", {
                        "backup": backup_name,
                        "host": target_host,
                        "reason": f"restore timed out after {self.RESTORE_TIMEOUT_SECONDS}s in in_progress state",
                    }

                if not restarted:
                    break
            return "error", {
                "backup": backup_name,
                "host": backup_host,
                "reason": "max retries exceeded due to tablet restarts",
            }
        finally:
            if copied_to_host:
                self._delete_copied_backup(copied_to_host, backup_path)

    def validate(self):
        try:
            print("Starting backup validation...")
            self.nodes = self._fetch_node_list()

            self._enable_node_broker_recovery()
            print("Enabled NodeBroker recovery mode")

            backups = self._discover_backups()
            total = len(backups)
            print(f"Discovered {total} backup(s)")

            if not backups:
                raise RuntimeError("No backups found")

            stats = {"success": 0, "warning": 0, "error": 0}
            issues = []

            for i, backup_info in enumerate(backups, 1):
                try:
                    status, info = self._restore_single_backup(backup_info)
                except Exception as e:
                    status = "error"
                    info = {
                        "backup": backup_info["backup_name"],
                        "host": backup_info["host"],
                        "reason": str(e),
                    }

                print(f"  [{i}/{total}] {backup_info['backup_name']} "
                      f"on {backup_info['host']}: {status}")

                stats[status] += 1
                if info:
                    issues.append((status, info))

            print(f"Validation: {stats['success']} ok, "
                  f"{stats['warning']} warnings, {stats['error']} errors "
                  f"/ {total} total")

            if issues:
                print("Backups requiring manual attention:")
                for status, info in issues:
                    reason = info.get("reason", "")
                    extra = f" - {reason}" if reason else ""
                    print(f"  [{status.upper()}] backup '{info['backup']}' on host {info['host']}{extra}")

            if stats["error"] > 0:
                raise RuntimeError(
                    f"Backup validation failed: "
                    f"{stats['error']} error(s) out of {total} backup(s)")
        finally:
            self._disable_node_broker_recovery()
            print("Disabled NodeBroker recovery mode")
        print("Backup validation completed")


class WorkloadRunner:
    def __init__(
        self,
        client,
        duration,
        endpoint,
        mon_endpoint,
        backup_path,
        only_workloads=(),
        exclude_workloads=(),
        rps=None,
        ledger_path=None,
    ):
        self.client = client
        self.duration = duration
        self.endpoint = endpoint
        self.mon_endpoint = mon_endpoint
        self.backup_path = backup_path
        self.only_workloads = list(only_workloads)
        self.exclude_workloads = list(exclude_workloads)
        self.rps = dict(rps or {})
        self.ledger_path = ledger_path
        self.ledger = None
        ydb.interceptor.monkey_patch_event_handler()

    def __enter__(self):
        return self

    def __exit__(self, exc_type, exc_value, traceback):
        if self.ledger is not None:
            self.ledger.close()

    def _make_context(self, stop):
        parsed = urlparse(self.endpoint if "://" in self.endpoint else "grpc://" + self.endpoint)
        self.ledger = open_ledger(self.ledger_path)
        return WorkloadContext(
            client=self.client,
            stop=stop,
            database=self.client.database,
            mon_endpoint=self.mon_endpoint,
            grpc_host=parsed.hostname or "localhost",
            grpc_port=parsed.port or 2135,
            ledger=self.ledger,
            rps=self.rps,
        )

    def run(self):
        stop = threading.Event()
        specs = select_workloads(only=self.only_workloads, exclude=self.exclude_workloads)
        ctx = self._make_context(stop)
        workloads = build_workloads(specs, ctx)

        print("Running workloads: %s" % ", ".join(spec.name for spec in specs))
        if self.ledger_path:
            print("Ledger: %s" % self.ledger_path)

        started = []
        for w in workloads:
            if w.start() is False:
                print(f"\t{w.name}: did not start, skipped")
                continue
            started.append(w)

        started_at = time.time()
        while time.time() - started_at < self.duration:
            print(f"Elapsed {(int)(time.time() - started_at)} seconds, stat:")
            for w in started:
                print(f"\t{w.name}: {w.get_stat()}")
            time.sleep(10)
        stop.set()
        print("Waiting for stop...")
        for w in started:
            w.join()
        print("Stopped")

        if self.ledger is not None:
            self.ledger.close()

        if self.backup_path:
            validator = BackupValidator(self.endpoint, self.mon_endpoint, self.backup_path)
            validator.validate()
