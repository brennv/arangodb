////////////////////////////////////////////////////////////////////////////////
/// DISCLAIMER
///
/// Copyright 2014-2016 ArangoDB GmbH, Cologne, Germany
/// Copyright 2004-2014 triAGENS GmbH, Cologne, Germany
///
/// Licensed under the Apache License, Version 2.0 (the "License");
/// you may not use this file except in compliance with the License.
/// You may obtain a copy of the License at
///
///     http://www.apache.org/licenses/LICENSE-2.0
///
/// Unless required by applicable law or agreed to in writing, software
/// distributed under the License is distributed on an "AS IS" BASIS,
/// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
/// See the License for the specific language governing permissions and
/// limitations under the License.
///
/// Copyright holder is ArangoDB GmbH, Cologne, Germany
///
/// @author Max Neunhoeffer
////////////////////////////////////////////////////////////////////////////////

#include "ClusterBlocks.h"
#include "Aql/ExecutionEngine.h"
#include "Aql/AqlValue.h"
#include "Basics/Exceptions.h"
#include "Basics/json-utilities.h"
#include "Basics/StaticStrings.h"
#include "Basics/StringUtils.h"
#include "Basics/StringBuffer.h"
#include "Basics/VelocyPackHelper.h"
#include "Cluster/ClusterComm.h"
#include "Cluster/ClusterInfo.h"
#include "Cluster/ClusterMethods.h"
#include "Dispatcher/DispatcherThread.h"
#include "V8/v8-globals.h"
#include "VocBase/server.h"
#include "VocBase/vocbase.h"

#include <velocypack/Builder.h>
#include <velocypack/Collection.h>
#include <velocypack/Slice.h>
#include <velocypack/velocypack-aliases.h>

using namespace arangodb;
using namespace arangodb::aql;

using Json = arangodb::basics::Json;
using JsonHelper = arangodb::basics::JsonHelper;
using VelocyPackHelper = arangodb::basics::VelocyPackHelper;
using StringBuffer = arangodb::basics::StringBuffer;

// uncomment the following to get some debugging information
#if 0
#define ENTER_BLOCK \
  try {             \
    (void)0;
#define LEAVE_BLOCK                                                            \
  }                                                                            \
  catch (...) {                                                                \
    std::cout << "caught an exception in " << __FUNCTION__ << ", " << __FILE__ \
              << ":" << __LINE__ << "!\n";                                     \
    throw;                                                                     \
  }
#else
#define ENTER_BLOCK
#define LEAVE_BLOCK
#endif

GatherBlock::GatherBlock(ExecutionEngine* engine, GatherNode const* en)
    : ExecutionBlock(engine, en),
      _sortRegisters(),
      _isSimple(en->getElements().empty()) {
  if (!_isSimple) {
    for (auto const& p : en->getElements()) {
      // We know that planRegisters has been run, so
      // getPlanNode()->_registerPlan is set up
      auto it = en->getRegisterPlan()->varInfo.find(p.first->id);
      TRI_ASSERT(it != en->getRegisterPlan()->varInfo.end());
      TRI_ASSERT(it->second.registerId < ExecutionNode::MaxRegisterId);
      _sortRegisters.emplace_back(
          std::make_pair(it->second.registerId, p.second));
    }
  }
}

GatherBlock::~GatherBlock() {
  ENTER_BLOCK
  for (std::deque<AqlItemBlock*>& x : _gatherBlockBuffer) {
    for (AqlItemBlock* y : x) {
      delete y;
    }
    x.clear();
  }
  _gatherBlockBuffer.clear();
  LEAVE_BLOCK
}

/// @brief initialize
int GatherBlock::initialize() {
  ENTER_BLOCK
  _atDep = 0;
  return ExecutionBlock::initialize();
  LEAVE_BLOCK
}

/// @brief shutdown: need our own method since our _buffer is different
int GatherBlock::shutdown(int errorCode) {
  ENTER_BLOCK
  // don't call default shutdown method since it does the wrong thing to
  // _gatherBlockBuffer
  for (auto it = _dependencies.begin(); it != _dependencies.end(); ++it) {
    int res = (*it)->shutdown(errorCode);

    if (res != TRI_ERROR_NO_ERROR) {
      return res;
    }
  }

  if (!_isSimple) {
    for (std::deque<AqlItemBlock*>& x : _gatherBlockBuffer) {
      for (AqlItemBlock* y : x) {
        delete y;
      }
      x.clear();
    }
    _gatherBlockBuffer.clear();
    _gatherBlockPos.clear();
  }

  return TRI_ERROR_NO_ERROR;
  LEAVE_BLOCK
}

/// @brief initializeCursor
int GatherBlock::initializeCursor(AqlItemBlock* items, size_t pos) {
  ENTER_BLOCK
  int res = ExecutionBlock::initializeCursor(items, pos);

  if (res != TRI_ERROR_NO_ERROR) {
    return res;
  }

  _atDep = 0;

  if (!_isSimple) {
    for (std::deque<AqlItemBlock*>& x : _gatherBlockBuffer) {
      for (AqlItemBlock* y : x) {
        delete y;
      }
      x.clear();
    }
    _gatherBlockBuffer.clear();
    _gatherBlockPos.clear();

    _gatherBlockBuffer.reserve(_dependencies.size());
    _gatherBlockPos.reserve(_dependencies.size());
    for (size_t i = 0; i < _dependencies.size(); i++) {
      _gatherBlockBuffer.emplace_back();
      _gatherBlockPos.emplace_back(std::make_pair(i, 0));
    }
  }

  _done = false;
  return TRI_ERROR_NO_ERROR;
  LEAVE_BLOCK
}

/// @brief count: the sum of the count() of the dependencies or -1 (if any
/// dependency has count -1
int64_t GatherBlock::count() const {
  ENTER_BLOCK
  int64_t sum = 0;
  for (auto const& x : _dependencies) {
    if (x->count() == -1) {
      return -1;
    }
    sum += x->count();
  }
  return sum;
  LEAVE_BLOCK
}

