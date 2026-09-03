# -*- coding: utf-8 -*-
"""Invariants involving tenant tablets, which have no backups.

``ITablet::NeedBackup`` returns false as soon as ``TenantPathId`` is set, so a
tenant SchemeShard, a tenant Hive, and a database's coordinators and mediators
are never backed up.  That asymmetry is the whole problem: restoring a *cluster*
tablet rolls back one side of a reference while the tenant side keeps running
with the newer value.

Two of these get their tenant-side facts from the live cluster (``LIVE``); the
rest work from the root SchemeShard backup alone, which already lists every
database and its tablets.
"""

from __future__ import annotations

from typing import Dict, Iterator

from ..model import HIVE, LIVE, SCHEME_SHARD, ClusterState, Finding, critical, error, info, warning
from ..registry import check
from ..views import ETabletType, hive_view, schemeshard_views, uniq_part
from ._util import capped
from .sequences import _reissue_predicate


@check(
    id="I15",
    title="Hive will not hand out a tablet id a tenant Hive already owns",
    needs={HIVE: ["Tablet", "State", "Sequences"], LIVE: []},
    tags=("tenant", "sequences", "hive", "corruption"),
)
def hive_reuses_tenant_tablet_id(state: ClusterState) -> Iterator[Finding]:
    """The worst case of a stale root Hive, and invisible offline.

    The root Hive is the authority for tablet ids: a tenant Hive asks it for a
    range (``TEvRequestTabletIdSequence``), the root records the grant in
    ``Sequences``/``TabletOwners`` and the tenant stores the range in its own --
    unbacked -- database.  Roll the root back and the grant is forgotten, so the
    same range gets handed to someone else and two Hives mint identical ids.

    Nothing repairs this by itself.  The only resync that exists runs the wrong
    way: ``TEvRequestTabletOwners`` has the *tenant* ask the *root* for the
    ranges the root remembers, guarded by a one-shot ``TabletOwnersSynced`` flag
    in the tenant's state (``tx__tablet_owners_reply.cpp``).
    """
    hive = hive_view(state)

    # Report coverage gaps before anything else: a Hive that could not be read
    # contributes no ids, and staying silent would look like a pass.
    unreachable = state.live.unreachable()
    if unreachable:
        yield warning(
            "%d Hive(s) could not be read, the ids they own were not checked: %s"
            % (len(unreachable), ", ".join(str(h) for h in unreachable)),
            hives=unreachable,
        )

    foreign = state.foreign_tablet_ids
    if not foreign:
        if not unreachable:
            yield info("no live tenant tablets were read, nothing to compare against")
        return

    known = {t.tablet_id for t in hive.tablets()}
    stored_next = hive.next_tablet_id()
    own_max = max((uniq_part(t) for t in known), default=0)
    effective_next = max(stored_next or 0, own_max + 1 if known else 0)
    will_reissue = _reissue_predicate(hive.sequences(), effective_next)

    owner_of: Dict[int, int] = {}
    for live_hive in state.live.hives.values():
        for tablet_id in live_hive.tablet_ids:
            owner_of[tablet_id] = live_hive.hive_id

    at_risk = sorted(t for t in foreign if will_reissue(uniq_part(t)))

    yield from capped(
        at_risk,
        lambda tablet_id: critical(
            "tablet id %d (uniq part %d) is live under tenant Hive %s, but the root Hive "
            "will allocate it again (effective NextTabletId %d)"
            % (tablet_id, uniq_part(tablet_id), owner_of.get(tablet_id), effective_next),
            tablet_id=tablet_id,
            uniq_part=uniq_part(tablet_id),
            tenant_hive=owner_of.get(tablet_id),
            effective_next_tablet_id=effective_next,
        ),
        lambda total, rest: critical(
            "%d live tenant tablet ids will be handed out again by the root Hive" % total,
            total=total,
            effective_next_tablet_id=effective_next,
            sample=rest[:100],
        ),
    )

    if not at_risk and not unreachable:
        yield info(
            "root Hive allocator is ahead of all %d live tenant tablet ids" % len(foreign),
            tenant_tablets=len(foreign),
            effective_next_tablet_id=effective_next,
        )


