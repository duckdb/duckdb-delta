#!/usr/bin/env bash
#Note: DONT run as root

HTTPFS_SCRIPT_DIR=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)

if [ ! -f test/test_data/attach.db ]; then
    echo "File test/test_data/attach.db not found, run ./scripts/generate_presigned_url.sh to generate"
else
  "${HTTPFS_SCRIPT_DIR}/cleanup_s3_test_server.sh"
  mkdir -p /tmp/minio_test_data
  mkdir -p /tmp/minio_root_data
  docker compose -f "${HTTPFS_SCRIPT_DIR}/minio_s3.yml" -p duckdb-minio up -d

  # for testing presigned url
  container_name=$(docker ps -a --format '{{.Names}}' | grep -m 1 "duckdb-minio")
  echo $container_name

  for i in $(seq 1 360);
  do
    docker_finish_logs=$(docker logs $container_name 2>/dev/null | grep -m 1 'FINISHED SETTING UP MINIO' || echo '')
    if [ ! -z "${docker_finish_logs}" ]; then
      break
    fi
    sleep 1
  done


  export S3_SMALL_CSV_PRESIGNED_URL=$(docker logs $container_name 2>/dev/null | grep -m 1 'Share:.*phonenumbers\.csv' | grep -o 'http[s]\?://[^ ]\+')
  echo $S3_SMALL_CSV_PRESIGNED_URL

  export S3_SMALL_PARQUET_PRESIGNED_URL=$(docker logs $container_name 2>/dev/null | grep -m 1 'Share:.*t1\.parquet' | grep -o 'http[s]\?://[^ ]\+')
  echo $S3_SMALL_PARQUET_PRESIGNED_URL

  export S3_LARGE_PARQUET_PRESIGNED_URL=$(docker logs $container_name 2>/dev/null | grep -m 1 'Share:.*lineitem_large\.parquet' | grep -o 'http[s]\?://[^ ]\+')
  echo $S3_LARGE_PARQUET_PRESIGNED_URL

  export S3_ATTACH_DB_PRESIGNED_URL=$(docker logs $container_name 2>/dev/null | grep -m 1 'Share:.*attach\.db' | grep -o 'http[s]\?://[^ ]\+')
  echo $S3_ATTACH_DB_PRESIGNED_URL

  export S3_ATTACH_DB="s3://test-bucket/presigned/attach.db"
fi
