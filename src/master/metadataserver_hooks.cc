/*
   Copyright 2026      Leil Storage OÜ

   This file is part of SaunaFS.

   SaunaFS is free software: you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation, version 3.

   SaunaFS is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with SaunaFS. If not, see <http://www.gnu.org/licenses/>.
*/

#include "common/platform.h"

#include "master/metadataserver_hooks.h"

#include "master/masterconn.h"
#include "master/matomlserv.h"
#include "master/personality.h"
#include "protocol/SFSCommunication.h"

namespace {

MetadataserverStatusResult defaultMetadataserverStatus() {
	MetadataserverStatusResult result;
	result.status =
	    metadataserver::isMaster()
	        ? SAU_METADATASERVER_STATUS_MASTER
	        : (masterconn_is_connected() ? SAU_METADATASERVER_STATUS_SHADOW_CONNECTED
	                                     : SAU_METADATASERVER_STATUS_SHADOW_DISCONNECTED);
	return result;
}

std::vector<MetadataserverListEntry> defaultMetadataserversList() { return matomlserv_shadows(); }

}  // namespace

MetadataserversListHook gMetadataserversListHook = defaultMetadataserversList;
MetadataserverStatusHook gMetadataserverStatusHook = defaultMetadataserverStatus;
MetadataserverClusterViewHook gMetadataserverClusterViewHook;
ChunkserverSessionRegistrationHook gChunkserverSessionRegistrationHook;