/// @brief remaining: the sum of the remaining() of the dependencies or -1 (if
/// any dependency has remaining -1
int64_t GatherBlock::remaining() {
  ENTER_BLOCK
  int64_t sum = 0;
  for (auto const& x : _dependencies) {
    if (x->remaining() == -1) {
      return -1;
    }
    sum += x->remaining();
  }
  return sum;
  LEAVE_BLOCK
}

/// @brief hasMore: true if any position of _buffer hasMore and false
/// otherwise.
bool GatherBlock::hasMore() {
  ENTER_BLOCK
  if (_done) {
    return false;
  }

  if (_isSimple) {
    for (size_t i = 0; i < _dependencies.size(); i++) {
      if (_dependencies.at(i)->hasMore()) {
        return true;
      }
    }
  } else {
    for (size_t i = 0; i < _gatherBlockBuffer.size(); i++) {
      if (!_gatherBlockBuffer.at(i).empty()) {
        return true;
      } else if (getBlock(i, DefaultBatchSize(), DefaultBatchSize())) {
        _gatherBlockPos.at(i) = std::make_pair(i, 0);
        return true;
      }
    }
  }
  _done = true;
  return false;
  LEAVE_BLOCK
}

/// @brief getSome
AqlItemBlock* GatherBlock::getSome(size_t atLeast, size_t atMost) {
  ENTER_BLOCK
  if (_done) {
    return nullptr;
  }

  // the simple case . . .
  if (_isSimple) {
    auto res = _dependencies.at(_atDep)->getSome(atLeast, atMost);
    while (res == nullptr && _atDep < _dependencies.size() - 1) {
      _atDep++;
      res = _dependencies.at(_atDep)->getSome(atLeast, atMost);
    }
    if (res == nullptr) {
      _done = true;
    }
    return res;
  }

  // the non-simple case . . .
  size_t available = 0;  // nr of available rows
  size_t index = 0;      // an index of a non-empty buffer

  // pull more blocks from dependencies . . .
  for (size_t i = 0; i < _dependencies.size(); i++) {
    if (_gatherBlockBuffer.at(i).empty()) {
      if (getBlock(i, atLeast, atMost)) {
        index = i;
        _gatherBlockPos.at(i) = std::make_pair(i, 0);
      }
    } else {
      index = i;
    }

    auto cur = _gatherBlockBuffer.at(i);
    if (!cur.empty()) {
      available += cur.at(0)->size() - _gatherBlockPos.at(i).second;
      for (size_t j = 1; j < cur.size(); j++) {
        available += cur.at(j)->size();
      }
    }
  }

  if (available == 0) {
    _done = true;
    return nullptr;
  }

  size_t toSend = (std::min)(available, atMost);  // nr rows in outgoing block

  // the following is similar to AqlItemBlock's slice method . . .
  std::unordered_map<AqlValue, AqlValue> cache;

  // comparison function
  OurLessThan ourLessThan(_trx, _gatherBlockBuffer, _sortRegisters);
  AqlItemBlock* example = _gatherBlockBuffer.at(index).front();
  size_t nrRegs = example->getNrRegs();

  auto res = std::make_unique<AqlItemBlock>(
      toSend, static_cast<arangodb::aql::RegisterId>(nrRegs));
  // automatically deleted if things go wrong

  for (size_t i = 0; i < toSend; i++) {
    // get the next smallest row from the buffer . . .
    std::pair<size_t, size_t> val = *(std::min_element(
        _gatherBlockPos.begin(), _gatherBlockPos.end(), ourLessThan));

    // copy the row in to the outgoing block . . .
    for (RegisterId col = 0; col < nrRegs; col++) {
      AqlValue const& x(
          _gatherBlockBuffer.at(val.first).front()->getValue(val.second, col));
      if (!x.isEmpty()) {
        auto it = cache.find(x);

        if (it == cache.end()) {
          AqlValue y = x.clone();
          try {
            res->setValue(i, col, y);
          } catch (...) {
            y.destroy();
            throw;
          }
          cache.emplace(x, y);
        } else {
          res->setValue(i, col, it->second);
        }
      }
    }

    // renew the _gatherBlockPos and clean up the buffer if necessary
    _gatherBlockPos.at(val.first).second++;
    if (_gatherBlockPos.at(val.first).second ==
        _gatherBlockBuffer.at(val.first).front()->size()) {
      AqlItemBlock* cur = _gatherBlockBuffer.at(val.first).front();
      delete cur;
      _gatherBlockBuffer.at(val.first).pop_front();
      _gatherBlockPos.at(val.first) = std::make_pair(val.first, 0);
    }
  }

  return res.release();
  LEAVE_BLOCK
}

/// @brief skipSome
size_t GatherBlock::skipSome(size_t atLeast, size_t atMost) {
  ENTER_BLOCK
  if (_done) {
    return 0;
  }

  // the simple case . . .
  if (_isSimple) {
    auto skipped = _dependencies.at(_atDep)->skipSome(atLeast, atMost);
    while (skipped == 0 && _atDep < _dependencies.size() - 1) {
      _atDep++;
      skipped = _dependencies.at(_atDep)->skipSome(atLeast, atMost);
    }
    if (skipped == 0) {
      _done = true;
    }
    return skipped;
  }

  // the non-simple case . . .
  size_t available = 0;  // nr of available rows
  TRI_ASSERT(_dependencies.size() != 0);

  // pull more blocks from dependencies . . .
  for (size_t i = 0; i < _dependencies.size(); i++) {
    if (_gatherBlockBuffer.at(i).empty()) {
      if (getBlock(i, atLeast, atMost)) {
        _gatherBlockPos.at(i) = std::make_pair(i, 0);
      }
    } 

    auto cur = _gatherBlockBuffer.at(i);
    if (!cur.empty()) {
      available += cur.at(0)->size() - _gatherBlockPos.at(i).second;
      for (size_t j = 1; j < cur.size(); j++) {
        available += cur.at(j)->size();
      }
    }
  }

  if (available == 0) {
    _done = true;
    return 0;
  }

  size_t skipped = (std::min)(available, atMost);  // nr rows in outgoing block

  // comparison function
  OurLessThan ourLessThan(_trx, _gatherBlockBuffer, _sortRegisters);

  for (size_t i = 0; i < skipped; i++) {
    // get the next smallest row from the buffer . . .
    std::pair<size_t, size_t> val = *(std::min_element(
        _gatherBlockPos.begin(), _gatherBlockPos.end(), ourLessThan));

    // renew the _gatherBlockPos and clean up the buffer if necessary
    _gatherBlockPos.at(val.first).second++;
    if (_gatherBlockPos.at(val.first).second ==
        _gatherBlockBuffer.at(val.first).front()->size()) {
      AqlItemBlock* cur = _gatherBlockBuffer.at(val.first).front();
      delete cur;
      _gatherBlockBuffer.at(val.first).pop_front();
      _gatherBlockPos.at(val.first) = std::make_pair(val.first, 0);
    }
  }

  return skipped;
  LEAVE_BLOCK
}