@check(
    id="I16",
    title="Every database the root SchemeShard knows still has its tablets",
    needs={SCHEME_SHARD: ["SubDomains", "SubDomainShards", "Shards", "Paths"], HIVE: ["Tablet"]},
    tags=("tenant", "refs", "schemeshard"),
)
def databases_have_their_tablets(state: ClusterState) -> Iterator[Finding]:
    """A database is a set of tablets none of which are backed up.

    Losing the root SchemeShard's record of a database does not stop it running,
    but nothing owns it any more; losing Hive's record of its SchemeShard means
    the database cannot boot at all.  Both are worth reporting at database
    granularity rather than as a pile of anonymous shard findings.
    """
    hive = hive_view(state)
    hive_id = hive.tablet_id
    hive_tablets = hive.tablets_by_id()

    live_tablets = set()
    if state.live is not None:
        for live_hive in state.live.hives.values():
            live_tablets |= live_hive.tablet_ids

    for ss in schemeshard_views(state):
        subdomains = ss.databases()
        if not subdomains:
            continue

        for subdomain in subdomains:
            if subdomain.scheme_shard_id is None:
                yield warning(
                    "database %s (path %d) has no tenant SchemeShard among its shards"
                    % (subdomain.name, subdomain.path_id),
                    database=subdomain.name,
                    path_id=subdomain.path_id,
                )
                continue

            tenant_hive = subdomain.effective_hive_id
            has_own_hive = bool(tenant_hive) and tenant_hive != hive_id

            # Losing the tablet that *is* the database's Hive stops the database
            # outright: nothing else can start its other tablets.
            if has_own_hive and tenant_hive not in hive_tablets and tenant_hive not in live_tablets:
                yield critical(
                    "database %s (path %d) has its own Hive %d, and no Hive knows it: "
                    "nothing can start the database"
                    % (subdomain.name, subdomain.path_id, tenant_hive),
                    database=subdomain.name,
                    path_id=subdomain.path_id,
                    tenant_hive=tenant_hive,
                )

            # The rest of a database's system tablets may live in the root Hive
            # or in the database's own Hive -- both happen.  Only count one as
            # lost when no Hive we can see has it.
            missing = [
                t
                for t in subdomain.tablet_ids
                if t not in hive_tablets and t not in live_tablets
            ]
            if has_own_hive and state.live is None:
                if missing:
                    yield warning(
                        "database %s (path %d): %d tablet(s) are not in the root Hive and its "
                        "own Hive %d was not read, so they could not be verified"
                        % (subdomain.name, subdomain.path_id, len(missing), tenant_hive),
                        database=subdomain.name,
                        path_id=subdomain.path_id,
                        tenant_hive=tenant_hive,
                        unverified_tablets=sorted(missing)[:50],
                    )
                continue

            if not missing:
                continue

            head_missing = subdomain.scheme_shard_id in missing
            message = "database %s (path %d) has %d tablet(s) Hive does not know" % (
                subdomain.name,
                subdomain.path_id,
                len(missing),
            )
            if head_missing:
                message += ", including its SchemeShard %d: the database cannot boot" % (
                    subdomain.scheme_shard_id,
                )

            detail = dict(
                database=subdomain.name,
                path_id=subdomain.path_id,
                missing_tablets=sorted(missing)[:50],
                scheme_shard_id=subdomain.scheme_shard_id,
            )
            yield critical(message, **detail) if head_missing else error(message, **detail)

        yield info(
            "SchemeShard %d owns %d database(s): %s"
            % (
                ss.tablet_id,
                len(subdomains),
                ", ".join(sorted(s.name for s in subdomains)),
            ),
            databases={s.name: s.scheme_shard_id for s in subdomains},
        )


