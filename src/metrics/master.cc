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

	METRICS_INITIALIZE_FAMILY(metadata, packets_client_total  , "Number of observed packets from and for client" );
	METRICS_INITIALIZE_FAMILY(metadata, bytes_client_total    , "Number of observed bytes from and for client"   );
	METRICS_INITIALIZE_FAMILY(metadata, filesystem_stats_total, "Number of observed filesystem operations"       );
	METRICS_INITIALIZE_FAMILY(metadata, chunk_operations_total, "Number of chunk operations"                     );

	// NOLINTEND(cppcoreguidelines-prefer-member-initializer)

	METRICS_START_COUNTER_SWITCH(metadata)

	METRICS_DEFINE_COUNTER_CASE(CHUNK_DELETE     , chunk_operations_total, { {"chunk", "operations"}, {"operation", "delete"}        });
	METRICS_DEFINE_COUNTER_CASE(CHUNK_REPLICATE  , chunk_operations_total, { {"chunk", "operations"}, {"operation", "replicate"}     });
	METRICS_DEFINE_COUNTER_CASE(FS_STATFS        , filesystem_stats_total, { {"filesystem", "operations"}, {"operation", "STATFS"}   });
	METRICS_DEFINE_COUNTER_CASE(FS_GETATTR       , filesystem_stats_total, { {"filesystem", "operations"}, {"operation", "GETATTR"}  });
	METRICS_DEFINE_COUNTER_CASE(FS_SETATTR       , filesystem_stats_total, { {"filesystem", "operations"}, {"operation", "SETATTR"}  });
	METRICS_DEFINE_COUNTER_CASE(FS_LOOKUP        , filesystem_stats_total, { {"filesystem", "operations"}, {"operation", "LOOKUP"}   });
	METRICS_DEFINE_COUNTER_CASE(FS_MKDIR         , filesystem_stats_total, { {"filesystem", "operations"}, {"operation", "MKDIR"}    });
	METRICS_DEFINE_COUNTER_CASE(FS_RMDIR         , filesystem_stats_total, { {"filesystem", "operations"}, {"operation", "RMDIR"}    });
	METRICS_DEFINE_COUNTER_CASE(FS_SYMLINK       , filesystem_stats_total, { {"filesystem", "operations"}, {"operation", "SYMLINK"}  });
	METRICS_DEFINE_COUNTER_CASE(FS_READLINK      , filesystem_stats_total, { {"filesystem", "operations"}, {"operation", "READLINK"} });
	METRICS_DEFINE_COUNTER_CASE(FS_MKNOD         , filesystem_stats_total, { {"filesystem", "operations"}, {"operation", "MKNOD"}    });
	METRICS_DEFINE_COUNTER_CASE(FS_UNLINK        , filesystem_stats_total, { {"filesystem", "operations"}, {"operation", "UNLINK"}   });
	METRICS_DEFINE_COUNTER_CASE(FS_RENAME        , filesystem_stats_total, { {"filesystem", "operations"}, {"operation", "RENAME"}   });
	METRICS_DEFINE_COUNTER_CASE(FS_LINK          , filesystem_stats_total, { {"filesystem", "operations"}, {"operation", "LINK"}     });
	METRICS_DEFINE_COUNTER_CASE(FS_READDIR       , filesystem_stats_total, { {"filesystem", "operations"}, {"operation", "READDIR"}  });
	METRICS_DEFINE_COUNTER_CASE(FS_OPEN          , filesystem_stats_total, { {"filesystem", "operations"}, {"operation", "OPEN"}     });
	METRICS_DEFINE_COUNTER_CASE(FS_READ          , filesystem_stats_total, { {"filesystem", "operations"}, {"operation", "READ"}     });
	METRICS_DEFINE_COUNTER_CASE(FS_WRITE         , filesystem_stats_total, { {"filesystem", "operations"}, {"operation", "WRITE"}    });
	METRICS_DEFINE_COUNTER_CASE(CLIENT_RX_PACKETS, packets_client_total  , { {"protocol", "tcp"}, {"direction", "rx"}                });
	METRICS_DEFINE_COUNTER_CASE(CLIENT_TX_PACKETS, packets_client_total  , { {"protocol", "tcp"}, {"direction", "tx"}                });
	METRICS_DEFINE_COUNTER_CASE(CLIENT_RX_BYTES  , bytes_client_total    , { {"protocol", "tcp"}, {"direction", "rx"}                });
	METRICS_DEFINE_COUNTER_CASE(CLIENT_TX_BYTES  , bytes_client_total    , { {"protocol", "tcp"}, {"direction", "tx"}                });

	METRICS_END_COUNTER_CASE
	// clang-format on
}
#endif