/// @brief getBlock: from dependency i into _gatherBlockBuffer.at(i),
/// non-simple case only
bool GatherBlock::getBlock(size_t i, size_t atLeast, size_t atMost) {
  ENTER_BLOCK
  TRI_ASSERT(i < _dependencies.size());
  TRI_ASSERT(!_isSimple);
  AqlItemBlock* docs = _dependencies.at(i)->getSome(atLeast, atMost);
  if (docs != nullptr) {
    try {
      _gatherBlockBuffer.at(i).emplace_back(docs);
    } catch (...) {
      delete docs;
      throw;
    }
    return true;
  }

  return false;
  LEAVE_BLOCK
}

/// @brief OurLessThan: comparison method for elements of _gatherBlockPos
bool GatherBlock::OurLessThan::operator()(std::pair<size_t, size_t> const& a,
                                          std::pair<size_t, size_t> const& b) {
  // nothing in the buffer is maximum!
  if (_gatherBlockBuffer.at(a.first).empty()) {
    return false;
  }
  if (_gatherBlockBuffer.at(b.first).empty()) {
    return true;
  }

  size_t i = 0;
  for (auto const& reg : _sortRegisters) {
    int cmp = AqlValue::Compare(
        _trx,
        _gatherBlockBuffer.at(a.first).front()->getValue(a.second, reg.first),
        _gatherBlockBuffer.at(b.first).front()->getValue(b.second, reg.first), true);

    if (cmp == -1) {
      return reg.second;
    } else if (cmp == 1) {
      return !reg.second;
    }
    i++;
  }

  return false;
}

BlockWithClients::BlockWithClients(ExecutionEngine* engine,
                                   ExecutionNode const* ep,
                                   std::vector<std::string> const& shardIds)
    : ExecutionBlock(engine, ep), _nrClients(shardIds.size()) {
  _shardIdMap.reserve(_nrClients);
  for (size_t i = 0; i < _nrClients; i++) {
    _shardIdMap.emplace(std::make_pair(shardIds[i], i));
  }
}

/// @brief initializeCursor: reset _doneForClient
int BlockWithClients::initializeCursor(AqlItemBlock* items, size_t pos) {
  ENTER_BLOCK

  int res = ExecutionBlock::initializeCursor(items, pos);

  if (res != TRI_ERROR_NO_ERROR) {
    return res;
  }

  _doneForClient.clear();
  _doneForClient.reserve(_nrClients);

  for (size_t i = 0; i < _nrClients; i++) {
    _doneForClient.push_back(false);
  }

  return TRI_ERROR_NO_ERROR;

  LEAVE_BLOCK
}

/// @brief shutdown
int BlockWithClients::shutdown(int errorCode) {
  ENTER_BLOCK

  _doneForClient.clear();

  return ExecutionBlock::shutdown(errorCode);
  LEAVE_BLOCK
}

/// @brief getSomeForShard
AqlItemBlock* BlockWithClients::getSomeForShard(size_t atLeast, size_t atMost,
                                                std::string const& shardId) {
  ENTER_BLOCK
  size_t skipped = 0;
  AqlItemBlock* result = nullptr;

  int out =
      getOrSkipSomeForShard(atLeast, atMost, false, result, skipped, shardId);

  if (out == TRI_ERROR_NO_ERROR) {
    return result;
  }

  if (result != nullptr) {
    delete result;
  }

  THROW_ARANGO_EXCEPTION(out);

  LEAVE_BLOCK
}

/// @brief skipSomeForShard
size_t BlockWithClients::skipSomeForShard(size_t atLeast, size_t atMost,
                                          std::string const& shardId) {
  ENTER_BLOCK
  size_t skipped = 0;
  AqlItemBlock* result = nullptr;
  int out =
      getOrSkipSomeForShard(atLeast, atMost, true, result, skipped, shardId);
  TRI_ASSERT(result == nullptr);
  if (out != TRI_ERROR_NO_ERROR) {
    THROW_ARANGO_EXCEPTION(out);
  }
  return skipped;
  LEAVE_BLOCK
}

/// @brief skipForShard
bool BlockWithClients::skipForShard(size_t number, std::string const& shardId) {
  ENTER_BLOCK
  size_t skipped = skipSomeForShard(number, number, shardId);
  size_t nr = skipped;
  while (nr != 0 && skipped < number) {
    nr = skipSomeForShard(number - skipped, number - skipped, shardId);
    skipped += nr;
  }
  if (nr == 0) {
    return true;
  }
  return !hasMoreForShard(shardId);
  LEAVE_BLOCK
}

/// @brief getClientId: get the number <clientId> (used internally)
/// corresponding to <shardId>
size_t BlockWithClients::getClientId(std::string const& shardId) {
  ENTER_BLOCK
  if (shardId.empty()) {
    THROW_ARANGO_EXCEPTION_MESSAGE(TRI_ERROR_INTERNAL, "got empty shard id");
  }

  auto it = _shardIdMap.find(shardId);
  if (it == _shardIdMap.end()) {
    std::string message("AQL: unknown shard id ");
    message.append(shardId);
    THROW_ARANGO_EXCEPTION_MESSAGE(TRI_ERROR_INTERNAL, message);
  }
  return ((*it).second);
  LEAVE_BLOCK
}

/// @brief initializeCursor
int ScatterBlock::initializeCursor(AqlItemBlock* items, size_t pos) {
  ENTER_BLOCK

  int res = BlockWithClients::initializeCursor(items, pos);
  if (res != TRI_ERROR_NO_ERROR) {
    return res;
  }

  // local clean up
  _posForClient.clear();

  for (size_t i = 0; i < _nrClients; i++) {
    _posForClient.emplace_back(std::make_pair(0, 0));
  }
  return TRI_ERROR_NO_ERROR;
  LEAVE_BLOCK
}

