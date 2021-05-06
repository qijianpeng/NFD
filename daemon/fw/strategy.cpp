/* -*- Mode:C++; c-file-style:"gnu"; indent-tabs-mode:nil; -*- */
/*
 * Copyright (c) 2014-2019,  Regents of the University of California,
 *                           Arizona Board of Regents,
 *                           Colorado State University,
 *                           University Pierre & Marie Curie, Sorbonne University,
 *                           Washington University in St. Louis,
 *                           Beijing Institute of Technology,
 *                           The University of Memphis.
 *
 * This file is part of NFD (Named Data Networking Forwarding Daemon).
 * See AUTHORS.md for complete list of NFD authors and contributors.
 *
 * NFD is free software: you can redistribute it and/or modify it under the terms
 * of the GNU General Public License as published by the Free Software Foundation,
 * either version 3 of the License, or (at your option) any later version.
 *
 * NFD is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY;
 * without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR
 * PURPOSE.  See the GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along with
 * NFD, e.g., in COPYING.md file.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "strategy.hpp"
#include "forwarder.hpp"
#include "common/logger.hpp"

#include <ndn-cxx/lp/pit-token.hpp>

#include <boost/range/adaptor/map.hpp>
#include <boost/range/algorithm/copy.hpp>
#include <climits>
#include <ndn-cxx/util/snake-utils.hpp>
#include <ndn-cxx/lp/tags.hpp>
#include "ns3/node-list.h"
#include "ns3/node.h"
#include "ns3/ptr.h"
#include "ns3/computation-module.h"
#include "ns3/core-module.h"
#include "ns3/simulator.h"//< Just for simulation.

namespace nfd {
namespace fw {
namespace snake_util = ::ndn::snake::util;

NFD_LOG_INIT(Strategy);

Strategy::Registry&
Strategy::getRegistry()
{
  static Registry registry;
  return registry;
}

Strategy::Registry::const_iterator
Strategy::find(const Name& instanceName)
{
  const Registry& registry = getRegistry();
  ParsedInstanceName parsed = parseInstanceName(instanceName);

  if (parsed.version) {
    // specified version: find exact or next higher version

    auto found = registry.lower_bound(parsed.strategyName);
    if (found != registry.end()) {
      if (parsed.strategyName.getPrefix(-1).isPrefixOf(found->first)) {
        NFD_LOG_TRACE("find " << instanceName << " versioned found=" << found->first);
        return found;
      }
    }

    NFD_LOG_TRACE("find " << instanceName << " versioned not-found");
    return registry.end();
  }

  // no version specified: find highest version

  if (!parsed.strategyName.empty()) { // Name().getSuccessor() would be invalid
    auto found = registry.lower_bound(parsed.strategyName.getSuccessor());
    if (found != registry.begin()) {
      --found;
      if (parsed.strategyName.isPrefixOf(found->first)) {
        NFD_LOG_TRACE("find " << instanceName << " unversioned found=" << found->first);
        return found;
      }
    }
  }

  NFD_LOG_TRACE("find " << instanceName << " unversioned not-found");
  return registry.end();
}

bool
Strategy::canCreate(const Name& instanceName)
{
  return Strategy::find(instanceName) != getRegistry().end();
}

unique_ptr<Strategy>
Strategy::create(const Name& instanceName, Forwarder& forwarder)
{
  auto found = Strategy::find(instanceName);
  if (found == getRegistry().end()) {
    NFD_LOG_DEBUG("create " << instanceName << " not-found");
    return nullptr;
  }

  unique_ptr<Strategy> instance = found->second(forwarder, instanceName);
  NFD_LOG_DEBUG("create " << instanceName << " found=" << found->first
                << " created=" << instance->getInstanceName());
  BOOST_ASSERT(!instance->getInstanceName().empty());
  return instance;
}

bool
Strategy::areSameType(const Name& instanceNameA, const Name& instanceNameB)
{
  return Strategy::find(instanceNameA) == Strategy::find(instanceNameB);
}

std::set<Name>
Strategy::listRegistered()
{
  std::set<Name> strategyNames;
  boost::copy(getRegistry() | boost::adaptors::map_keys,
              std::inserter(strategyNames, strategyNames.end()));
  return strategyNames;
}

Strategy::ParsedInstanceName
Strategy::parseInstanceName(const Name& input)
{
  for (ssize_t i = input.size() - 1; i > 0; --i) {
    if (input[i].isVersion()) {
      return {input.getPrefix(i + 1), input[i].toVersion(), input.getSubName(i + 1)};
    }
  }
  return {input, nullopt, PartialName()};
}

Name
Strategy::makeInstanceName(const Name& input, const Name& strategyName)
{
  BOOST_ASSERT(strategyName.at(-1).isVersion());

  bool hasVersion = std::any_of(input.rbegin(), input.rend(),
                                [] (const name::Component& comp) { return comp.isVersion(); });
  return hasVersion ? input : Name(input).append(strategyName.at(-1));
}

Strategy::Strategy(Forwarder& forwarder)
  : afterAddFace(forwarder.m_faceTable.afterAdd)
  , beforeRemoveFace(forwarder.m_faceTable.beforeRemove)
  , m_forwarder(forwarder)
  , m_measurements(m_forwarder.getMeasurements(), m_forwarder.getStrategyChoice(), *this)
{
}

Strategy::~Strategy() = default;


void
Strategy::afterReceiveLoopedInterest(const FaceEndpoint& ingress, const Interest& interest,
                                     pit::Entry& pitEntry)
{
  NFD_LOG_DEBUG("afterReceiveLoopedInterest pitEntry=" << pitEntry.getName()
                << " in=" << ingress);
}

void
Strategy::beforeSatisfyInterest(const shared_ptr<pit::Entry>& pitEntry,
                                const FaceEndpoint& ingress, const Data& data)
{
  NFD_LOG_DEBUG("beforeSatisfyInterest pitEntry=" << pitEntry->getName()
                << " in=" << ingress << " data=" << data.getName());
}

void
Strategy::afterContentStoreHit(const shared_ptr<pit::Entry>& pitEntry,
                               const FaceEndpoint& ingress, const Data& data)
{
  NFD_LOG_DEBUG("afterContentStoreHit pitEntry=" << pitEntry->getName()
                << " in=" << ingress << " data=" << data.getName());

  this->sendData(pitEntry, data, ingress);
}

void
Strategy::afterReceiveData(const shared_ptr<pit::Entry>& pitEntry,
                           const FaceEndpoint& ingress, const Data& data)
{
  NFD_LOG_DEBUG("afterReceiveData pitEntry=" << pitEntry->getName()
                << " in=" << ingress << " data=" << data.getName());

  this->beforeSatisfyInterest(pitEntry, ingress, data);

  this->sendDataToAll(pitEntry, ingress, data);
}

void
Strategy::afterReceiveNack(const FaceEndpoint& ingress, const lp::Nack& nack,
                           const shared_ptr<pit::Entry>& pitEntry)
{
  NFD_LOG_DEBUG("afterReceiveNack in=" << ingress << " pitEntry=" << pitEntry->getName());
}

void
Strategy::onDroppedInterest(const FaceEndpoint& egress, const Interest& interest)
{
  NFD_LOG_DEBUG("onDroppedInterest out=" << egress << " name=" << interest.getName());
}

void
Strategy::sendInterest(const shared_ptr<pit::Entry>& pitEntry,
                       const FaceEndpoint& egress, const Interest& interest)
{
  if (interest.getTag<lp::PitToken>() != nullptr) {
    Interest interest2 = interest; // make a copy to preserve tag on original packet
    interest2.removeTag<lp::PitToken>();
    m_forwarder.onOutgoingInterest(pitEntry, egress, interest2);
    return;
  }
  m_forwarder.onOutgoingInterest(pitEntry, egress, interest);
}

void
Strategy::afterNewNextHop(const fib::NextHop& nextHop, const shared_ptr<pit::Entry>& pitEntry)
{
  NFD_LOG_DEBUG("afterNewNextHop pitEntry=" << pitEntry->getName()
                << " nexthop=" << nextHop.getFace().getId());
}
uint64_t functionInvoke(ns3::Ptr<ns3::Node> &node, Data& data, const std::string functionName, const std::string functionParasJSONStr)
{
  return snake_util::functionInvoke(node, data, functionName, functionParasJSONStr);
}
std::string extractFunctionName(Data & data)
{
  std::string uri = data.getName().toUri();
  uri = snake_util::unescape(uri);
  return snake_util::extractFunctionName(uri);
}
std::string extractFunctionParas(const Interest & interest)
{
  Block block = interest.getApplicationParameters();
  return ndn::encoding::readString(block);
}
void
Strategy::sendData(const shared_ptr<pit::Entry>& pitEntry, const Data& data,
                   const FaceEndpoint& egress)
{
  BOOST_ASSERT(pitEntry->getInterest().matchesData(data));

  shared_ptr<lp::PitToken> pitToken;
  auto inRecord = pitEntry->getInRecord(egress.face);
  if (inRecord != pitEntry->in_end()) {
    pitToken = inRecord->getInterest().getTag<lp::PitToken>();
  }

  // delete the PIT entry's in-record based on egress,
  // since Data is sent to face and endpoint from which the Interest was received
  pitEntry->deleteExpiredOrNonLongLivedInRecords(time::steady_clock::now());
  if(!pitEntry->hasNonExpiredLongLivedInRecord(time::steady_clock::now())) {
    pitEntry->deleteInRecord(egress.face);
    NFD_LOG_DEBUG(">>>Delete PitEntry InRecord: " << pitEntry->getInterest() << egress);
  }
  // auto copiedData = snake_util::cloneData(data);

  if (pitToken != nullptr) {
    Data data2 = data; // make a copy so each downstream can get a different PIT token
    data2.setTag(pitToken);
    m_forwarder.onOutgoingData(data2, egress);
    return;
  }
  m_forwarder.onOutgoingData(data, egress);
}

void
Strategy::sendDataToAll(const shared_ptr<pit::Entry>& pitEntry,
                        const FaceEndpoint& ingress, const Data& data)
{
  std::set<Face*> pendingDownstreams;
  auto now = time::steady_clock::now();

  // remember pending downstreams
  for (const pit::InRecord& inRecord : pitEntry->getInRecords()) {
    if (inRecord.getExpiry() > now) {
      if (inRecord.getFace().getId() == ingress.face.getId() &&
          inRecord.getFace().getLinkType() != ndn::nfd::LINK_TYPE_AD_HOC) {
        continue;
      }
      pendingDownstreams.emplace(&inRecord.getFace());
    }
  }

  for (const auto& pendingDownstream : pendingDownstreams) {
    this->sendData(pitEntry, data, FaceEndpoint(*pendingDownstream, 0));
  }
}


void
Strategy::sendNacks(const shared_ptr<pit::Entry>& pitEntry, const lp::NackHeader& header,
                    std::initializer_list<FaceEndpoint> exceptFaceEndpoints)
{
  // populate downstreams with all downstreams faces
  std::set<Face*> downstreams;
  std::transform(pitEntry->in_begin(), pitEntry->in_end(), std::inserter(downstreams, downstreams.end()),
                 [] (const pit::InRecord& inR) {
                  return &inR.getFace();
                 });

  // delete excluded faces
  for (const auto& exceptFaceEndpoint : exceptFaceEndpoints) {
    downstreams.erase(&exceptFaceEndpoint.face);
  }

  // send Nacks
  for (const auto& downstream : downstreams) {
    this->sendNack(pitEntry, FaceEndpoint(*downstream, 0), header);
  }
  // warning: don't loop on pitEntry->getInRecords(), because in-record is deleted when sending Nack
}

const fib::Entry&
Strategy::lookupFib(const pit::Entry& pitEntry) const
{
  const Fib& fib = m_forwarder.getFib();

  const Interest& interest = pitEntry.getInterest();
  // has forwarding hint?
  if (interest.getForwardingHint().empty()) {
    // FIB lookup with Interest name
    const fib::Entry& fibEntry = fib.findLongestPrefixMatch(pitEntry);
    NFD_LOG_TRACE("lookupFib noForwardingHint found=" << fibEntry.getPrefix());
    return fibEntry;
  }

  const DelegationList& fh = interest.getForwardingHint();
  // Forwarding hint should have been stripped by incoming Interest pipeline when reaching producer region
  BOOST_ASSERT(!m_forwarder.getNetworkRegionTable().isInProducerRegion(fh));

  const fib::Entry* fibEntry = nullptr;
  for (const Delegation& del : fh) {
    fibEntry = &fib.findLongestPrefixMatch(del.name);
    if (fibEntry->hasNextHops()) {
      if (fibEntry->getPrefix().size() == 0) {
        // in consumer region, return the default route
        NFD_LOG_TRACE("lookupFib inConsumerRegion found=" << fibEntry->getPrefix());
      }
      else {
        // in default-free zone, use the first delegation that finds a FIB entry
        NFD_LOG_TRACE("lookupFib delegation=" << del.name << " found=" << fibEntry->getPrefix());
      }
      return *fibEntry;
    }
    BOOST_ASSERT(fibEntry->getPrefix().size() == 0); // only ndn:/ FIB entry can have zero nexthop
  }
  BOOST_ASSERT(fibEntry != nullptr && fibEntry->getPrefix().size() == 0);
  return *fibEntry; // only occurs if no delegation finds a FIB nexthop
}

// Trace route
void
Strategy::notfound(const FaceEndpoint& ingress, const shared_ptr<pit::Entry>& pitEntry, const Interest& interest){           
  // notfound simply stays put
  NFD_LOG_DEBUG("Cs lookup negative, procees to forwarding based on S or M");
  int k = (-1)*interest.getName().size();    
  const ndn::Name name = GetLookupName(interest);              
  // if it's a multipath interest check               
  if (interest.getName().at(k+1).toUri()=="S"){
    S_ForwardIt(ingress, interest, pitEntry, name);
  }else{
    M_ForwardIt(ingress, interest, pitEntry, name);
  }
}

void
Strategy::found(const FaceEndpoint& ingress, const shared_ptr<pit::Entry>& pitEntry, const Interest& interest, const Data& data){
        
  NFD_LOG_DEBUG("Traced name is cached");

  lp::NackHeader nackHeader;
  nackHeader.setReason(lp::NackReason::CACHE_LOCAL);

  this->sendNack(pitEntry, ingress, nackHeader);
  this->rejectPendingInterest(pitEntry);          
}

void 
Strategy::Cache_check(const FaceEndpoint& ingress, const shared_ptr<pit::Entry>& pitEntry, const ndn::Name name){
  // P2 = c
  NFD_LOG_DEBUG("Checking the content store ");                    
  // Access to the Forwarder's FIB
  const Cs& cs = m_forwarder.getCs();                    
  //Cs lookup with name                    
  NFD_LOG_DEBUG("Creating a fake interest for the cs_lookup");                        
  ndn::Interest fi;
  fi.setName(name);
  fi.setInterestLifetime(ndn::time::milliseconds(10000));                    
  cs.find(fi, bind(&Strategy::found, this, ingress, pitEntry, _1, _2),
  bind(&Strategy::notfound, this, ingress, pitEntry, _1));  
}   


void
Strategy::M_ForwardIt(const FaceEndpoint& ingress, const Interest& interest, const shared_ptr<pit::Entry>& pitEntry, const ndn::Name name){
  const Fib& fib = m_forwarder.getFib();                    
  // FIB lookup with name
  const fib::Entry& fibEntry = fib.findLongestPrefixMatch(name);                                       
  // Getting faces for nexthops??
  std::string face_id = interest.getName().at(-1).toUri(); // Regular multipath request >> has face_id in the end
  const fib::NextHopList& nexthops = fibEntry.getNextHops();                    
  for (fib::NextHopList::const_iterator it = nexthops.begin(); it != nexthops.end(); ++it) {                        
    if(it->getFace().getId()==std::stoi(face_id)){                            
      if((it->getFace().getScope()== ndn::nfd::FACE_SCOPE_LOCAL)){                             
        lp::NackHeader nackHeader;
        nackHeader.setReason(lp::NackReason::PRODUCER_LOCAL);                                                            
        this->sendNack(pitEntry, ingress, nackHeader);
        this->rejectPendingInterest(pitEntry);
        return;                                
      }else{                                
        this->sendInterest(pitEntry, FaceEndpoint(it->getFace(), 0), interest);
        return;
      }
    }
  }                                        
  return;	
}
void       
Strategy::multi_process(const FaceEndpoint& ingress, const Interest& interest, const shared_ptr<pit::Entry>& pitEntry){
  NFD_LOG_DEBUG("Received a multipath interest");                    
  //Constructing name                    
  int i;
  int k = (-1)*interest.getName().size();  
  const ndn::Name c = interest.getName().at(k+3).toUri();
  const ndn::Name n = c.toUri();                    
  // actual multipath
  std::string face_id = interest.getName().at(-1).toUri(); // Regular multipath request >> has face_id in the end
  ndn::Name v = n.toUri();                    
  for(i=k+4; i< -2; i++){                        
      v = v.toUri() + "/" + interest.getName().at(i).toUri();                        
  }                
  
  const ndn::Name name = v.toUri();                                                          
  if (interest.getName().at(k+2).toUri()== "c"){                    
      Cache_check(ingress, pitEntry, name);               
  }else{
    M_ForwardIt(ingress, interest, pitEntry, name);
  }  
}

void
Strategy::S_ForwardIt(const FaceEndpoint& ingress, const Interest& interest, const shared_ptr<pit::Entry>& pitEntry, const ndn::Name name){
  NFD_LOG_DEBUG("Entering S_ForwardIt with name: " << name);
  // Access to the Forwarder's FIB
  const Fib& fib = m_forwarder.getFib();
  // FIB lookup with name
  const fib::Entry& fibEntry = fib.findLongestPrefixMatch(name);
  // const fib::Entry& fibEntry = this->lookupFib(*pitEntry);
  // Getting faces for nexthops??
  const fib::NextHopList& nexthops = fibEntry.getNextHops();
  /// This part is for sending
  for (fib::NextHopList::const_iterator it = nexthops.begin(); it != nexthops.end(); ++it) {
    NFD_LOG_DEBUG("S_ForwardIt: faceid: " << it->getFace().getId() << ", local-uri: " 
                  << it->getFace().getLocalUri().toString() << ", remote-uri: " << it->getFace().getRemoteUri().toString());
    // if (it->getFace().getScope()== ndn::nfd::FACE_SCOPE_LOCAL){
    //   lp::NackHeader nackHeader;
    //   nackHeader.setReason(lp::NackReason::PRODUCER_LOCAL);
    //   this->sendNack(pitEntry, ingress, nackHeader);
    //   this->rejectPendingInterest(pitEntry);
    //   return;
    // }else{
      this->sendInterest(pitEntry, FaceEndpoint(it->getFace(), 0), interest);
    // }
    return;
  }
}

  
const ndn::Name
Strategy::GetLookupName(const Interest& interest){                 
  int i;
  int k = (-1)*interest.getName().size();
  const ndn::Name c = interest.getName().at(k+3).toUri();
  const ndn::Name n = c.toUri() ;
  //const ndn::Name x = n.toUri() + c.toUri
  ndn::Name v = n.toUri();
  for(i=k+4; i< -1; i++){
    v = v.toUri() + "/" + interest.getName().at(i).toUri();
  }
  return v.toUri();
}
            
void
Strategy::single_process(const FaceEndpoint& ingress, const Interest& interest, const shared_ptr<pit::Entry>& pitEntry) {        	                  
  NFD_LOG_DEBUG("**************Single path option****************");
  // Extract name from interest
  const ndn::Name name = GetLookupName(interest);
  int k = (-1)*interest.getName().size();
  if (interest.getName().at(k+2).toUri()== "c"){
    Cache_check(ingress, pitEntry, name);
  }else{
    S_ForwardIt(ingress, interest, pitEntry, name);
  } // end of option p2
}// End of if S

void 
Strategy::Trace(const FaceEndpoint& ingress, const Interest& interest, const shared_ptr<pit::Entry>& pitEntry){
  
  std::size_t found3 = interest.getName().toUri().find("Key-TID");
  std::size_t found2 = interest.getName().toUri().find("/Trace");
  NFD_LOG_DEBUG("Entering Trace pipeline. found Key-TID: " << found3 << ", found /Trace: " << found2
                << ", Ingress: " << ingress.face.getScope() << ", Name: "<< interest.getName().toUri());
  if ((found2!=std::string::npos)&&(found3!=std::string::npos)&&(ingress.face.getScope() == ndn::nfd::FACE_SCOPE_LOCAL)){
    int k = (-1)*interest.getName().size();                
    // if it's a multipath interest check               
    if (interest.getName().at(k+1).toUri()=="M"){
      multi_process(ingress, interest, pitEntry);
    }
    if (interest.getName().at(k+1).toUri()=="S"){
      NFD_LOG_DEBUG("Entering single_process pipeline.");
      single_process(ingress, interest, pitEntry);
    }
  }
}
} // namespace fw
} // namespace nfd
