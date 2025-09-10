timeout_set 2 minutes
WAIT_FOR_REPLICATION=60
NUMBER_OF_CHUNKSERVERS=10
GOALS_TO_BE_TESTED="ec21 2 ec22 3 ec31 5 ec32 6 ec33 4 ec43 9 ec44"
VERIFY_FILE_CONTENT=YES

source test_suites/TestTemplates/test_chunk_type_conversion.inc