/// @brief initializeCursor
int ScatterBlock::shutdown(int errorCode) {
  ENTER_BLOCK
  int res = BlockWithClients::shutdown(errorCode);
  if (res != TRI_ERROR_NO_ERROR) {
    return res;
  }

  // local clean up
  _posForClient.clear();

  return TRI_ERROR_NO_ERROR;
  LEAVE_BLOCK
}

/// @brief hasMoreForShard: any more for shard <shardId>?
bool ScatterBlock::hasMoreForShard(std::string const& shardId) {
  ENTER_BLOCK
  size_t clientId = getClientId(shardId);

  if (_doneForClient.at(clientId)) {
    return false;
  }

  std::pair<size_t, size_t> pos = _posForClient.at(clientId);
  // (i, j) where i is the position in _buffer, and j is the position in
  // _buffer.at(i) we are sending to <clientId>

  if (pos.first > _buffer.size()) {
    if (!ExecutionBlock::getBlock(DefaultBatchSize(), DefaultBatchSize())) {
      _doneForClient.at(clientId) = true;
      return false;
    }
  }
  return true;
  LEAVE_BLOCK
}

/// @brief remainingForShard: remaining for shard, sum of the number of row left
/// in the buffer and _dependencies[0]->remaining()
int64_t ScatterBlock::remainingForShard(std::string const& shardId) {
  ENTER_BLOCK
  size_t clientId = getClientId(shardId);
  if (_doneForClient.at(clientId)) {
    return 0;
  }

  int64_t sum = _dependencies[0]->remaining();
  if (sum == -1) {
    return -1;
  }

  std::pair<size_t, size_t> pos = _posForClient.at(clientId);

  if (pos.first <= _buffer.size()) {
    sum += _buffer.at(pos.first)->size() - pos.second;
    for (auto i = pos.first + 1; i < _buffer.size(); i++) {
      sum += _buffer.at(i)->size();
    }
  }

  return sum;
  LEAVE_BLOCK
}

/// @brief getOrSkipSomeForShard
int ScatterBlock::getOrSkipSomeForShard(size_t atLeast, size_t atMost,
                                        bool skipping, AqlItemBlock*& result,
                                        size_t& skipped,
                                        std::string const& shardId) {
  ENTER_BLOCK
  TRI_ASSERT(0 < atLeast && atLeast <= atMost);
  TRI_ASSERT(result == nullptr && skipped == 0);

  size_t clientId = getClientId(shardId);

  if (_doneForClient.at(clientId)) {
    return TRI_ERROR_NO_ERROR;
  }

  std::pair<size_t, size_t> pos = _posForClient.at(clientId);

  // pull more blocks from dependency if necessary . . .
  if (pos.first >= _buffer.size()) {
    if (!getBlock(atLeast, atMost)) {
      _doneForClient.at(clientId) = true;
      return TRI_ERROR_NO_ERROR;
    }
  }

  size_t available = _buffer.at(pos.first)->size() - pos.second;
  // available should be non-zero

  skipped = (std::min)(available, atMost);  // nr rows in outgoing block

  if (!skipping) {
    result = _buffer.at(pos.first)->slice(pos.second, pos.second + skipped);
  }

  // increment the position . . .
  _posForClient.at(clientId).second += skipped;

  // check if we're done at current block in buffer . . .
  if (_posForClient.at(clientId).second ==
      _buffer.at(_posForClient.at(clientId).first)->size()) {
    _posForClient.at(clientId).first++;
    _posForClient.at(clientId).second = 0;

    // check if we can pop the front of the buffer . . .
    bool popit = true;
    for (size_t i = 0; i < _nrClients; i++) {
      if (_posForClient.at(i).first == 0) {
        popit = false;
        break;
      }
    }
    if (popit) {
      delete _buffer.front();
      _buffer.pop_front();
      // update the values in first coord of _posForClient
      for (size_t i = 0; i < _nrClients; i++) {
        _posForClient.at(i).first--;
      }
    }
  }

  return TRI_ERROR_NO_ERROR;
  LEAVE_BLOCK
}

DistributeBlock::DistributeBlock(ExecutionEngine* engine,
                                 DistributeNode const* ep,
                                 std::vector<std::string> const& shardIds,
                                 Collection const* collection)
    : BlockWithClients(engine, ep, shardIds),
      _collection(collection),
      _index(0),
      _regId(ExecutionNode::MaxRegisterId),
      _alternativeRegId(ExecutionNode::MaxRegisterId) {
  // get the variable to inspect . . .
  VariableId varId = ep->_varId;

  // get the register id of the variable to inspect . . .
  auto it = ep->getRegisterPlan()->varInfo.find(varId);
  TRI_ASSERT(it != ep->getRegisterPlan()->varInfo.end());
  _regId = (*it).second.registerId;

  TRI_ASSERT(_regId < ExecutionNode::MaxRegisterId);

  if (ep->_alternativeVarId != ep->_varId) {
    // use second variable
    auto it = ep->getRegisterPlan()->varInfo.find(ep->_alternativeVarId);
    TRI_ASSERT(it != ep->getRegisterPlan()->varInfo.end());
    _alternativeRegId = (*it).second.registerId;

    TRI_ASSERT(_alternativeRegId < ExecutionNode::MaxRegisterId);
  }

  _usesDefaultSharding = collection->usesDefaultSharding();
}

/// @brief initializeCursor
int DistributeBlock::initializeCursor(AqlItemBlock* items, size_t pos) {
  ENTER_BLOCK

  int res = BlockWithClients::initializeCursor(items, pos);

  if (res != TRI_ERROR_NO_ERROR) {
    return res;
  }

  // local clean up
  _distBuffer.clear();
  _distBuffer.reserve(_nrClients);

  for (size_t i = 0; i < _nrClients; i++) {
    _distBuffer.emplace_back();
  }

  return TRI_ERROR_NO_ERROR;
  LEAVE_BLOCK
}

