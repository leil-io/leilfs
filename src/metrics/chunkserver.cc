#include "metrics/chunkserver.h"
#include "metrics/utils.h"
#ifdef HAVE_PROMETHEUS
using namespace metrics::chunkserver;

metrics::Chunkserver::Chunkserver(std::shared_ptr<prometheus::Registry>& registry) {
	// clang-format off
	// NOLINTBEGIN(cppcoreguidelines-prefer-member-initializer)
	INITIALIZE_FAMILY(chunkserver, network_bytes_total      , "Number of observed network bytes"                    );
	INITIALIZE_FAMILY(chunkserver, network_operations_total , "Number of chunkserver operations"                    );
	INITIALIZE_FAMILY(chunkserver, max_operations_jobs_total, "Number of max operations in total"                   );
	INITIALIZE_FAMILY(chunkserver, chunk_operations_total   , "Number of chunk operations"                          );
	INITIALIZE_FAMILY(chunkserver, local_bytes_total        , "Number of read/writes on local filesystem"           );
	INITIALIZE_FAMILY(chunkserver, hdd_bytes_total          , "Number of bytes read/written on local filesystem"    );
	INITIALIZE_FAMILY(chunkserver, hdd_operations_total     , "Number of read/write operations on local filesystem" );
	INITIALIZE_FAMILY(chunkserver, replications_total       , "Number of replications"                              );
	// NOLINTEND(cppcoreguidelines-prefer-member-initializer)

	// A very hacky way to allow compile time checking if metrics have been
	// set. Any enum value not used will throw a compile time error.
	START_COUNTER_SWITCH(chunkserver)
	DEFINE_COUNTER_CASE(MASTER_TX_BYTES,                  network_bytes_total,       { {"direction", "tx"}, {"peer", "master"}              });
	DEFINE_COUNTER_CASE(MASTER_RX_BYTES,                  network_bytes_total,       { {"direction", "rx"}, {"peer", "master"}              });
	DEFINE_COUNTER_CASE(ANY_RX_BYTES,                     network_bytes_total,       { {"direction", "rx"}, {"peer", "client_chunkserver"}  });
	DEFINE_COUNTER_CASE(ANY_TX_BYTES,                     network_bytes_total,       { {"direction", "tx"}, {"peer", "client_chunkserver"}  });
	// DEFINE_COUNTER_CASE(MASTER_MAX_OPERATION_JOBS,        max_operations_jobs_total, { {"from", "master"},                                  });
	// DEFINE_COUNTER_CASE(CHUNKSERVER_MAX_SERVER_JOBS,      max_operations_jobs_total, { {"from", "chunkserver"},                             });
	DEFINE_COUNTER_CASE(CHUNKSERVER_HIGH_LEVEL_READ_OPS,  network_operations_total,  { {"level", "high"}, {"operation", "read"}             });
	DEFINE_COUNTER_CASE(CHUNKSERVER_HIGH_LEVEL_WRITE_OPS, network_operations_total,  { {"level", "high"}, {"operation", "write"}            });
	DEFINE_COUNTER_CASE(CHUNKSERVER_LOW_LEVEL_READ_OPS,   network_operations_total,  { {"level", "low"}, {"operation", "read"}              });
	DEFINE_COUNTER_CASE(CHUNKSERVER_LOW_LEVEL_WRITE_OPS,  network_operations_total,  { {"level", "low"}, {"operation", "write"}             });
	DEFINE_COUNTER_CASE(OVERHEAD_BYTES_READ,              local_bytes_total,         { {"overhead", "true"}, {"operation", "read"}          });
	DEFINE_COUNTER_CASE(OVERHEAD_BYTES_WRITE,             local_bytes_total,         { {"overhead", "true"}, {"operation", "write"}         });
	DEFINE_COUNTER_CASE(OVERHEAD_OPERATIONS_READ,         hdd_operations_total,      { {"overhead", "true"}, {"type", "read"}               });
	DEFINE_COUNTER_CASE(OVERHEAD_OPERATIONS_WRITE,        hdd_operations_total,      { {"overhead", "true"}, {"type", "write"}              });
	DEFINE_COUNTER_CASE(HDD_TOTAL_BYTES_READ,             hdd_bytes_total,           { {"type", "read"}                                     });
	DEFINE_COUNTER_CASE(HDD_TOTAL_BYTES_WRITE,            hdd_bytes_total,           { {"type", "write"}                                    });
	DEFINE_COUNTER_CASE(HDD_TOTAL_LOW_LEVEL_READS,        hdd_operations_total,      { {"type", "read"}                                     });
	DEFINE_COUNTER_CASE(HDD_TOTAL_LOW_LEVEL_WRITES,       hdd_operations_total,      { {"type", "write"}                                    });
	DEFINE_COUNTER_CASE(REPLICATIONS,                     replications_total,        {                                                      });
	DEFINE_COUNTER_CASE(OPERATION_CREATE,                 chunk_operations_total,    { {"type", "create"}                                   });
	DEFINE_COUNTER_CASE(OPERATION_DELETE,                 chunk_operations_total,    { {"type", "delete"}                                   });
	DEFINE_COUNTER_CASE(OPERATION_VERSION,                chunk_operations_total,    { {"type", "version"}                                  });
	DEFINE_COUNTER_CASE(OPERATION_DUPLICATE,              chunk_operations_total,    { {"type", "duplicate"}                                });
	DEFINE_COUNTER_CASE(OPERATION_TRUNCATE,               chunk_operations_total,    { {"type", "truncate"}                                 });
	DEFINE_COUNTER_CASE(OPERATION_DUPLICATE_TRUNCATE,     chunk_operations_total,    { {"type", "duplicate_truncate"}                       });
	DEFINE_COUNTER_CASE(OPERATION_TEST,                   chunk_operations_total,    { {"type", "test"}                                     });
	END_COUNTER_CASE
	// clang-format on
}
#endif
