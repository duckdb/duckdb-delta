#!/bin/bash

# Default Azurite connection string (see: https://github.com/Azure/Azurite)
conn_string="DefaultEndpointsProtocol=http;AccountName=devstoreaccount1;AccountKey=Eby8vdM02xNOcqFlqUwJPLlmEtlCDXJ1OUzFT50uSRZ6IFsuFq2UVErCz4I6tq/K1SZFPTOtr/KBHBeksoGMGw==;BlobEndpoint=http://127.0.0.1:28000/devstoreaccount1;QueueEndpoint=http://127.0.0.1:28001/devstoreaccount1;TableEndpoint=http://127.0.0.1:28002/devstoreaccount1;"

# Create container
az storage container create -n delta-testing-private --connection-string "${conn_string}"
az storage container create -n delta-testing-public  --connection-string "${conn_string}" --public-access blob

copy_file() {
  local from="${1}"
  local to="${2}"
  az storage blob upload --file "${from}" --name "${to}" --container-name "delta-testing-private" --connection-string "${conn_string}"
  az storage blob upload --file "${from}" --name "${to}" --container-name "delta-testing-public"  --connection-string "${conn_string}"
}

cd ./build/release/rust/src/delta_kernel/acceptance/tests/dat/out/reader_tests/generated &&
while read filepath; do
    remote_filepath=dat/"$(echo "${filepath}" | cut -c 3-)"
    copy_file "${filepath}" "${remote_filepath}"
done < <(find . -type f)