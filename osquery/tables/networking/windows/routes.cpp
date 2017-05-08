/*
*  Copyright (c) 2014-present, Facebook, Inc.
*  All rights reserved.
*
*  This source code is licensed under the BSD-style license found in the
*  LICENSE file in the root directory of this source tree. An additional grant
*  of patent rights can be found in the PATENTS file in the same directory.
*
*/

#define WIN32_LEAN_AND_MEAN

#include <memory>
#include <string>
#include <windows.h>
#include <winsock2.h>

#include <wS2tcpip.h>
#include <ws2ipdef.h>

#include <iphlpapi.h>
#include <mstcpip.h>

#include <boost/algorithm/string/join.hpp>
#include <osquery/logger.h>
#include <osquery/tables.h>

#include "osquery/core/conversions.h"
#include "osquery/core/windows/wmi.h"

namespace osquery {
namespace tables {

std::map<DWORD, IP_ADAPTER_INFO> getAdapterAddressMapping() {
  std::map<DWORD, IP_ADAPTER_INFO> returnMapping;
  auto dwBufLen = 0UL;
  auto dwStatus = GetAdaptersInfo(NULL, &dwBufLen);

  if (dwStatus != ERROR_BUFFER_OVERFLOW) {
    return returnMapping;
  }

  std::vector<BYTE> buffer(dwBufLen);
  auto pAdapterInfo = reinterpret_cast<PIP_ADAPTER_INFO>(buffer.data());
  dwStatus = GetAdaptersInfo(pAdapterInfo, &dwBufLen);

  if (dwStatus != NO_ERROR) {
    return returnMapping;
  }

  while (pAdapterInfo != nullptr) {
    const auto& adapter = *pAdapterInfo;
    const auto& index = pAdapterInfo->Index;
    returnMapping.insert(std::make_pair(index, adapter));
    pAdapterInfo = pAdapterInfo->Next;
  }

  buffer.clear();

  return returnMapping;
}

std::map<unsigned long, MIB_IPINTERFACE_ROW> getInterfaceRowMapping(
    int type = AF_UNSPEC) {
  std::map<unsigned long, MIB_IPINTERFACE_ROW> returnMapping;
  PMIB_IPINTERFACE_TABLE interfaces;
  auto dwRetVal = GetIpInterfaceTable(type, &interfaces);

  if (dwRetVal != NO_ERROR) {
    return returnMapping;
  }

  for (unsigned long i = 0; i < interfaces->NumEntries; ++i) {
    MIB_IPINTERFACE_ROW currentRow = interfaces->Table[i];
    returnMapping.insert(std::make_pair(currentRow.InterfaceIndex, currentRow));
  }

  FreeMibTable(interfaces);

  return returnMapping;
}

QueryData genRoutes(QueryContext& context) {
  QueryData results;
  PMIB_IPFORWARD_TABLE2 ipTable = nullptr;
  auto result = GetIpForwardTable2(AF_UNSPEC, &ipTable);

  if (result != NO_ERROR) {
    FreeMibTable(ipTable);

    return results;
  }

  auto numEntries = ipTable[0].NumEntries;
  auto interfaces = getInterfaceRowMapping();
  auto adapters = getAdapterAddressMapping();

  for (unsigned long i = 0; i < numEntries; ++i) {
    Row r;
    std::string interfaceIpAddress;
    const auto& currentRow = ipTable[0].Table[i];
    auto addrFamily = currentRow.DestinationPrefix.Prefix.si_family;
    auto actualInterface = interfaces.at(currentRow.InterfaceIndex);
    if (addrFamily == AF_INET6) {
      std::vector<char> buffer(INET6_ADDRSTRLEN);

      r["mtu"] = INTEGER(actualInterface.NlMtu);
      // These are all technically "on-link" addresses according to
      // `route print -6`.
      r["type"] = "local";
      auto ipAddress = currentRow.DestinationPrefix.Prefix.Ipv6.sin6_addr;
      auto gateway = currentRow.NextHop.Ipv6.sin6_addr;

      InetNtop(addrFamily, (PVOID)&ipAddress, buffer.data(), buffer.size());
      r["destination"] = SQL_TEXT(buffer.data());
      InetNtop(addrFamily, (PVOID)&gateway, buffer.data(), buffer.size());
      r["gateway"] = SQL_TEXT(buffer.data());
    } else if (addrFamily == AF_INET) {
      std::vector<char> buffer(INET_ADDRSTRLEN);
      auto ipAddress = currentRow.DestinationPrefix.Prefix.Ipv4.sin_addr;
      auto gateway = currentRow.NextHop.Ipv4.sin_addr;

      InetNtop(addrFamily, (PVOID)&ipAddress, buffer.data(), buffer.size());
      r["destination"] = SQL_TEXT(buffer.data());
      buffer.clear();

      // The software loopback is not returned by GetAdaptersInfo, so any
      // lookups into that index must be skipped and default values set.
      IP_ADAPTER_INFO actualAdapter;
      if (currentRow.InterfaceIndex != 1) {
        try {
          actualAdapter = adapters.at(currentRow.InterfaceIndex);
          interfaceIpAddress = actualAdapter.IpAddressList.IpAddress.String;
          r["gateway"] = SQL_TEXT(actualAdapter.GatewayList.IpAddress.String);
          r["mtu"] = INTEGER(actualInterface.NlMtu);
        } catch (const std::out_of_range& oor) {
          LOG(ERROR) << "Error looking up interface "
                     << currentRow.InterfaceIndex;
          LOG(ERROR) << oor.what();
        }
      } else {
        interfaceIpAddress = "127.0.0.1";
        InetNtop(addrFamily, (PVOID)&gateway, buffer.data(), buffer.size());
        r["gateway"] = SQL_TEXT(buffer.data());
        r["mtu"] = UNSIGNED_BIGINT(0xFFFFFFFF);
        buffer.clear();
      }
      r["type"] = currentRow.Loopback ? "local" : "remote";
    }
    r["interface"] = SQL_TEXT(interfaceIpAddress);
    r["metric"] = INTEGER(currentRow.Metric + actualInterface.Metric);
    r["netmask"] =
        SQL_TEXT(std::to_string(currentRow.DestinationPrefix.PrefixLength));
    // TODO: implement routes flags
    r["flags"] = SQL_TEXT("-1");

    results.push_back(r);
  }

  // Cleanup
  FreeMibTable(ipTable);

  return results;
}
}
}