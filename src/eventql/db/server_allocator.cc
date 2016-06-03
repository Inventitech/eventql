/**
 * Copyright (c) 2016 zScale Technology GmbH <legal@zscale.io>
 * Authors:
 *   - Paul Asmuth <paul@zscale.io>
 *
 * This program is free software: you can redistribute it and/or modify it under
 * the terms of the GNU Affero General Public License ("the license") as
 * published by the Free Software Foundation, either version 3 of the License,
 * or any later version.
 *
 * In accordance with Section 7(e) of the license, the licensing of the Program
 * under the license does not imply a trademark license. Therefore any rights,
 * title and interest in our trademarks remain entirely with us.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
 * FOR A PARTICULAR PURPOSE. See the license for more details.
 *
 * You can be released from the requirements of the license by purchasing a
 * commercial license. Buying such a license is mandatory as soon as you develop
 * commercial activities involving this program without disclosing the source
 * code of your own applications
 */
#include "eventql/db/server_allocator.h"
#include "eventql/util/random.h"

namespace eventql {

ServerAllocator::ServerAllocator(ConfigDirectory* cdir) : cdir_(cdir) {}

Status ServerAllocator::allocateServers(
    size_t num_servers,
    Set<String>* servers) const {
  size_t num_alloced = 0;
  auto all_servers = cdir_->listServers();
  uint64_t idx = Random::singleton()->random64();
  for (int i = 0; i < all_servers.size(); ++i) {
    const auto& s = all_servers[++idx % all_servers.size()];

    if (s.is_dead() || s.is_leaving() || s.server_status() != SERVER_UP) {
      continue;
    }

    if (servers->count(s.server_id()) > 0) {
      continue;
    }

    servers->emplace(s.server_id());
    if (++num_alloced == num_servers) {
      break;
    }
  }

  if (num_alloced < num_servers) {
    return Status(eRuntimeError, "not enough live servers");
  }

  return Status::success();
}

} // namespace eventql

