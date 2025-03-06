/*

   Copyright 2024 Leil Storage OÜ

   This file is part of SaunaFS.

   SaunaFS is free software: you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation, version 3.

   SaunaFS is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with SaunaFS. If not, see <http://www.gnu.org/licenses/>.
 */

#include "metrics/master.h"
#include "metrics/utils.h"

#ifdef HAVE_PROMETHEUS
using namespace metrics;

Master::Master(std::shared_ptr<prometheus::Registry> &registry) {
	// clang-format off
	// NOLINTBEGIN(cppcoreguidelines-prefer-member-initializer)

	INITIALIZE_FAMILY(metadata, packets_client_total  , "Number of observed packets from and for client" );
	INITIALIZE_FAMILY(metadata, bytes_client_total    , "Number of observed bytes from and for client"   );
	INITIALIZE_FAMILY(metadata, filesystem_stats_total, "Number of observed filesystem operations"       );
	INITIALIZE_FAMILY(metadata, chunk_operations_total, "Number of chunk operations"                     );

	// NOLINTEND(cppcoreguidelines-prefer-member-initializer)

	START_COUNTER_SWITCH(metadata)

	DEFINE_COUNTER_CASE(CHUNK_DELETE     , chunk_operations_total, { {"chunk", "operations"}, {"operation", "delete"}        });
	DEFINE_COUNTER_CASE(CHUNK_REPLICATE  , chunk_operations_total, { {"chunk", "operations"}, {"operation", "replicate"}     });
	DEFINE_COUNTER_CASE(FS_STATFS        , filesystem_stats_total, { {"filesystem", "operations"}, {"operation", "STATFS"}   });
	DEFINE_COUNTER_CASE(FS_GETATTR       , filesystem_stats_total, { {"filesystem", "operations"}, {"operation", "GETATTR"}  });
	DEFINE_COUNTER_CASE(FS_SETATTR       , filesystem_stats_total, { {"filesystem", "operations"}, {"operation", "SETATTR"}  });
	DEFINE_COUNTER_CASE(FS_LOOKUP        , filesystem_stats_total, { {"filesystem", "operations"}, {"operation", "LOOKUP"}   });
	DEFINE_COUNTER_CASE(FS_MKDIR         , filesystem_stats_total, { {"filesystem", "operations"}, {"operation", "MKDIR"}    });
	DEFINE_COUNTER_CASE(FS_RMDIR         , filesystem_stats_total, { {"filesystem", "operations"}, {"operation", "RMDIR"}    });
	DEFINE_COUNTER_CASE(FS_SYMLINK       , filesystem_stats_total, { {"filesystem", "operations"}, {"operation", "SYMLINK"}  });
	DEFINE_COUNTER_CASE(FS_READLINK      , filesystem_stats_total, { {"filesystem", "operations"}, {"operation", "READLINK"} });
	DEFINE_COUNTER_CASE(FS_MKNOD         , filesystem_stats_total, { {"filesystem", "operations"}, {"operation", "MKNOD"}    });
	DEFINE_COUNTER_CASE(FS_UNLINK        , filesystem_stats_total, { {"filesystem", "operations"}, {"operation", "UNLINK"}   });
	DEFINE_COUNTER_CASE(FS_RENAME        , filesystem_stats_total, { {"filesystem", "operations"}, {"operation", "RENAME"}   });
	DEFINE_COUNTER_CASE(FS_LINK          , filesystem_stats_total, { {"filesystem", "operations"}, {"operation", "LINK"}     });
	DEFINE_COUNTER_CASE(FS_READDIR       , filesystem_stats_total, { {"filesystem", "operations"}, {"operation", "READDIR"}  });
	DEFINE_COUNTER_CASE(FS_OPEN          , filesystem_stats_total, { {"filesystem", "operations"}, {"operation", "OPEN"}     });
	DEFINE_COUNTER_CASE(FS_READ          , filesystem_stats_total, { {"filesystem", "operations"}, {"operation", "READ"}     });
	DEFINE_COUNTER_CASE(FS_WRITE         , filesystem_stats_total, { {"filesystem", "operations"}, {"operation", "WRITE"}    });
	DEFINE_COUNTER_CASE(CLIENT_RX_PACKETS, packets_client_total  , { {"protocol", "tcp"}, {"direction", "rx"}                });
	DEFINE_COUNTER_CASE(CLIENT_TX_PACKETS, packets_client_total  , { {"protocol", "tcp"}, {"direction", "tx"}                });
	DEFINE_COUNTER_CASE(CLIENT_RX_BYTES  , bytes_client_total    , { {"protocol", "tcp"}, {"direction", "rx"}                });
	DEFINE_COUNTER_CASE(CLIENT_TX_BYTES  , bytes_client_total    , { {"protocol", "tcp"}, {"direction", "tx"}                });

	END_COUNTER_CASE
	// clang-format on
}
#endif
