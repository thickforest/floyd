#include "floyd/src/floyd_context.h"

namespace floyd {

FloydContext::FloydContext(const floyd::Options& opt,
    Log* log)
  : options_(opt),
  log_(log),
  current_term_(0),
  commit_index_(0),
  role_(Role::kFollower),
  vote_quorum(0),
  voted_for_port_(0),
  apply_cond_(&apply_mu_) {
    pthread_rwlockattr_t attr;
    pthread_rwlockattr_init(&attr);
    pthread_rwlockattr_setkind_np(&attr, PTHREAD_RWLOCK_PREFER_WRITER_NONRECURSIVE_NP);
    pthread_rwlock_init(&stat_rw_, &attr);
  }

FloydContext::~FloydContext() {
  pthread_rwlock_destory(&stat_rw_);
}

void FloydContext::Init() {
  assert(log_ != NULL);
  slash::RWLock(&stat_rw_, true);
  if (log_->metadata.has_current_term())
    current_term_ = log_->metadata.current_term();
  if (log_->metadata.has_voted_for_ip() &&
      log_->metadata.has_voted_for_port()) {
    voted_for_ip_ = log_->metadata.voted_for_ip();
    voted_for_port_ = log_->metadata.voted_for_port();
  }
  role_ = Role::kFollower;
}

void FloydContext:BecomeFollower(uint64_t new_term,
      const std::string leader_ip, int leader_port) {
  assert(current_term <= new_term)
  slash::RWLock(&stat_rw_, true);
  if (current_term_ < new_term) {
    current_term_ = new_term;
    voted_for_ip_ = "";
    voted_for_port_ = 0;
    LogApply();
  }
  if (!leader_ip.empty() && leader_port != 0) {
    leader_ip_ = leader_ip;
    leader_port_ = leader_port;
  }
  role_ = Role::kFollower;

  LOG_DEBUG("BecomeFollower: with current_term_(%lu) and new_term(%lu)",
      current_term_, new_term);
}

void FloydContext::BecomeCandidate() {
  assert(role_ == Role::kFollower || role_ == Role::kCandidate);
  slash::RWLock(&stat_rw_, true);
  switch(role_) {
    case Role::kFollower:
      LOG_DEBUG("Become Candidate since prev leader timeout, prev term: %lu, prev leader is (%s:%d)",
          current_term_, leader_ip_.c_str(), eader_port_);
      break; 
    case Role::kCandidate:
      LOG_DEBUG("Become Candidate since prev election timeout, prev term: %lu",
          current_term_, leader_ip_.c_str(), eader_port_);
      break; 
    default:
      LOG_DEBUG("Become Candidate, should not be here, role: %d", role_);
  }

  ++current_term_;
  role_ = Role::kCandidate;
  leader_ip_.clear();
  leader_port_ = 0;
  voted_for_ip_ = options_.local_ip;
  voted_for_port_ = options_.local_port;
  vote_quorum_ = 1;
  LogApply();
}

void FloydContext::BecomeLeader() {
  assert(role_ == Role::kCandidate);
  slash::RWLock(&stat_rw_, true);
  role_ = Role::kLeader;
  leader_ip_ = options_.local_ip;
  leader_port_ = options_.local_port;
  LOG_DEBUG ("I am become Leader");

  //ForEach(&PeerThread::BeginLeaderShip);
  // printf ("I am become Leader\n");

  // Append noop entry to guarantee that new leader can
  // Get commitindex timely.
  //std::vector<Log::Entry*> entries;
  //Log::Entry entry;
  //entry.set_type(floyd::raft::Entry::NOOP);
  //entry.set_term(current_term_);
  //entries.push_back(&entry);
  //Append(entries);

  //state_changed_.SignalAll();
}

void FloydContext::LogApply() {
  log_->metadata.set_current_term(current_term_);
  log_->metadata.set_voted_for_ip(voted_for_ip_);
  log_->metadata.set_voted_for_port(voted_for_port_);
  log_->UpdateMetadata();
}

bool FloydContext::VoteAndCheck(uint64_t vote_term) {
  slash::RWLock(&stat_rw_, true);
  if (current_term_ != vote_term) {
    return false;
  }
  return (++vote_quorum_) > (options_.members.size() / 2);
}

Status FloydContext::WaitApply(uint64_t commit_index, uint32_t timeout) { 
  while (log_.GetCommitIndex() < commit_index) {
    if (!commit_cond_.TimeWait(timeout)) {
      return Status::Timeout("apply timeout");
    }
  }
  return Status::OK();
}

// Peer ask my vote with it's ip, port, log_term and log_index
bool FloydContext::RequestVote(uint64_t term, const std::string ip,
    uint32_t port, uint64_t log_index, uint64_t log_term,
    uint64_t *my_term) {
  slash::RWLock l(&stat_rw_, true);
  if (term < current_term_) {
    return false; // stale term
  }

  if (term == current_term_) {
    if (!voted_for_ip_.empty()
        && (voted_for_ip_ != ip || voted_for_port_ != port)) {
      return false; // I have vote someone else
    }
  }
  
  uint64_t my_log_term = log_->GetLastLogTerm();
  uint64_t my_log_index = log_->GetLastLogIndex();
  if (log_term < my_log_term
      || (log_term == my_log_term && log_index < my_log_index)) {
    return false; // log index is not up-to-date as mine
  }

  // Got my vote
  vote_for_ip_ = ip;
  voted_for_port_ = port;
  *my_term = current_term_;
  LogApply();
  return true;
}

bool FloydContext::AppendEntires(uint64_t term,
    uint64_t pre_log_term, uint64_t pre_log_index,
    std::vector<Log::Entry*> entries, uint64_t* my_term) {
  slash::RWLock l(&stat_rw_, true);
  // Check last log
  if (pre_log_index > log_->GetLastLogIndex()
      || pre_log_term != log_->GetLastLogTerm()) {
    return false;
  }

  // Append entry
  if (pre_log_index < log_->GetLastLogIndex()) {
      log_->Resize(pre_log_index);
  }
  log_.Append(entries);
  *my_term = current_term_;

  return true;
}


}
