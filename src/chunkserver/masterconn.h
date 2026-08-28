/*
   Copyright 2005-2010 Jakub Kruszona-Zawadzki, Gemius SA
   Copyright 2013-2014 EditShare
   Copyright 2013-2015 Skytechnology sp. z o.o.
   Copyright 2023      Leil Storage OÜ


   SaunaFS is free software: you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation, version 3.

   SaunaFS is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with SaunaFS  If not, see <http://www.gnu.org/licenses/>.
 */

#pragma once

#include "chunkserver/bgjobs.h"
#include "common/platform.h"

#include <cstdint>
#include <vector>

#include "common/distributed_chunkserver_session.h"
#include "common/metadataserver_cluster_entry.h"

class MasterConn;

void masterconn_stats(uint64_t *bin, uint64_t *bout, uint32_t *maxjobscnt);
int masterconn_init(void);
int masterconn_init_threads(void);
MasterJobPool *masterconn_get_job_pool();
bool masterconn_canexit();

/// Reconciles the connection set against a cluster view pushed by one MDS: binds the
/// sender's id to the connection it arrived on and dials every member no connection
/// covers yet. Members that vanish keep their connection retrying; registry rows are
/// never retired, only their liveness changes.
void masterconn_apply_cluster_view(MasterConn *from, uint32_t senderMdsId,
                                   const std::vector<MetadataserverClusterEntry> &members);

/// This chunkserver's durable stable id; 0 until a distributed registration assigns one.
uint32_t masterconn_stable_id();

/// Random nonzero identity of this chunkserver process start in distributed mode.
uint64_t masterconn_incarnation();

/// The claim token this process holds, zero when it holds none. Process memory only: never
/// persisted, logged, or streamed, so cloning storage clones identity and not admission.
uint64_t masterconn_claim_token();
void masterconn_adopt_claim_token(uint64_t token);

/// Adopts an id assigned by a registration reply: persists it durably on first sight,
/// accepts a matching repeat, and refuses a mismatch (false), which must fail visibly.
bool masterconn_adopt_stable_id(uint32_t stableId);

/// True while the accepted session claim leaves a conservative serving window (always
/// true in legacy mode). Readable from any thread; the client-plane read and write
/// gates and the distributed control gate all consult it.
bool masterconn_session_serving_allowed();

/// The serving era currently admitting work; zero means authority is retired.
///
/// An era names an unbroken stretch of authority, and every unit of work is bound to the one
/// that admitted it. A cutoff retires the era for good. Readmission opens a new one and never
/// revives anything from an older one, which is the whole difference from a flag: a flag can
/// only say whether this process may serve now, and the question at a result boundary is
/// whether this particular piece of work was admitted by an authority that still holds.
///
/// Ordinary renewals that preserve uninterrupted authority do not mint a new era; doing so
/// would abort healthy work on every lease refresh. Legacy mode has one permanent era.
uint64_t masterconn_serving_era();

/// Whether @p era is nonzero and still the era admitting work. This is the question every
/// positive-result boundary asks, and it is not the same question as whether serving is
/// allowed right now.
bool masterconn_era_is_current(uint64_t era);

/// Whether a command stamped with @p claimSequence was issued by an authority this era still
/// holds. False once authority is retired, and false for a claim older than the one that
/// readmitted this process, which is how a command from before a cutoff is recognised.
///
/// Eras are minted locally and no metadata server can name one, so the claim sequence is what
/// the two sides share. An ordinary renewal raises the claim without ending the era, so work
/// issued under an earlier claim of the same era keeps running.
bool masterconn_claim_admits_work(uint64_t claimSequence);

/// The claim sequence of the currently accepted session tuple; zero when none is accepted.
uint64_t masterconn_claim_sequence();

/// Runs one lease tuple through the acceptance model, adopting a newer exact-holder
/// tuple together with its cutoff reserve. Emits the H9 lease acceptance events under
/// the given source label. Event-loop thread only.
LeaseTupleAcceptance masterconn_accept_session_lease(const ChunkserverSessionLease &incoming,
                                                     uint64_t cutoffReserveSeconds,
                                                     uint32_t senderMdsId,
                                                     uint64_t senderMdsIncarnation,
                                                     const char *source);