/// @brief shutdown
int DistributeBlock::shutdown(int errorCode) {
  ENTER_BLOCK
  int res = BlockWithClients::shutdown(errorCode);
  if (res != TRI_ERROR_NO_ERROR) {
    return res;
  }

  // local clean up
  _distBuffer.clear();

  return TRI_ERROR_NO_ERROR;
  LEAVE_BLOCK
}

/// @brief hasMore: any more for any shard?
bool DistributeBlock::hasMoreForShard(std::string const& shardId) {
  ENTER_BLOCK

  size_t clientId = getClientId(shardId);
  if (_doneForClient.at(clientId)) {
    return false;
  }

  if (!_distBuffer.at(clientId).empty()) {
    return true;
  }

  if (!getBlockForClient(DefaultBatchSize(), DefaultBatchSize(), clientId)) {
    _doneForClient.at(clientId) = true;
    return false;
  }
  return true;
  LEAVE_BLOCK
}

/// @brief getOrSkipSomeForShard
int DistributeBlock::getOrSkipSomeForShard(size_t atLeast, size_t atMost,
                                           bool skipping, AqlItemBlock*& result,
                                           size_t& skipped,
                                           std::string const& shardId) {
  ENTER_BLOCK
  TRI_ASSERT(0 < atLeast && atLeast <= atMost);
  TRI_ASSERT(result == nullptr && skipped == 0);

  size_t clientId = getClientId(shardId);

  if (_doneForClient.at(clientId)) {
    return TRI_ERROR_NO_ERROR;
  }

  std::deque<std::pair<size_t, size_t>>& buf = _distBuffer.at(clientId);

  std::vector<AqlItemBlock*> collector;

  auto freeCollector = [&collector]() {
    for (auto& x : collector) {
      delete x;
    }
    collector.clear();
  };

  try {
    if (buf.empty()) {
      if (!getBlockForClient(atLeast, atMost, clientId)) {
        _doneForClient.at(clientId) = true;
        return TRI_ERROR_NO_ERROR;
      }
    }

    skipped = (std::min)(buf.size(), atMost);

    if (skipping) {
      for (size_t i = 0; i < skipped; i++) {
        buf.pop_front();
      }
      freeCollector();
      return TRI_ERROR_NO_ERROR;
    }

    size_t i = 0;
    while (i < skipped) {
      std::vector<size_t> chosen;
      size_t const n = buf.front().first;
      while (buf.front().first == n && i < skipped) {
        chosen.emplace_back(buf.front().second);
        buf.pop_front();
        i++;

        // make sure we are not overreaching over the end of the buffer
        if (buf.empty()) {
          break;
        }
      }

      std::unique_ptr<AqlItemBlock> more(
          _buffer.at(n)->slice(chosen, 0, chosen.size()));
      collector.emplace_back(more.get());
      more.release();  // do not delete it!
    }
  } catch (...) {
    freeCollector();
    throw;
  }

  if (!skipping) {
    if (collector.size() == 1) {
      result = collector[0];
      collector.clear();
    } else if (!collector.empty()) {
      try {
        result = AqlItemBlock::concatenate(collector);
      } catch (...) {
        freeCollector();
        throw;
      }
    }
  }

  freeCollector();

  // _buffer is left intact, deleted and cleared at shutdown

  return TRI_ERROR_NO_ERROR;
  LEAVE_BLOCK
}

/// @brief getBlockForClient: try to get atLeast pairs into
/// _distBuffer.at(clientId), this means we have to look at every row in the
/// incoming blocks until they run out or we find enough rows for clientId. We
/// also keep track of blocks which should be sent to other clients than the
/// current one.
bool DistributeBlock::getBlockForClient(size_t atLeast, size_t atMost,
                                        size_t clientId) {
  ENTER_BLOCK
  if (_buffer.empty()) {
    _index = 0;  // position in _buffer
    _pos = 0;    // position in _buffer.at(_index)
  }

  std::vector<std::deque<std::pair<size_t, size_t>>>& buf = _distBuffer;
  // it should be the case that buf.at(clientId) is empty

  while (buf.at(clientId).size() < atLeast) {
    if (_index == _buffer.size()) {
      if (!ExecutionBlock::getBlock(atLeast, atMost)) {
        if (buf.at(clientId).size() == 0) {
          _doneForClient.at(clientId) = true;
          return false;
        }
        break;
      }
    }

    AqlItemBlock* cur = _buffer.at(_index);

    while (_pos < cur->size() && buf.at(clientId).size() < atMost) {
      // this may modify the input item buffer in place
      size_t id = sendToClient(cur);

      buf.at(id).emplace_back(std::make_pair(_index, _pos++));
    }

    if (_pos == cur->size()) {
      _pos = 0;
      _index++;
    } else {
      break;
    }
  }

  return true;
  LEAVE_BLOCK
}

