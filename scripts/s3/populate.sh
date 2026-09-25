#!/usr/bin/env bash
# Creates the test buckets, opens the public one to anonymous reads and uploads the DAT tables to the
# local S3 test server. Run through scripts/env_s3.
set -euo pipefail

s3() { aws --endpoint-url "http://$AWS_ENDPOINT" s3 "$@"; }

for _ in $(seq 60); do
  s3 ls >/dev/null 2>&1 && break
  echo 'waiting for the S3 test server...'
  sleep 1
done

DAT=${DAT:-build/release/rust/src/delta_kernel/acceptance/tests/dat/out/reader_tests/generated}
for bucket in test-bucket test-bucket-public; do
  s3 mb "s3://$bucket"
  s3 cp --recursive --quiet "$DAT" "s3://$bucket/dat"
done

aws --endpoint-url "http://$AWS_ENDPOINT" s3api put-bucket-policy --bucket test-bucket-public --policy '{
  "Version": "2012-10-17",
  "Statement": [{
    "Effect": "Allow",
    "Principal": {"AWS": ["*"]},
    "Action": ["s3:GetObject", "s3:ListBucket"],
    "Resource": ["arn:aws:s3:::test-bucket-public", "arn:aws:s3:::test-bucket-public/*"]
  }]
}'