@check(
    id="I17",
    title="Hive holds no database tablets that the root SchemeShard forgot",
    needs={SCHEME_SHARD: ["SubDomains", "SubDomainShards", "Shards", "Paths"], HIVE: ["Tablet"]},
    tags=("tenant", "refs", "hive"),
)
def orphaned_databases(state: ClusterState) -> Iterator[Finding]:
    """Broken by a stale root SchemeShard.

    A tenant SchemeShard, Hive, coordinator or mediator that Hive still runs on
    behalf of the root SchemeShard, while the root SchemeShard has no subdomain
    for it, is an orphaned database: it keeps serving data nobody can find, and
    its shard index will be handed to a new object (see ``I6``).
    """
    hive = hive_view(state)
    tenant_types = {
        ETabletType.SCHEME_SHARD,
        ETabletType.HIVE,
        ETabletType.COORDINATOR,
        ETabletType.MEDIATOR,
    }

    for ss in schemeshard_views(state):
        claimed = set()
        for subdomain in ss.databases():
            claimed |= set(subdomain.tablet_ids)

        orphans = []
        for tablet in hive.tablets():
            if tablet.owner_tablet_id != ss.tablet_id or tablet.is_deleting:
                continue
            if tablet.tablet_type not in tenant_types:
                continue
            if tablet.tablet_id not in claimed:
                orphans.append(tablet)

        yield from capped(
            orphans,
            lambda tablet: error(
                "tablet %d (%s, owner idx %s) looks like part of a database the root "
                "SchemeShard no longer knows"
                % (
                    tablet.tablet_id,
                    ETabletType.name(tablet.tablet_type),
                    tablet.owner_idx,
                ),
                tablet_id=tablet.tablet_id,
                tablet_type=ETabletType.name(tablet.tablet_type),
                shard_idx=tablet.owner_idx,
                schemeshard=ss.tablet_id,
            ),
            lambda total, rest: error(
                "%d database tablets are orphaned by SchemeShard %d" % (total, ss.tablet_id),
                total=total,
                schemeshard=ss.tablet_id,
                sample=[t.tablet_id for t in rest[:100]],
            ),
        )


@check(
    id="I18",
    title="Tenant tablets are covered by a live reading",
    needs={SCHEME_SHARD: ["SubDomains", "SubDomainShards", "Shards", "Paths"]},
    tags=("tenant", "meta"),
)
def tenant_coverage(state: ClusterState) -> Iterator[Finding]:
    """States plainly what the run could not see.

    Without ``--mon-endpoint`` the tenant side is entirely unobserved, and the
    identifier checks silently understate the risk -- a tenant Hive's tablet ids
    are simply absent from the comparison.  Better to say so than to pass.
    """
    tenant_hives: Dict[int, str] = {}
    databases = 0
    for ss in schemeshard_views(state):
        for subdomain in ss.databases():
            databases += 1
            hive_id = subdomain.effective_hive_id
            if hive_id:
                tenant_hives[hive_id] = subdomain.name

    if not databases:
        return

    if state.live is None:
        yield warning(
            "%d database(s) with %d tenant Hive(s) were not read: tenant tablets have no "
            "backups, so without --mon-endpoint the identifier checks cannot see the ids "
            "those Hives own (I15 skipped)" % (databases, len(tenant_hives)),
            databases=databases,
            tenant_hives=sorted(tenant_hives),
        )
        return

    unread = sorted(h for h in tenant_hives if h not in state.live.hives)
    if unread:
        yield warning(
            "%d tenant Hive(s) known to SchemeShard were not read live: %s"
            % (len(unread), ", ".join(str(h) for h in unread)),
            tenant_hives=unread,
        )
    else:
        yield info(
            "all %d tenant Hive(s) were read live" % len(tenant_hives),
            tenant_hives=sorted(tenant_hives),
        )
