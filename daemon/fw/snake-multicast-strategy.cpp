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

#include "snake-multicast-strategy.hpp"
#include "multicast-strategy.hpp"
#include "algorithm.hpp"
#include <ndn-cxx/lp/pit-token.hpp>
#include "common/logger.hpp"

namespace nfd {
namespace fw {

NFD_REGISTER_STRATEGY(SnakeMulticastStrategy);

NFD_LOG_INIT(SnakeMulticastStrategy);


SnakeMulticastStrategy::SnakeMulticastStrategy(Forwarder& forwarder, const Name& name)
  : MulticastStrategy(forwarder, name)
{
  ParsedInstanceName parsed = parseInstanceName(name);
  if (!parsed.parameters.empty()) {
    NDN_THROW(std::invalid_argument("SnakeMulticastStrategy does not accept parameters"));
  }
  if (parsed.version && *parsed.version != getStrategyName()[-1].toVersion()) {
    NDN_THROW(std::invalid_argument(
      "SnakeMulticastStrategy does not support version " + to_string(*parsed.version)));
  }
  this->setInstanceName(makeInstanceName(name, getStrategyName()));
}

const Name&
SnakeMulticastStrategy::getStrategyName()
{
  static Name strategyName("/localhost/nfd/strategy/snake-multicast/%FD%03");
  return strategyName;
}

void
SnakeMulticastStrategy::afterReceiveInterest(const FaceEndpoint& ingress, const Interest& interest,
                                        const shared_ptr<pit::Entry>& pitEntry)
{
  MulticastStrategy::afterReceiveInterest(ingress, interest, pitEntry);
}

void
SnakeMulticastStrategy::afterReceiveNack(const FaceEndpoint& ingress, const lp::Nack& nack,
                                    const shared_ptr<pit::Entry>& pitEntry)
{
  MulticastStrategy::afterReceiveNack(ingress, nack, pitEntry);
}

void
SnakeMulticastStrategy::beforeSatisfyInterest(const shared_ptr<pit::Entry>& pitEntry,
                        const FaceEndpoint& ingress, const Data& data)
{
  MulticastStrategy::beforeSatisfyInterest(pitEntry, ingress, data);
  //TODO find a way to calculate the data.

}


} // namespace fw
} // namespace nfd