/// @brief sendToClient: for each row of the incoming AqlItemBlock use the
/// attributes <shardKeys> of the Aql value <val> to determine to which shard
/// the row should be sent and return its clientId
size_t DistributeBlock::sendToClient(AqlItemBlock* cur) {
  ENTER_BLOCK

  // inspect cur in row _pos and check to which shard it should be sent . .
  AqlValue val = cur->getValueReference(_pos, _regId);

  VPackSlice input = val.slice(); // will throw when wrong type

  if (input.isNull() && _alternativeRegId != ExecutionNode::MaxRegisterId) {
    // value is set, but null
    // check if there is a second input register available (UPSERT makes use of
    // two input registers,
    // one for the search document, the other for the insert document)
    val = cur->getValueReference(_pos, _alternativeRegId);

    input = val.slice(); // will throw when wrong type
  }

  VPackSlice value = input;

  VPackBuilder builder;
  VPackBuilder builder2;

  bool hasCreatedKeyAttribute = false;

  if (input.isString() &&
      static_cast<DistributeNode const*>(_exeNode)
          ->_allowKeyConversionToObject) {
    builder.openObject();
    builder.add(StaticStrings::KeyString, input);
    builder.close();
    
    // clear the previous value
    cur->destroyValue(_pos, _regId);

    // overwrite with new value
    cur->setValue(_pos, _regId, AqlValue(builder));

    value = builder.slice();
    hasCreatedKeyAttribute = true;
  } else if (!input.isObject()) {
    THROW_ARANGO_EXCEPTION(TRI_ERROR_ARANGO_DOCUMENT_TYPE_INVALID);
  }

  TRI_ASSERT(value.isObject());

  if (static_cast<DistributeNode const*>(_exeNode)->_createKeys) {
    // we are responsible for creating keys if none present

    if (_usesDefaultSharding) {
      // the collection is sharded by _key...

      if (!hasCreatedKeyAttribute && !value.hasKey(StaticStrings::KeyString)) {
        // there is no _key attribute present, so we are responsible for
        // creating one
        VPackBuilder temp;
        temp.openObject();
        temp.add(StaticStrings::KeyString, VPackValue(createKey()));
        temp.close();

        builder2 = VPackCollection::merge(input, temp.slice(), true);

        // clear the previous value
        cur->destroyValue(_pos, _regId);

        // overwrite with new value
        cur->setValue(_pos, _regId, AqlValue(builder2));
        value = builder2.slice();
      }
    } else {
      // the collection is not sharded by _key

      if (hasCreatedKeyAttribute || value.hasKey(StaticStrings::KeyString)) {
        // a _key was given, but user is not allowed to specify _key
        THROW_ARANGO_EXCEPTION(TRI_ERROR_CLUSTER_MUST_NOT_SPECIFY_KEY);
      }
        
      VPackBuilder temp;
      temp.openObject();
      temp.add(StaticStrings::KeyString, VPackValue(createKey()));
      temp.close();

      builder2 = VPackCollection::merge(input, temp.slice(), true);

      // clear the previous value
      cur->destroyValue(_pos, _regId);

      // overwrite with new value
      cur->setValue(_pos, _regId, AqlValue(builder2.slice()));
      value = builder2.slice();
    }
  }

  // std::cout << "JSON: " << arangodb::basics::JsonHelper::toString(json) <<
  // "\n";

  std::string shardId;
  bool usesDefaultShardingAttributes;
  auto clusterInfo = arangodb::ClusterInfo::instance();
  auto const planId =
      arangodb::basics::StringUtils::itoa(_collection->getPlanId());

  int res = clusterInfo->getResponsibleShard(planId, value, true, shardId,
                                             usesDefaultShardingAttributes);

  // std::cout << "SHARDID: " << shardId << "\n";

  if (res != TRI_ERROR_NO_ERROR) {
    THROW_ARANGO_EXCEPTION(res);
  }

  TRI_ASSERT(!shardId.empty());

  return getClientId(shardId);
  LEAVE_BLOCK
}

/// @brief create a new document key
std::string DistributeBlock::createKey() const {
  ClusterInfo* ci = ClusterInfo::instance();
  uint64_t uid = ci->uniqid();
  return std::to_string(uid);
}

/// @brief local helper to throw an exception if a HTTP request went wrong
static bool throwExceptionAfterBadSyncRequest(ClusterCommResult* res,
                                              bool isShutdown) {
  ENTER_BLOCK
  if (res->status == CL_COMM_TIMEOUT) {
    std::string errorMessage =
        std::string("Timeout in communication with shard '") +
        std::string(res->shardID) + std::string("' on cluster node '") +
        std::string(res->serverID) + std::string("' failed.");

    // No reply, we give up:
    THROW_ARANGO_EXCEPTION_MESSAGE(TRI_ERROR_CLUSTER_TIMEOUT, errorMessage);
  }

  if (res->status == CL_COMM_BACKEND_UNAVAILABLE) {
    // there is no result
    std::string errorMessage = 
        std::string("Empty result in communication with shard '") +
        std::string(res->shardID) + std::string("' on cluster node '") +
        std::string(res->serverID) + std::string("'");
    THROW_ARANGO_EXCEPTION_MESSAGE(TRI_ERROR_CLUSTER_CONNECTION_LOST,
                                   errorMessage);
  }

  if (res->status == CL_COMM_ERROR) {
    std::string errorMessage;
    TRI_ASSERT(nullptr != res->result);

    StringBuffer const& responseBodyBuf(res->result->getBody());

    // extract error number and message from response
    int errorNum = TRI_ERROR_NO_ERROR;
    TRI_json_t* json =
        TRI_JsonString(TRI_UNKNOWN_MEM_ZONE, responseBodyBuf.c_str());

    if (JsonHelper::getBooleanValue(json, "error", true)) {
      errorNum = TRI_ERROR_INTERNAL;
      errorMessage = std::string("Error message received from shard '") +
                     std::string(res->shardID) +
                     std::string("' on cluster node '") +
                     std::string(res->serverID) + std::string("': ");
    }

    if (TRI_IsObjectJson(json)) {
      TRI_json_t const* v = TRI_LookupObjectJson(json, "errorNum");

      if (TRI_IsNumberJson(v)) {
        if (static_cast<int>(v->_value._number) != TRI_ERROR_NO_ERROR) {
          /* if we've got an error num, error has to be true. */
          TRI_ASSERT(errorNum == TRI_ERROR_INTERNAL);
          errorNum = static_cast<int>(v->_value._number);
        }
      }

      v = TRI_LookupObjectJson(json, "errorMessage");
      if (TRI_IsStringJson(v)) {
        errorMessage +=
            std::string(v->_value._string.data, v->_value._string.length - 1);
      } else {
        errorMessage += std::string("(no valid error in response)");
      }
    } else {
      errorMessage += std::string("(no valid response)");
    }

    if (json != nullptr) {
      TRI_FreeJson(TRI_UNKNOWN_MEM_ZONE, json);
    }

    if (isShutdown && errorNum == TRI_ERROR_QUERY_NOT_FOUND) {
      // this error may happen on shutdown and is thus tolerated
      // pass the info to the caller who can opt to ignore this error
      return true;
    }

    // In this case a proper HTTP error was reported by the DBserver,
    if (errorNum > 0 && !errorMessage.empty()) {
      THROW_ARANGO_EXCEPTION_MESSAGE(errorNum, errorMessage);
    }

    // default error
    THROW_ARANGO_EXCEPTION(TRI_ERROR_CLUSTER_AQL_COMMUNICATION);
  }

  return false;
  LEAVE_BLOCK
}

