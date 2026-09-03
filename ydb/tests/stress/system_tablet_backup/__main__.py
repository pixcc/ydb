# -*- coding: utf-8 -*-
import argparse

from ydb.tests.stress.system_tablet_backup.workload import WorkloadRunner
from ydb.tests.stress.system_tablet_backup.workload.registry import all_workloads
from ydb.tests.stress.common.instrumented_client import InstrumentedYdbClient


def parse_rps(values):
    """--rps ddl_churn.create=5 -> {"ddl_churn.create": 5.0}"""
    rps = {}
    for value in values:
        if "=" not in value:
            raise argparse.ArgumentTypeError("--rps expects <name>=<value>, got %r" % value)
        name, raw = value.split("=", 1)
        try:
            rps[name.strip()] = float(raw)
        except ValueError:
            raise argparse.ArgumentTypeError("--rps value must be a number, got %r" % raw)
    return rps


if __name__ == "__main__":
    parser = argparse.ArgumentParser(
        description="system tablet backup workload", formatter_class=argparse.RawDescriptionHelpFormatter
    )
    parser.add_argument("--endpoint", default="localhost:2135", help="An endpoint to be used")
    parser.add_argument("--mon-endpoint", default="localhost:8765", help="A monitoring endpoint to be used")
    parser.add_argument("--database", default="Root/test", help="A database to connect")
    parser.add_argument("--duration", default=10**9, type=lambda x: int(x), help="A duration of workload in seconds.")
    parser.add_argument("--backup-path", help="A path to system tablet backup directory to be used for validation")
    parser.add_argument(
        "--workload",
        action="append",
        default=[],
        help="run only these load generators (repeatable); default: every generator enabled by default",
    )
    parser.add_argument(
        "--exclude-workload", action="append", default=[], help="skip these load generators (repeatable)"
    )
    parser.add_argument(
        "--rps",
        action="append",
        default=[],
        metavar="NAME=VALUE",
        help="override a generator's target rate, e.g. ddl_churn.create=5 (repeatable)",
    )
    parser.add_argument(
        "--ledger",
        help="write an operation ledger (JSONL) here; enables the ledger-backed consistency checks",
    )
    parser.add_argument("--list-workloads", action="store_true", help="list load generators and exit")
    args = parser.parse_args()

    if args.list_workloads:
        for spec in all_workloads():
            default = "on" if spec.enabled_by_default else "off"
            rate = "" if spec.target_rps is None else " (%.3g/s)" % spec.target_rps
            print("%-16s [%s]%s %s" % (spec.name, default, rate, spec.description))
        raise SystemExit(0)

    client = InstrumentedYdbClient(args.endpoint, args.database, True)
    client.wait_connection()
    with WorkloadRunner(
        client,
        args.duration,
        args.endpoint,
        args.mon_endpoint,
        args.backup_path,
        only_workloads=args.workload,
        exclude_workloads=args.exclude_workload,
        rps=parse_rps(args.rps),
        ledger_path=args.ledger,
    ) as runner:
        runner.run()
