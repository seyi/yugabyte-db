// Copyright (c) YugaByte, Inc.
//
// Licensed under the Apache License, Version 2.0 (the "License"); you may not use this file except
// in compliance with the License.  You may obtain a copy of the License at
//
// http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software distributed under the License
// is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express
// or implied.  See the License for the specific language governing permissions and limitations
// under the License.
//

#include "yb/tablet/tablet_snapshots.h"

#include <boost/algorithm/string/predicate.hpp>

#include "yb/docdb/consensus_frontier.h"
#include "yb/docdb/docdb_rocksdb_util.h"

#include "yb/rocksdb/db.h"
#include "yb/rocksdb/util/file_util.h"

#include "yb/rocksdb/utilities/checkpoint.h"

#include "yb/tablet/tablet.h"
#include "yb/tablet/tablet_metadata.h"
#include "yb/tablet/operations/snapshot_operation.h"

#include "yb/util/pending_op_counter.h"
#include "yb/util/scope_exit.h"
#include "yb/util/trace.h"

namespace yb {
namespace tablet {

namespace {

const std::string kSnapshotsDirSuffix = ".snapshots";
const std::string kTempSnapshotDirSuffix = ".tmp";

} // namespace

TabletSnapshots::TabletSnapshots(Tablet* tablet) : TabletComponent(tablet) {}

std::string TabletSnapshots::SnapshotsDirName(const std::string& rocksdb_dir) {
  return rocksdb_dir + kSnapshotsDirSuffix;
}

bool TabletSnapshots::IsTempSnapshotDir(const std::string& dir) {
  return boost::ends_with(dir, kTempSnapshotDirSuffix);
}

Status TabletSnapshots::Bootstrap(SnapshotOperationState* tx_state) {
  RETURN_NOT_OK(Prepare(tx_state));

  return Apply(tx_state);
}

Status TabletSnapshots::Apply(SnapshotOperationState* tx_state) {
  // Apply the snapshot operation to the tablet.
  switch (tx_state->request()->operation()) {
    case tserver::TabletSnapshotOpRequestPB::CREATE: {
      return Create(tx_state);
    }
    case tserver::TabletSnapshotOpRequestPB::RESTORE: {
      return Restore(tx_state);
    }
    case tserver::TabletSnapshotOpRequestPB::DELETE: {
      return Delete(tx_state);
    }
    case tserver::TabletSnapshotOpRequestPB::UNKNOWN: break; // Not handled.
  }

  FATAL_INVALID_ENUM_VALUE(tserver::TabletSnapshotOpRequestPB::Operation,
                           tx_state->request()->operation());
}

Status TabletSnapshots::Replicated(SnapshotOperationState* tx_state) {
  RETURN_NOT_OK(Apply(tx_state));

  // The schema lock was acquired by Tablet::PrepareForCreateSnapshot.
  // Normally, we would release it in tablet.cc after applying the operation,
  // but currently we need to wait until after the COMMIT message is logged
  // to release this lock as a workaround for KUDU-915. See the same TODO in
  // AlterSchemaOperation().
  tx_state->ReleaseSchemaLock();

  // Now that all of the changes have been applied and the commit is durable
  // make the changes visible to readers.
  TRACE("SnapshotOperation: making snapshot visible");
  tx_state->Finish();

  return Status::OK();
}

Status TabletSnapshots::Prepare(SnapshotOperationState* tx_state) {
  tx_state->AcquireSchemaLock(&schema_lock());
  return Status::OK();
}

Status TabletSnapshots::Create(SnapshotOperationState* tx_state) {
  ScopedPendingOperation scoped_read_operation(&pending_op_counter());
  RETURN_NOT_OK(scoped_read_operation);

  Status s = regular_db().Flush(rocksdb::FlushOptions());
  if (PREDICT_FALSE(!s.ok())) {
    LOG_WITH_PREFIX(WARNING) << "RocksDB flush status: " << s;
    return s.CloneAndPrepend("Unable to flush RocksDB");
  }

  const string top_snapshots_dir = SnapshotsDirName(metadata().rocksdb_dir());
  RETURN_NOT_OK_PREPEND(
      metadata().fs_manager()->CreateDirIfMissingAndSync(top_snapshots_dir),
      Format("Unable to create snapshots directory $0", top_snapshots_dir));

  Env* const env = metadata().fs_manager()->env();
  const string snapshot_dir = JoinPathSegments(top_snapshots_dir,
                                               tx_state->request()->snapshot_id());
  // Delete previous snapshot in the same directory if it exists.
  if (env->FileExists(snapshot_dir)) {
    LOG_WITH_PREFIX(INFO) << "Deleting old snapshot dir " << snapshot_dir;
    RETURN_NOT_OK_PREPEND(env->DeleteRecursively(snapshot_dir),
                          "Cannot recursively delete old snapshot dir " + snapshot_dir);
    RETURN_NOT_OK_PREPEND(env->SyncDir(top_snapshots_dir),
                          "Cannot sync top snapshots dir " + top_snapshots_dir);
  }

  LOG_WITH_PREFIX(INFO) << "Started tablet snapshot creation in folder: " << snapshot_dir;

  const string tmp_snapshot_dir = snapshot_dir + kTempSnapshotDirSuffix;

  // Delete temp directory if it exists.
  if (env->FileExists(tmp_snapshot_dir)) {
    LOG_WITH_PREFIX(INFO) << "Deleting old temp snapshot dir " << tmp_snapshot_dir;
    RETURN_NOT_OK_PREPEND(env->DeleteRecursively(tmp_snapshot_dir),
                          "Cannot recursively delete old temp snapshot dir " + tmp_snapshot_dir);
    RETURN_NOT_OK_PREPEND(env->SyncDir(top_snapshots_dir),
                          "Cannot sync top snapshots dir " + top_snapshots_dir);
  }

  bool exit_on_failure = true;
  // Delete snapshot (RocksDB checkpoint) directories on exit.
  auto se = ScopeExit(
      [this, env, &exit_on_failure, &snapshot_dir, &tmp_snapshot_dir, &top_snapshots_dir] {
    bool do_sync = false;

    if (env->FileExists(tmp_snapshot_dir)) {
      do_sync = true;
      const Status deletion_status = env->DeleteRecursively(tmp_snapshot_dir);
      if (PREDICT_FALSE(!deletion_status.ok())) {
        LOG_WITH_PREFIX(WARNING)
            << "Cannot recursively delete temp snapshot dir "
            << tmp_snapshot_dir << ": " << deletion_status;
      }
    }

    if (exit_on_failure && env->FileExists(snapshot_dir)) {
      do_sync = true;
      const Status deletion_status = env->DeleteRecursively(snapshot_dir);
      if (PREDICT_FALSE(!deletion_status.ok())) {
        LOG_WITH_PREFIX(WARNING)
            << "Cannot recursively delete snapshot dir " << snapshot_dir << ": " << deletion_status;
      }
    }

    if (do_sync) {
      const Status sync_status = env->SyncDir(top_snapshots_dir);
      if (PREDICT_FALSE(!sync_status.ok())) {
        LOG_WITH_PREFIX(WARNING)
            << "Cannot sync top snapshots dir " << top_snapshots_dir << ": " << sync_status;
      }
    }
  });

  // Note: checkpoint::CreateCheckpoint() calls DisableFileDeletions()/EnableFileDeletions()
  //       for the RocksDB object.
  s = CreateCheckpoint(tmp_snapshot_dir);
  if (PREDICT_FALSE(!s.ok())) {
    LOG_WITH_PREFIX(WARNING) << "Cannot create RocksDB checkpoint: " << s;
    return s.CloneAndPrepend("Cannot create RocksDB checkpoint");
  }

  RETURN_NOT_OK_PREPEND(
      env->RenameFile(tmp_snapshot_dir, snapshot_dir),
      Format("Cannot rename temp snapshot dir $0 to $1", tmp_snapshot_dir, snapshot_dir));
  RETURN_NOT_OK_PREPEND(
      env->SyncDir(top_snapshots_dir),
      Format("Cannot sync top snapshots dir $0", top_snapshots_dir));

  // Record the fact that we've executed the "create snapshot" Raft operation. We are not forcing
  // the flushed frontier to have this exact value, although in practice it will, since this is the
  // latest operation we've ever executed in this Raft group. This way we keep the current value
  // of history cutoff.
  docdb::ConsensusFrontier frontier;
  frontier.set_op_id(tx_state->op_id());
  frontier.set_hybrid_time(tx_state->hybrid_time());
  RETURN_NOT_OK(tablet().ModifyFlushedFrontier(
      frontier, rocksdb::FrontierModificationMode::kUpdate));

  LOG_WITH_PREFIX(INFO) << "Complete snapshot creation in folder: " << snapshot_dir;

  exit_on_failure = false;
  return Status::OK();
}

Status TabletSnapshots::Restore(SnapshotOperationState* tx_state) {
  const std::string top_snapshots_dir = SnapshotsDirName(metadata().rocksdb_dir());
  const std::string snapshot_dir = tx_state->GetSnapshotDir(top_snapshots_dir);

  docdb::ConsensusFrontier frontier;
  frontier.set_op_id(tx_state->op_id());
  frontier.set_hybrid_time(tx_state->hybrid_time());
  const Status s = RestoreCheckpoint(snapshot_dir, frontier);
  VLOG_WITH_PREFIX(1) << "Complete checkpoint restoring with result " << s << " in folder: "
                      << metadata().rocksdb_dir();
  return s;
}

Status TabletSnapshots::RestoreCheckpoint(
    const std::string& dir, const docdb::ConsensusFrontier& frontier) {
  // The following two lines can't just be changed to RETURN_NOT_OK(PauseReadWriteOperations()):
  // op_pause has to stay in scope until the end of the function.
  auto op_pause = PauseReadWriteOperations();
  RETURN_NOT_OK(op_pause);

  // Check if tablet is in shutdown mode.
  if (tablet().IsShutdownRequested()) {
    return STATUS(IllegalState, "Tablet was shut down");
  }

  std::lock_guard<std::mutex> lock(create_checkpoint_lock());

  const rocksdb::SequenceNumber sequence_number = regular_db().GetLatestSequenceNumber();
  const string db_dir = regular_db().GetName();
  const std::string intents_db_dir = has_intents_db() ? intents_db().GetName() : std::string();

  // Destroy DB object.
  // TODO: snapshot current DB and try to restore it in case of failure.
  RETURN_NOT_OK(ResetRocksDBs(/* destroy= */ true));

  auto s = rocksdb::CopyDirectory(
      &rocksdb_env(), dir, db_dir, rocksdb::CreateIfMissing::kTrue);
  if (PREDICT_FALSE(!s.ok())) {
    LOG_WITH_PREFIX(WARNING) << "Copy checkpoint files status: " << s;
    return STATUS(IllegalState, "Unable to copy checkpoint files", s.ToString());
  }

  if (!intents_db_dir.empty()) {
    auto intents_tmp_dir = JoinPathSegments(dir, tablet::kIntentsSubdir);
    rocksdb_env().RenameFile(intents_db_dir, intents_db_dir);
  }

  // Reopen database from copied checkpoint.
  // Note: db_dir == metadata()->rocksdb_dir() is still valid db dir.
  s = OpenRocksDBs();
  if (PREDICT_FALSE(!s.ok())) {
    LOG_WITH_PREFIX(WARNING) << "Failed tablet db opening from checkpoint: " << s;
    return s;
  }

  docdb::ConsensusFrontier final_frontier = frontier;
  rocksdb::UserFrontierPtr checkpoint_flushed_frontier = regular_db().GetFlushedFrontier();

  // The history cutoff we are setting after restoring to this snapshot is determined by the
  // compactions that were done in the checkpoint, not in the old state of RocksDB in this replica.
  if (checkpoint_flushed_frontier) {
    final_frontier.set_history_cutoff(
        down_cast<docdb::ConsensusFrontier&>(*checkpoint_flushed_frontier).history_cutoff());
  }

  s = tablet().ModifyFlushedFrontier(final_frontier, rocksdb::FrontierModificationMode::kForce);
  if (PREDICT_FALSE(!s.ok())) {
    LOG_WITH_PREFIX(WARNING) << "Failed tablet DB setting flushed op id: " << s;
    return s;
  }

  LOG_WITH_PREFIX(INFO) << "Checkpoint restored from " << dir;
  LOG_WITH_PREFIX(INFO) << "Sequence numbers: old=" << sequence_number
            << ", restored=" << regular_db().GetLatestSequenceNumber();

  LOG_WITH_PREFIX(INFO) << "Re-enabling compactions";
  s = tablet().EnableCompactions(&op_pause);
  if (!s.ok()) {
    LOG_WITH_PREFIX(WARNING) << "Failed to enable compactions after restoring a checkpoint";
    return s;
  }

  DCHECK(op_pause.status().ok());  // Ensure that op_pause stays in scope throughout this function.

  return Status::OK();
}

Status TabletSnapshots::Delete(SnapshotOperationState* tx_state) {
  const std::string top_snapshots_dir = SnapshotsDirName(metadata().rocksdb_dir());
  const std::string snapshot_dir =
      JoinPathSegments(top_snapshots_dir, tx_state->request()->snapshot_id());

  std::lock_guard<std::mutex> lock(create_checkpoint_lock());
  Env* const env = metadata().fs_manager()->env();

  if (env->FileExists(snapshot_dir)) {
    const Status deletion_status = env->DeleteRecursively(snapshot_dir);
    if (PREDICT_FALSE(!deletion_status.ok())) {
      LOG_WITH_PREFIX(WARNING) << "Cannot recursively delete snapshot dir " << snapshot_dir
                               << ": " << deletion_status;
    }

    const Status sync_status = env->SyncDir(top_snapshots_dir);
    if (PREDICT_FALSE(!sync_status.ok())) {
      LOG_WITH_PREFIX(WARNING) << "Cannot sync top snapshots dir " << top_snapshots_dir
                               << ": " << sync_status;
    }
  }

  docdb::ConsensusFrontier frontier;
  frontier.set_op_id(tx_state->op_id());
  frontier.set_hybrid_time(tx_state->hybrid_time());
  // Here we are just recording the fact that we've executed the "delete snapshot" Raft operation
  // so that it won't get replayed if we crash. No need to force the flushed frontier to be the
  // exact value set above.
  RETURN_NOT_OK(tablet().ModifyFlushedFrontier(
      frontier, rocksdb::FrontierModificationMode::kUpdate));

  LOG_WITH_PREFIX(INFO) << "Complete snapshot deletion on tablet in folder: " << snapshot_dir;

  return Status::OK();
}

Status TabletSnapshots::CreateCheckpoint(const std::string& dir) {
  ScopedPendingOperation scoped_read_operation(&pending_op_counter());
  RETURN_NOT_OK(scoped_read_operation);

  auto temp_intents_dir = dir + kIntentsDBSuffix;
  auto final_intents_dir = JoinPathSegments(dir, kIntentsSubdir);

  std::lock_guard<std::mutex> lock(create_checkpoint_lock());

  if (!has_regular_db()) {
    LOG_WITH_PREFIX(INFO) << "Skipped creating checkpoint in " << dir;
    return STATUS(NotSupported,
                  "Tablet does not have a RocksDB (could be a transaction status tablet)");
  }

  auto parent_dir = DirName(dir);
  RETURN_NOT_OK_PREPEND(metadata().fs_manager()->CreateDirIfMissing(parent_dir),
                        Format("Unable to create checkpoints directory $0", parent_dir));

  // Order does not matter because we flush both DBs and does not have parallel writes.
  Status status;
  if (has_intents_db()) {
    status = rocksdb::checkpoint::CreateCheckpoint(&intents_db(), temp_intents_dir);
  }
  if (status.ok()) {
    status = rocksdb::checkpoint::CreateCheckpoint(&regular_db(), dir);
  }
  if (status.ok() && has_intents_db()) {
    status = Env::Default()->RenameFile(temp_intents_dir, final_intents_dir);
  }

  if (!status.ok()) {
    LOG_WITH_PREFIX(WARNING) << "Create checkpoint status: " << status;
    return STATUS_FORMAT(IllegalState, "Unable to create checkpoint: $0", status);
  }
  LOG_WITH_PREFIX(INFO) << "Checkpoint created in " << dir;

  TEST_last_rocksdb_checkpoint_dir_ = dir;

  return Status::OK();
}

Status TabletSnapshots::CreateDirectories(const string& rocksdb_dir, FsManager* fs) {
  const auto top_snapshots_dir = SnapshotsDirName(rocksdb_dir);
  RETURN_NOT_OK_PREPEND(fs->CreateDirIfMissingAndSync(top_snapshots_dir),
                        Format("Unable to create snapshots directory $0", top_snapshots_dir));
  return Status::OK();
}

} // namespace tablet
} // namespace yb