/// @brief timeout
double const RemoteBlock::defaultTimeOut = 3600.0;

/// @brief creates a remote block
RemoteBlock::RemoteBlock(ExecutionEngine* engine, RemoteNode const* en,
                         std::string const& server, std::string const& ownName,
                         std::string const& queryId)
    : ExecutionBlock(engine, en),
      _server(server),
      _ownName(ownName),
      _queryId(queryId),
      _isResponsibleForInitializeCursor(en->isResponsibleForInitializeCursor()) {
  TRI_ASSERT(!queryId.empty());
  TRI_ASSERT(
      (arangodb::ServerState::instance()->isCoordinator() && ownName.empty()) ||
      (!arangodb::ServerState::instance()->isCoordinator() &&
       !ownName.empty()));
}

RemoteBlock::~RemoteBlock() {}

/// @brief local helper to send a request
std::unique_ptr<ClusterCommResult> RemoteBlock::sendRequest(
    arangodb::GeneralRequest::RequestType type,
    std::string const& urlPart, std::string const& body) const {
  ENTER_BLOCK
  ClusterComm* cc = ClusterComm::instance();

  // Later, we probably want to set these sensibly:
  ClientTransactionID const clientTransactionId = "AQL";
  CoordTransactionID const coordTransactionId = TRI_NewTickServer();
  std::unordered_map<std::string, std::string> headers;
  if (!_ownName.empty()) {
    headers.emplace("Shard-Id", _ownName);
  }

  auto currentThread = arangodb::rest::DispatcherThread::current();

  if (currentThread != nullptr) {
    currentThread->block();
  }

  auto result = cc->syncRequest(
      clientTransactionId, coordTransactionId, _server, type,
      std::string("/_db/") + arangodb::basics::StringUtils::urlEncode(
                                 _engine->getQuery()->trx()->vocbase()->_name) +
          urlPart + _queryId,
      body, headers, defaultTimeOut);

  if (currentThread != nullptr) {
    currentThread->unblock();
  }

  return result;
  LEAVE_BLOCK
}

/// @brief initialize
int RemoteBlock::initialize() {
  ENTER_BLOCK
  
  if (!_isResponsibleForInitializeCursor) {
    // do nothing...
    return TRI_ERROR_NO_ERROR;
  }
 
  std::unique_ptr<ClusterCommResult> res =
      sendRequest(GeneralRequest::RequestType::PUT,
                  "/_api/aql/initialize/", "{}");
  throwExceptionAfterBadSyncRequest(res.get(), false);
 

  // If we get here, then res->result is the response which will be
  // a serialized AqlItemBlock:
  StringBuffer const& responseBodyBuf(res->result->getBody());
  Json responseBodyJson(
      TRI_UNKNOWN_MEM_ZONE,
      TRI_JsonString(TRI_UNKNOWN_MEM_ZONE, responseBodyBuf.begin()));

  return JsonHelper::getNumericValue<int>(responseBodyJson.json(), "code",
                                          TRI_ERROR_INTERNAL);
  LEAVE_BLOCK
}

/// @brief initializeCursor, could be called multiple times
int RemoteBlock::initializeCursor(AqlItemBlock* items, size_t pos) {
  ENTER_BLOCK
  // For every call we simply forward via HTTP

  if (!_isResponsibleForInitializeCursor) {
    // do nothing...
    return TRI_ERROR_NO_ERROR;
  }

  VPackBuilder builder;
  builder.openObject();

  if (items == nullptr) {
    // first call, items is still a nullptr
    builder.add("exhausted", VPackValue(true));
    builder.add("error", VPackValue(false));
  } else {
    builder.add("exhausted", VPackValue(false));
    builder.add("error", VPackValue(false));
    builder.add("pos", VPackValue(pos));
    builder.add(VPackValue("items"));
    builder.openObject();
    items->toVelocyPack(_engine->getQuery()->trx(), builder);
    builder.close();
  }

  builder.close();

  std::string bodyString(builder.slice().toJson());

  std::unique_ptr<ClusterCommResult> res =
      sendRequest(GeneralRequest::RequestType::PUT,
                  "/_api/aql/initializeCursor/", bodyString);
  throwExceptionAfterBadSyncRequest(res.get(), false);

  // If we get here, then res->result is the response which will be
  // a serialized AqlItemBlock:
  StringBuffer const& responseBodyBuf(res->result->getBody());
  Json responseBodyJson(
      TRI_UNKNOWN_MEM_ZONE,
      TRI_JsonString(TRI_UNKNOWN_MEM_ZONE, responseBodyBuf.begin()));
  return JsonHelper::getNumericValue<int>(responseBodyJson.json(), "code",
                                          TRI_ERROR_INTERNAL);
  LEAVE_BLOCK
}

/// @brief shutdown, will be called exactly once for the whole query
int RemoteBlock::shutdown(int errorCode) {
  ENTER_BLOCK

  if (!_isResponsibleForInitializeCursor) {
    // do nothing...
    return TRI_ERROR_NO_ERROR;
  }

  // For every call we simply forward via HTTP

  std::unique_ptr<ClusterCommResult> res =
      sendRequest(GeneralRequest::RequestType::PUT, "/_api/aql/shutdown/",
                  std::string("{\"code\":" + std::to_string(errorCode) + "}"));
  if (throwExceptionAfterBadSyncRequest(res.get(), true)) {
    // artificially ignore error in case query was not found during shutdown
    return TRI_ERROR_NO_ERROR;
  }

  StringBuffer const& responseBodyBuf(res->result->getBody());
  Json responseBodyJson(
      TRI_UNKNOWN_MEM_ZONE,
      TRI_JsonString(TRI_UNKNOWN_MEM_ZONE, responseBodyBuf.begin()));

  // read "warnings" attribute if present and add it our query
  if (responseBodyJson.isObject()) {
    auto warnings = responseBodyJson.get("warnings");
    if (warnings.isArray()) {
      auto query = _engine->getQuery();
      for (size_t i = 0; i < warnings.size(); ++i) {
        auto warning = warnings.at(i);
        if (warning.isObject()) {
          auto code = warning.get("code");
          auto message = warning.get("message");
          if (code.isNumber() && message.isString()) {
            query->registerWarning(
                static_cast<int>(code.json()->_value._number),
                message.json()->_value._string.data);
          }
        }
      }
    }
  }

  return JsonHelper::getNumericValue<int>(responseBodyJson.json(), "code",
                                          TRI_ERROR_INTERNAL);
  LEAVE_BLOCK
}

