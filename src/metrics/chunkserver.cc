#include "metrics/chunkserver.h"
#include "metrics/utils.h"
#ifdef HAVE_PROMETHEUS
using namespace metrics::chunkserver;

metrics::Chunkserver::Chunkserver(std::shared_ptr<prometheus::Registry>& registry) {
	// clang-format off
	// NOLINTBEGIN(cppcoreguidelines-prefer-member-initializer)
	METRICS_INITIALIZE_FAMILY(chunkserver, network_bytes_total      , "Number of observed network bytes"                    );
	METRICS_INITIALIZE_FAMILY(chunkserver, network_operations_total , "Number of chunkserver operations"                    );
	METRICS_INITIALIZE_FAMILY(chunkserver, operations_jobs          , "Number of operations jobs"                           );
	METRICS_INITIALIZE_FAMILY(chunkserver, chunk_operations_total   , "Number of chunk operations"                          );
	METRICS_INITIALIZE_FAMILY(chunkserver, hdd_time_total           , "Total time of read/writes on HDD's"                  );
	METRICS_INITIALIZE_FAMILY(chunkserver, hdd_bytes_total          , "Number of bytes read/written on local filesystem"    );
	METRICS_INITIALIZE_FAMILY(chunkserver, hdd_time_total           , "Number of bytes read/written on local filesystem"    );
	METRICS_INITIALIZE_FAMILY(chunkserver, hdd_operations_total     , "Number of read/write operations on local filesystem" );
	METRICS_INITIALIZE_FAMILY(chunkserver, replications_total       , "Number of replications"                              );
	// NOLINTEND(cppcoreguidelines-prefer-member-initializer)

	// A very hacky way to allow compile time checking if metrics have been
	// set. Any enum value not used will throw a compile time error.
	METRICS_START_COUNTER_SWITCH(chunkserver)
	METRICS_DEFINE_COUNTER_CASE(MASTER_TX_BYTES,                     network_bytes_total,       { {"direction", "tx"}, {"peer", "master"}              });
	METRICS_DEFINE_COUNTER_CASE(MASTER_RX_BYTES,                     network_bytes_total,       { {"direction", "rx"}, {"peer", "master"}              });
	METRICS_DEFINE_COUNTER_CASE(ANY_RX_BYTES,                        network_bytes_total,       { {"direction", "rx"}, {"peer", "client_chunkserver"}  });
	METRICS_DEFINE_COUNTER_CASE(ANY_TX_BYTES,                        network_bytes_total,       { {"direction", "tx"}, {"peer", "client_chunkserver"}  });
	METRICS_DEFINE_COUNTER_CASE(CHUNKSERVER_HIGH_LEVEL_READ_OPS,     network_operations_total,  { {"level", "high"}, {"operation", "read"}             });
	METRICS_DEFINE_COUNTER_CASE(CHUNKSERVER_HIGH_LEVEL_WRITE_OPS,    network_operations_total,  { {"level", "high"}, {"operation", "write"}            });
	METRICS_DEFINE_COUNTER_CASE(CHUNKSERVER_LOW_LEVEL_READ_OPS,      network_operations_total,  { {"level", "low"}, {"operation", "read"}              });
	METRICS_DEFINE_COUNTER_CASE(CHUNKSERVER_LOW_LEVEL_WRITE_OPS,     network_operations_total,  { {"level", "low"}, {"operation", "write"}             });
	METRICS_DEFINE_COUNTER_CASE(HDD_TOTAL_OVERHEAD_BYTES_READ,       hdd_bytes_total,           { {"overhead", "true"}, {"type", "read"}               });
	METRICS_DEFINE_COUNTER_CASE(HDD_TOTAL_OVERHEAD_BYTES_WRITE,      hdd_bytes_total,           { {"overhead", "true"}, {"type", "write"}              });
	METRICS_DEFINE_COUNTER_CASE(HDD_TOTAL_BYTES_READ,                hdd_bytes_total,           { {"type", "read"}                                     });
	METRICS_DEFINE_COUNTER_CASE(HDD_TOTAL_BYTES_WRITE,               hdd_bytes_total,           { {"type", "write"}                                    });
	METRICS_DEFINE_COUNTER_CASE(HDD_TOTAL_TIME_READ,                 hdd_time_total,            { {"type", "read"}                                     });
	METRICS_DEFINE_COUNTER_CASE(HDD_TOTAL_TIME_WRITE,                hdd_time_total,            { {"type", "write"}                                    });
	METRICS_DEFINE_COUNTER_CASE(HDD_TOTAL_READ_OPERATIONS_OVERHEAD,  hdd_operations_total,      { {"overhead", "true"}, {"type", "read"}               });
	METRICS_DEFINE_COUNTER_CASE(HDD_TOTAL_WRITE_OPERATIONS_OVERHEAD, hdd_operations_total,      { {"overhead", "true"}, {"type", "write"}              });
	METRICS_DEFINE_COUNTER_CASE(HDD_TOTAL_READ_OPERATIONS,           hdd_operations_total,      { {"type", "read"}                                     });
	METRICS_DEFINE_COUNTER_CASE(HDD_TOTAL_WRITE_OPERATIONS,          hdd_operations_total,      { {"type", "write"}                                    });
	METRICS_DEFINE_COUNTER_CASE(OPERATION_CREATE,                    chunk_operations_total,    { {"type", "create"}                                   });
	METRICS_DEFINE_COUNTER_CASE(OPERATION_DELETE,                    chunk_operations_total,    { {"type", "delete"}                                   });
	METRICS_DEFINE_COUNTER_CASE(OPERATION_VERSION,                   chunk_operations_total,    { {"type", "version"}                                  });
	METRICS_DEFINE_COUNTER_CASE(OPERATION_DUPLICATE,                 chunk_operations_total,    { {"type", "duplicate"}                                });
	METRICS_DEFINE_COUNTER_CASE(OPERATION_TRUNCATE,                  chunk_operations_total,    { {"type", "truncate"}                                 });
	METRICS_DEFINE_COUNTER_CASE(OPERATION_DUPLICATE_TRUNCATE,        chunk_operations_total,    { {"type", "duplicate_truncate"}                       });
	METRICS_DEFINE_COUNTER_CASE(OPERATION_TEST,                      chunk_operations_total,    { {"type", "test"}                                     });
	METRICS_END_COUNTER_CASE

	METRICS_START_GAUGE_SWITCH(chunkserver)
	METRICS_DEFINE_GAUGE_CASE(MASTER_OPERATION_JOBS,                     operations_jobs,       { {"from", "master"},                                  });
	METRICS_DEFINE_GAUGE_CASE(CHUNKSERVER_OPERATION_JOBS,                operations_jobs,       { {"from", "chunkserver"},                             });
	METRICS_END_GAUGE_CASE
	// clang-format on
}
#endif