/// @brief getSome
AqlItemBlock* RemoteBlock::getSome(size_t atLeast, size_t atMost) {
  ENTER_BLOCK
  // For every call we simply forward via HTTP

  Json body(Json::Object, 2);
  body("atLeast", Json(static_cast<double>(atLeast)))(
      "atMost", Json(static_cast<double>(atMost)));
  std::string bodyString(body.toString());

  std::unique_ptr<ClusterCommResult> res = sendRequest(
      GeneralRequest::RequestType::PUT, "/_api/aql/getSome/", bodyString);
  throwExceptionAfterBadSyncRequest(res.get(), false);

  // If we get here, then res->result is the response which will be
  // a serialized AqlItemBlock:
  std::shared_ptr<VPackBuilder> responseBodyBuilder = res->result->getBodyVelocyPack();
  VPackSlice responseBody = responseBodyBuilder->slice();

  ExecutionStats newStats(responseBody.get("stats"));

  _engine->_stats.addDelta(_deltaStats, newStats);
  _deltaStats = newStats;

  if (VelocyPackHelper::getBooleanValue(responseBody, "exhausted", true)) {
    return nullptr;
  }

  return new arangodb::aql::AqlItemBlock(responseBody);
  LEAVE_BLOCK
}

/// @brief skipSome
size_t RemoteBlock::skipSome(size_t atLeast, size_t atMost) {
  ENTER_BLOCK
  // For every call we simply forward via HTTP

  Json body(Json::Object, 2);
  body("atLeast", Json(static_cast<double>(atLeast)))(
      "atMost", Json(static_cast<double>(atMost)));
  std::string bodyString(body.toString());

  std::unique_ptr<ClusterCommResult> res = sendRequest(
      GeneralRequest::RequestType::PUT, "/_api/aql/skipSome/", bodyString);
  throwExceptionAfterBadSyncRequest(res.get(), false);

  // If we get here, then res->result is the response which will be
  // a serialized AqlItemBlock:
  StringBuffer const& responseBodyBuf(res->result->getBody());
  Json responseBodyJson(
      TRI_UNKNOWN_MEM_ZONE,
      TRI_JsonString(TRI_UNKNOWN_MEM_ZONE, responseBodyBuf.begin()));
  if (JsonHelper::getBooleanValue(responseBodyJson.json(), "error", true)) {
    THROW_ARANGO_EXCEPTION(TRI_ERROR_CLUSTER_AQL_COMMUNICATION);
  }
  size_t skipped = JsonHelper::getNumericValue<size_t>(responseBodyJson.json(),
                                                       "skipped", 0);
  return skipped;
  LEAVE_BLOCK
}

/// @brief hasMore
bool RemoteBlock::hasMore() {
  ENTER_BLOCK
  // For every call we simply forward via HTTP
  std::unique_ptr<ClusterCommResult> res = sendRequest(
      GeneralRequest::RequestType::GET, "/_api/aql/hasMore/", std::string());
  throwExceptionAfterBadSyncRequest(res.get(), false);

  // If we get here, then res->result is the response which will be
  // a serialized AqlItemBlock:
  StringBuffer const& responseBodyBuf(res->result->getBody());
  Json responseBodyJson(
      TRI_UNKNOWN_MEM_ZONE,
      TRI_JsonString(TRI_UNKNOWN_MEM_ZONE, responseBodyBuf.begin()));
  if (JsonHelper::getBooleanValue(responseBodyJson.json(), "error", true)) {
    THROW_ARANGO_EXCEPTION(TRI_ERROR_CLUSTER_AQL_COMMUNICATION);
  }
  return JsonHelper::getBooleanValue(responseBodyJson.json(), "hasMore", true);
  LEAVE_BLOCK
}

/// @brief count
int64_t RemoteBlock::count() const {
  ENTER_BLOCK
  // For every call we simply forward via HTTP
  std::unique_ptr<ClusterCommResult> res = sendRequest(
      GeneralRequest::RequestType::GET, "/_api/aql/count/", std::string());
  throwExceptionAfterBadSyncRequest(res.get(), false);

  // If we get here, then res->result is the response which will be
  // a serialized AqlItemBlock:
  StringBuffer const& responseBodyBuf(res->result->getBody());
  Json responseBodyJson(
      TRI_UNKNOWN_MEM_ZONE,
      TRI_JsonString(TRI_UNKNOWN_MEM_ZONE, responseBodyBuf.begin()));
  if (JsonHelper::getBooleanValue(responseBodyJson.json(), "error", true)) {
    THROW_ARANGO_EXCEPTION(TRI_ERROR_CLUSTER_AQL_COMMUNICATION);
  }
  return JsonHelper::getNumericValue<int64_t>(responseBodyJson.json(), "count",
                                              0);
  LEAVE_BLOCK
}

/// @brief remaining
int64_t RemoteBlock::remaining() {
  ENTER_BLOCK
  // For every call we simply forward via HTTP
  std::unique_ptr<ClusterCommResult> res =
      sendRequest(GeneralRequest::RequestType::GET, "/_api/aql/remaining/",
                  std::string());
  throwExceptionAfterBadSyncRequest(res.get(), false);

  // If we get here, then res->result is the response which will be
  // a serialized AqlItemBlock:
  StringBuffer const& responseBodyBuf(res->result->getBody());
  Json responseBodyJson(
      TRI_UNKNOWN_MEM_ZONE,
      TRI_JsonString(TRI_UNKNOWN_MEM_ZONE, responseBodyBuf.begin()));
  if (JsonHelper::getBooleanValue(responseBodyJson.json(), "error", true)) {
    THROW_ARANGO_EXCEPTION(TRI_ERROR_CLUSTER_AQL_COMMUNICATION);
  }
  return JsonHelper::getNumericValue<int64_t>(responseBodyJson.json(),
                                              "remaining", 0);
  LEAVE_BLOCK
}
